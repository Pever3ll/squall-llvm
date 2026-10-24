// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
// Native LLVM IR -> registerless Squall MCInst lowering. The serial cell-memory
// ABI deliberately matches the independently implemented Python baseline.
#include "SquallLowering.h"
#include "MCTargetDesc/SquallMCTargetDesc.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/MC/MCAsmBackend.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCCodeEmitter.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCInstPrinter.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCObjectFileInfo.h"
#include "llvm/MC/MCObjectWriter.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSectionELF.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FormattedStream.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/SHA256.h"
#include "llvm/Target/TargetMachine.h"
#include <map>
using namespace llvm;
namespace {
cl::opt<std::string> Entry("squall-entry",
                           cl::desc("Squall image entry function"),
                           cl::init("main"));
cl::list<std::string> Args("squall-arg",
                           cl::desc("Entry integer argument (repeatable)"));
cl::opt<uint64_t> MemoryBytes("squall-memory-bytes", cl::init(512 * 1024));
cl::opt<unsigned> Frames("squall-frames", cl::init(32));
cl::opt<unsigned> StackTokens("squall-stack-tokens", cl::init(1024));
// Unsupported IR/layout requests are compilation errors, not compiler crashes.
[[noreturn]] void fail(const Twine &S) {
  report_fatal_error(Twine("Squall: ") + S, false);
}
unsigned width(Type *T) {
  if (T->isPointerTy()) {
    if (T->getPointerAddressSpace())
      fail("nondefault address spaces are unsupported");
    return 32;
  }
  if (!T->isIntegerTy(1) && !T->isIntegerTy(32))
    fail("supported values are i1, i32 and ptr");
  return T->getIntegerBitWidth();
}
unsigned cells(Type *T) {
  if (T->isIntegerTy(32) || (T->isPointerTy() && !T->getPointerAddressSpace()))
    return 1;
  if (auto *A = dyn_cast<ArrayType>(T)) {
    uint64_t N = A->getNumElements(), E = cells(A->getElementType());
    if (!N || N > MemoryBytes / 8 / E)
      fail("array exceeds the available memory arena");
    return N * E;
  }
  fail("memory supports fixed arrays of aligned i32/ptr cells");
}
uint64_t slot(int64_t V) {
  if (!isInt<62>(V))
    fail("constant payload exceeds signed 62 bits");
  return (UINT64_C(2) << 62) | (uint64_t(V) & ((UINT64_C(1) << 62) - 1));
}
int64_t canonical(int64_t V, unsigned W) {
  return W == 1 ? V & 1 : int64_t(int32_t(V));
}
struct Ref {
  std::string Name = "halt";
  unsigned Port = 0;
};
struct Addr {
  unsigned Mode = 0;
  int64_t Offset = 0;
  std::string Label;
};
Addr frame(unsigned N) { return {0, int64_t(N), {}}; }
Addr absolute(unsigned N) { return {2, int64_t(N), {}}; }
Addr code(std::string L) { return {1, 0, std::move(L)}; }
struct Record {
  std::string Name;
  unsigned Kind = Squall::ALUI;
  int Op = 26;
  Ref Dest;
  int Second = -1;
  Addr Address;
  int Rule = 2;
  int64_t Value = 0;
  Ref Target;
  bool Data = false;
};
struct FunctionState {
  Function *F;
  unsigned Index, Count = 1, Scratch = 0, GepScratch = 0;
  DenseMap<const Value *, unsigned> Slots;
  DenseMap<const Value *, Addr> Allocas;
  DenseMap<const BasicBlock *, std::string> Blocks;
  DenseMap<const Instruction *, std::string> Instructions;
  SmallVector<unsigned> PhiScratch;
  std::string entry() const { return "fn" + utostr(Index); }
  unsigned allocate(const Value *V = nullptr, unsigned N = 1,
                    unsigned Alignment = 1) {
    Count = alignTo(Count, Alignment);
    unsigned At = Count;
    Count += N;
    if (V)
      Slots[V] = At;
    return At;
  }
};
class Lowering {
  Module &M;
  TargetMachine &TM;
  raw_pwrite_stream &Out;
  CodeGenFileType FileType;
  std::vector<Record> Records;
  std::vector<std::unique_ptr<FunctionState>> Functions;
  DenseMap<const Function *, FunctionState *> FunctionMap;
  DenseMap<const GlobalVariable *, unsigned> Globals;
  std::map<uint64_t, uint64_t> Initial;
  std::map<std::string, uint64_t> Labels;
  SmallVector<int64_t> Arguments;
  SmallVector<unsigned> Bootstrap;
  FunctionState *Context = nullptr;
  unsigned Serial = 0, GlobalNext = 16, DataBase = 0, DataNext = 0,
           FrameBase = 0, Stride = 0, Alignment = 1;
  bool MemoryHelpers = false;
  uint64_t StackBase = 0, StackTop = 0;
  std::string unique(StringRef S = "v") { return (S + Twine(++Serial)).str(); }
  Ref add(Record R, unsigned Port = 0) {
    if (R.Name.empty())
      R.Name = unique();
    Ref D{R.Name, Port};
    Records.push_back(std::move(R));
    return D;
  }
  Ref alu(int Op, Ref D, int64_t V = 0, int Second = -1) {
    Record R;
    R.Op = Op;
    R.Dest = D;
    R.Value = V;
    R.Second = Second;
    return add(R);
  }
  Ref dyadic(int Op, Addr A, Ref D, int Rule = 2, unsigned Port = 0) {
    Record R;
    R.Kind = Squall::ALUS;
    R.Op = Op;
    R.Address = A;
    R.Dest = D;
    R.Rule = Rule;
    return add(R, Port);
  }
  Ref send(Addr A, Ref D = {}, unsigned Port = 0) {
    Record R;
    R.Kind = Squall::SEND;
    R.Address = A;
    R.Dest = D;
    return add(R, Port);
  }
  Ref extract(Ref T, Ref D, Addr A = frame(0)) {
    Record R;
    R.Kind = Squall::EXTRACT;
    R.Target = T;
    R.Dest = D;
    R.Address = A;
    return add(R);
  }
  void jump(std::string Name, Ref D) {
    Record R;
    R.Name = Name;
    R.Op = 26;
    R.Dest = D;
    add(R);
  }
  void data(std::string Name, int64_t V) {
    Record R;
    R.Name = Name;
    R.Data = true;
    R.Value = slot(V);
    add(R);
  }
  Ref constant(int64_t V, Ref D) { return alu(27, D, V); }
  Ref readSlot(Addr A, Ref D) { return dyadic(27, A, D); }
  Ref storeSlot(Addr A, Ref D) { return dyadic(27, A, D, 1, 1); }
  Ref normalize(unsigned W, Ref D, bool Unsigned = false) {
    return W == 1 ? alu(16, D, 1) : alu(19, alu(Unsigned ? 20 : 21, D, 32), 32);
  }
  std::optional<Addr> staticPointer(const Value *V) {
    auto I = Context->Allocas.find(V);
    if (I != Context->Allocas.end())
      return I->second;
    if (auto *G = dyn_cast<GlobalVariable>(V)) {
      auto P = Globals.find(G);
      if (P != Globals.end() && P->second < 2048)
        return absolute(P->second);
    }
    return {};
  }
  Ref read(const Value *V, Ref D) {
    if (isa<ConstantPointerNull>(V))
      return constant(0, D);
    if (auto *G = dyn_cast<GlobalVariable>(V)) {
      if (!Globals.count(G))
        fail("unknown global");
      return constant(int64_t(Globals[G]) * 4, D);
    }
    if (auto A = staticPointer(V)) {
      if (A->Mode == 2)
        return constant(A->Offset * 4, D);
      Ref Add = alu(4, alu(19, D, 2), A->Offset);
      return extract({}, alu(20, Add, 29));
    }
    if (auto *C = dyn_cast<ConstantInt>(V))
      return constant(canonical(C->getSExtValue(), width(V->getType())), D);
    auto I = Context->Slots.find(V);
    if (I == Context->Slots.end())
      fail("unsupported operand (undef, poison, constant expression or "
           "external symbol)");
    return readSlot(frame(I->second), D);
  }
  Ref save(const Value *V, Ref D) {
    return normalize(width(V->getType()),
                     storeSlot(frame(Context->Slots.lookup(V)), D),
                     V->getType()->isPointerTy());
  }
  Ref literal(int64_t V, int Op, Ref D) {
    std::string L = unique(), C = unique("constant");
    Record R;
    R.Name = L;
    R.Kind = Squall::ALUS;
    R.Op = Op;
    R.Address = code(C);
    R.Dest = D;
    add(R);
    data(C, V);
    return {L, 0};
  }
  Ref secondary(int Op, Ref D, std::optional<Addr> A = {}, int64_t V = 0) {
    std::string L = unique(), Capture = unique("secondary"),
                Resume = unique("resume");
    Record R;
    R.Name = L;
    R.Kind = A ? Squall::ALUS : Squall::ALUI;
    R.Op = Op;
    R.Dest = {Resume, 0};
    R.Second = 1;
    R.Value = V;
    if (A)
      R.Address = *A;
    add(R);
    R = {};
    R.Name = Capture;
    R.Kind = Squall::ALUS;
    R.Op = 27;
    R.Address = frame(Context->Scratch);
    R.Rule = 1;
    add(R);
    R = {};
    R.Name = Resume;
    R.Kind = Squall::ALUS;
    R.Op = 27;
    R.Address = frame(Context->Scratch);
    R.Dest = D;
    add(R);
    return {L, 0};
  }
  Ref branch(Ref Yes, Ref No) {
    std::string L = unique(), Stub = unique("false");
    Record R;
    R.Name = L;
    R.Op = 32;
    R.Dest = Yes;
    R.Second = 0;
    add(R);
    jump(Stub, No);
    return {L, 0};
  }
  Ref operands(const Value *A, const Value *B, Ref Action,
               bool Unsigned = false, bool Flip = false) {
    int64_t Sign = width(A->getType()) == 1 ? 1 : INT32_MIN;
    if (Flip)
      Action = alu(18, Action, Sign);
    if (Unsigned)
      Action = normalize(32, Action, true);
    Ref First = read(A, Action),
        Store = storeSlot(frame(Context->Scratch), First);
    if (Flip)
      Store = alu(18, Store, Sign);
    if (Unsigned)
      Store = normalize(32, Store, true);
    return read(B, Store);
  }
  Ref binary(BinaryOperator &I, Ref D) {
    int Op;
    switch (I.getOpcode()) {
    case Instruction::Add:
      Op = 4;
      break;
    case Instruction::Sub:
      Op = 5;
      break;
    case Instruction::Mul:
      Op = 6;
      break;
    case Instruction::SDiv:
      Op = 7;
      break;
    case Instruction::UDiv:
      Op = 8;
      break;
    case Instruction::SRem:
    case Instruction::URem:
      Op = 9;
      break;
    case Instruction::And:
      Op = 16;
      break;
    case Instruction::Or:
      Op = 17;
      break;
    case Instruction::Xor:
      Op = 18;
      break;
    case Instruction::Shl:
      Op = 19;
      break;
    case Instruction::LShr:
      Op = 20;
      break;
    case Instruction::AShr:
      Op = 21;
      break;
    default:
      fail("unsupported binary operation");
    }
    return operands(I.getOperand(0), I.getOperand(1),
                    dyadic(Op, frame(Context->Scratch), D),
                    I.getOpcode() == Instruction::UDiv ||
                        I.getOpcode() == Instruction::URem ||
                        I.getOpcode() == Instruction::LShr);
  }
  Ref compare(ICmpInst &I, Ref D) {
    auto P = I.getPredicate();
    const Value *A = I.getOperand(0), *B = I.getOperand(1);
    if (P == CmpInst::ICMP_EQ || P == CmpInst::ICMP_NE) {
      Ref Yes = constant(1, D), No = constant(0, D);
      std::string L = unique(), Stub = unique("equal");
      Record R;
      R.Name = L;
      R.Kind = Squall::ALUS;
      R.Op = 33;
      R.Address = frame(Context->Scratch);
      R.Dest = P == CmpInst::ICMP_NE ? Yes : No;
      R.Second = 0;
      add(R);
      jump(Stub, P == CmpInst::ICMP_NE ? No : Yes);
      return operands(A, B, {L, 0});
    }
    if (P == CmpInst::ICMP_UGT || P == CmpInst::ICMP_ULE ||
        P == CmpInst::ICMP_SGT || P == CmpInst::ICMP_SLE)
      std::swap(A, B);
    if (P == CmpInst::ICMP_ULE || P == CmpInst::ICMP_UGE ||
        P == CmpInst::ICMP_SLE || P == CmpInst::ICMP_SGE)
      D = alu(18, D, 1);
    return operands(A, B, secondary(5, D, frame(Context->Scratch)), false,
                    I.isSigned());
  }
  void initialize(Type *T, const Constant *C, unsigned &At) {
    if (auto *A = dyn_cast<ArrayType>(T)) {
      for (unsigned N = 0; N < A->getNumElements(); ++N)
        initialize(A->getElementType(), C->getAggregateElement(N), At);
      return;
    }
    if (isa<ConstantPointerNull>(C)) {
      Initial[At++] = slot(0);
      return;
    }
    if (auto *I = dyn_cast<ConstantInt>(C)) {
      Initial[At++] = slot(canonical(I->getSExtValue(), 32));
      return;
    }
    fail("global storage requires an integer or null pointer initializer");
  }
  void layout() {
    if (MemoryBytes < 4096 || MemoryBytes % 16 ||
        MemoryBytes > UINT64_C(1) << 32 || !Frames || !StackTokens)
      fail("invalid memory/frame/token capacity");
    auto storageAlignment = [&](unsigned N) {
      if (!isPowerOf2_32(N) || N > 4096)
        fail("storage alignment must be a power of two up to 4096");
      return std::max(1U, N / 4);
    };
    for (auto &G : M.globals())
      Alignment = std::max(Alignment,
                           storageAlignment(G.getAlign().valueOrOne().value()));
    for (auto &F : M)
      for (auto &B : F)
        for (auto &I : B)
          if (auto *A = dyn_cast<AllocaInst>(&I))
            Alignment =
                std::max(Alignment, storageAlignment(A->getAlign().value()));
    FrameBase = alignTo(MemoryBytes / 32, Alignment);
    DataBase = DataNext = FrameBase / 2;
    for (auto &G : M.globals()) {
      if (!G.hasInitializer() || G.isThreadLocal() || G.getAddressSpace())
        fail("globals require defined non-TLS storage in address space zero");
      unsigned N = cells(G.getValueType()),
               A = storageAlignment(G.getAlign().valueOrOne().value()), At;
      if (G.getValueType()->isArrayTy()) {
        DataNext = alignTo(DataNext, A);
        At = DataNext;
        DataNext += N;
      } else {
        GlobalNext = alignTo(GlobalNext, A);
        At = GlobalNext++;
      }
      if (DataNext > FrameBase || GlobalNext > 256)
        fail("global storage overlaps code or frames");
      Globals[&G] = At;
      initialize(G.getValueType(), G.getInitializer(), At);
    }
    for (auto &F : M) {
      if (F.isDeclaration())
        continue;
      if (F.isVarArg())
        fail("variadic functions are unsupported");
      if (!F.getReturnType()->isVoidTy())
        width(F.getReturnType());
      auto C = std::make_unique<FunctionState>();
      C->F = &F;
      C->Index = Functions.size();
      for (auto &A : F.args()) {
        width(A.getType());
        C->allocate(&A);
      }
      unsigned BI = 0, MaxPhi = 0;
      for (auto &B : F) {
        C->Blocks[&B] = "f" + utostr(C->Index) + "_b" + utostr(BI);
        unsigned II = 0, Phis = 0;
        for (auto &I : B) {
          C->Instructions[&I] = C->Blocks[&B] + "_i" + utostr(II++);
          if (auto *A = dyn_cast<AllocaInst>(&I)) {
            width(A->getType());
            if (BI)
              fail("alloca outside the entry block is unsupported");
            auto *N = dyn_cast<ConstantInt>(A->getArraySize());
            unsigned E = cells(A->getAllocatedType());
            if (!N || N->isZero() || N->getValue().getLimitedValue() > 1024 / E)
              fail("alloca requires a constant size of at most 1024 cells");
            C->Allocas[A] =
                frame(C->allocate(nullptr, E * N->getZExtValue(),
                                  storageAlignment(A->getAlign().value())));
          } else if (!I.getType()->isVoidTy()) {
            width(I.getType());
            C->allocate(&I);
          }
          if (isa<GetElementPtrInst>(I) || isa<MemIntrinsic>(I))
            C->GepScratch = 1;
          if (isa<PHINode>(I))
            ++Phis;
        }
        MaxPhi = std::max(MaxPhi, Phis);
        ++BI;
      }
      C->Scratch = C->allocate();
      if (C->GepScratch)
        C->GepScratch = C->allocate();
      for (unsigned N = 0; N < MaxPhi; ++N)
        C->PhiScratch.push_back(C->allocate());
      FunctionMap[&F] = C.get();
      Functions.push_back(std::move(C));
    }
    Function *F = M.getFunction(Entry);
    if (!F || !FunctionMap.count(F))
      fail("entry function has no definition");
    if (F->arg_size() != Arguments.size())
      fail("entry argument count does not match -squall-arg options");
    if (StackTokens < std::max<uint64_t>(2, Arguments.size()))
      fail("token stack needs at least two slots and capacity for all "
           "arguments");
    for (auto [A, V] : zip(F->args(), Arguments)) {
      if (A.getType()->isPointerTy())
        fail("host pointer entry arguments are unsupported");
      if (width(A.getType()) == 1 ? V != 0 && V != 1
                                  : V < INT32_MIN || V > UINT32_MAX)
        fail("entry argument does not fit its type");
    }
    unsigned MaxCount = Alignment;
    for (auto &C : Functions)
      MaxCount = std::max(MaxCount, C->Count);
    Stride = PowerOf2Ceil(MaxCount);
    if (Stride > 1024)
      fail("activation frame exceeds 1024 words");
    StackBase = MemoryBytes - 16;
    if (uint64_t(StackTokens) * 16 > StackBase ||
        (uint64_t(FrameBase) + uint64_t(Frames) * Stride) * 8 >
            StackBase - uint64_t(StackTokens) * 16)
      fail("frame arena overlaps the token stack");
    for (unsigned F = 0; F < Frames; ++F)
      for (unsigned S = 0; S < Stride; ++S)
        Initial[FrameBase + F * Stride + S] = slot(0);
  }
  Ref memory(const Value *Pointer, Ref D, const Value *Value = nullptr,
             std::optional<Addr> Saved = {},
             std::optional<int64_t> Literal = {}, unsigned Offset = 0) {
    MemoryHelpers = true;
    for (unsigned I = 3; I <= 6; ++I)
      Initial[I] = slot(0);
    std::string Helper = unique("memory");
    Ref Return = send(absolute(3), {}, 1);
    Record R;
    R.Name = Helper;
    R.Kind = Squall::ALUS;
    R.Op = 27;
    bool Store = Value || Saved || Literal;
    if (Store) {
      R.Address = absolute(4);
      R.Dest = storeSlot(frame(0), Return);
    } else {
      R.Address = frame(0);
      R.Dest = Return;
    }
    add(R);
    Ref Send = send(absolute(4));
    Ref Shift = alu(19, dyadic(17, absolute(5), Send), 27);
    uint64_t Limit = MemoryBytes / 2;
    Ref ValidHigh = branch(readSlot(absolute(6), Shift), {"memory_error", 0});
    Ref Compare;
    if (Limit < UINT64_C(1) << 31)
      Compare = secondary(5, ValidHigh, {}, Limit);
    else {
      std::string L = unique("memory_limit");
      data(L, Limit);
      Compare = secondary(5, ValidHigh, code(L));
    }
    Ref High = readSlot(absolute(6), Compare);
    Ref ValidLow = branch({"memory_error", 0}, High);
    Ref Low = readSlot(absolute(6), secondary(5, ValidLow, {}, 64));
    Ref Aligned = alu(16, branch({"memory_error", 0}, Low), 3);
    Ref Setup = storeSlot(absolute(6), Aligned);
    if (Offset)
      Setup = alu(4, Setup, Offset);
    Setup = read(Pointer, Setup);
    Setup = extract({Helper, 0}, alu(16, storeSlot(absolute(5), Setup),
                                     (INT64_C(1) << 29) - 1));
    if (Store) {
      Ref S = storeSlot(absolute(4), Setup);
      Setup = Value   ? read(Value, S)
              : Saved ? readSlot(*Saved, S)
                      : constant(*Literal, S);
    }
    return extract(D, storeSlot(absolute(3), Setup));
  }
  Ref gep(GetElementPtrInst &I, Ref D) {
    SmallVector<std::pair<const Value *, unsigned>> Steps;
    Type *T = I.getSourceElementType();
    bool First = true;
    for (auto &Index : I.indices()) {
      if (!Index->getType()->isIntegerTy(32))
        fail("GEP indices must be i32");
      if (!First) {
        auto *A = dyn_cast<ArrayType>(T);
        if (!A)
          fail("GEP can only index fixed cell arrays");
        T = A->getElementType();
      }
      Steps.push_back({Index.get(), cells(T) * 4});
      First = false;
    }
    Addr Scratch = frame(Context->GepScratch);
    Ref Chain = read(I.getPointerOperand(), dyadic(4, Scratch, D));
    for (auto [V, Scale] : reverse(Steps))
      Chain =
          read(V, alu(6, dyadic(4, Scratch, storeSlot(Scratch, Chain)), Scale));
    return constant(0, storeSlot(Scratch, Chain));
  }
  Ref edge(const BasicBlock *Before, const BasicBlock *After) {
    SmallVector<const PHINode *> Phis;
    for (auto &P : After->phis())
      Phis.push_back(&P);
    Ref D{Context->Blocks.lookup(After), 0};
    for (unsigned N = Phis.size(); N--;)
      D = readSlot(frame(Context->PhiScratch[N]),
                   storeSlot(frame(Context->Slots.lookup(Phis[N])), D));
    for (unsigned N = Phis.size(); N--;)
      D = read(Phis[N]->getIncomingValueForBlock(Before),
               storeSlot(frame(Context->PhiScratch[N]), D));
    return D;
  }
  Ref call(CallInst &I, Ref D) {
    if (auto *II = dyn_cast<IntrinsicInst>(&I)) {
      if (II->getIntrinsicID() == Intrinsic::lifetime_start ||
          II->getIntrinsicID() == Intrinsic::lifetime_end)
        return D;
      auto *MI = dyn_cast<MemIntrinsic>(&I);
      if (!MI || (!isa<MemCpyInst>(MI) && !isa<MemSetInst>(MI)))
        fail("unsupported intrinsic");
      auto *Len = dyn_cast<ConstantInt>(MI->getLength());
      if (!Len || Len->getValue().getLimitedValue() > 1024 ||
          Len->getZExtValue() % 4 || MI->isVolatile())
        fail("memory intrinsic needs constant nonvolatile length, multiple of "
             "four up to 1024");
      if (MI->getDestAlign().valueOrOne() < Align(4))
        fail("memory intrinsic requires explicit four-byte alignment");
      auto *Copy = dyn_cast<MemCpyInst>(MI);
      auto *Set = dyn_cast<MemSetInst>(MI);
      if (Copy && Copy->getSourceAlign().valueOrOne() < Align(4))
        fail("memcpy source requires four-byte alignment");
      auto *Byte = Set ? dyn_cast<ConstantInt>(Set->getValue()) : nullptr;
      if (Set && !Byte)
        fail("memset fill must be constant");
      for (unsigned Offset = Len->getZExtValue(); Offset;) {
        Offset -= 4;
        if (Copy) {
          Addr Saved = frame(Context->GepScratch);
          D = memory(MI->getDest(), D, nullptr, Saved, {}, Offset);
          D = memory(Copy->getSource(), storeSlot(Saved, D), nullptr, {}, {},
                     Offset);
        } else
          D = memory(
              MI->getDest(), D, nullptr, {},
              int64_t(int32_t((Byte->getZExtValue() & 255) * 0x01010101)),
              Offset);
      }
      return D;
    }
    if (I.isMustTailCall())
      fail("musttail calls are unsupported");
    Function *F = I.getCalledFunction();
    if (!F || !FunctionMap.count(F))
      fail("external/indirect calls are unsupported");
    FunctionState *Target = FunctionMap[F];
    Ref Send = send(frame(0));
    Ref Jump =
        extract({Target->entry(), 0}, literal(int64_t(Stride) << 29, 4, Send));
    Ref Setup = extract(D, storeSlot(frame(Stride), Jump));
    for (unsigned N = I.arg_size(); N--;)
      Setup = read(
          I.getArgOperand(N),
          storeSlot(frame(Stride + Target->Slots.lookup(F->getArg(N))), Setup));
    Ref Guard = branch(Setup, {"depth_error", 0});
    return extract(
        {}, alu(20, secondary(5, Guard, {}, FrameBase + (Frames - 1) * Stride),
                29));
  }
  Ref lower(Instruction &I, Ref D) {
    if (auto *B = dyn_cast<BinaryOperator>(&I))
      return binary(*B, save(&I, D));
    if (auto *C = dyn_cast<ICmpInst>(&I))
      return compare(*C, save(&I, D));
    if (auto *S = dyn_cast<SelectInst>(&I)) {
      Ref Save = save(&I, D);
      Ref Yes = read(S->getTrueValue(), Save),
          No = read(S->getFalseValue(), Save);
      return read(S->getCondition(), branch(Yes, No));
    }
    if (auto *C = dyn_cast<CastInst>(&I)) {
      if (I.getOpcode() != Instruction::Trunc &&
          I.getOpcode() != Instruction::ZExt &&
          I.getOpcode() != Instruction::SExt)
        fail("unsupported cast");
      width(C->getSrcTy());
      Ref Save = save(&I, D);
      if (I.getOpcode() == Instruction::SExt && C->getSrcTy()->isIntegerTy(1))
        Save = alu(6, Save, -1);
      return read(C->getOperand(0), Save);
    }
    if (isa<AllocaInst>(I))
      return D;
    if (auto *G = dyn_cast<GetElementPtrInst>(&I))
      return gep(*G, save(&I, D));
    if (auto *L = dyn_cast<LoadInst>(&I)) {
      if (L->isVolatile() || L->isAtomic() || L->getAlign() < Align(4))
        fail("loads require nonvolatile, nonatomic, four-byte-aligned cells");
      cells(L->getType());
      Ref Save = save(&I, D);
      if (auto A = staticPointer(L->getPointerOperand()))
        return readSlot(*A, Save);
      return memory(L->getPointerOperand(), Save);
    }
    if (auto *S = dyn_cast<StoreInst>(&I)) {
      if (S->isVolatile() || S->isAtomic() || S->getAlign() < Align(4))
        fail("stores require nonvolatile, nonatomic, four-byte-aligned cells");
      cells(S->getValueOperand()->getType());
      if (auto A = staticPointer(S->getPointerOperand()))
        return read(S->getValueOperand(), storeSlot(*A, D));
      return memory(S->getPointerOperand(), D, S->getValueOperand());
    }
    if (auto *B = dyn_cast<UncondBrInst>(&I))
      return edge(I.getParent(), B->getSuccessor(0));
    if (auto *B = dyn_cast<CondBrInst>(&I)) {
      Ref Yes = edge(I.getParent(), B->getSuccessor(0)),
          No = edge(I.getParent(), B->getSuccessor(1));
      return read(B->getCondition(), branch(Yes, No));
    }
    if (auto *R = dyn_cast<ReturnInst>(&I)) {
      Ref S = send(frame(0), {}, 1);
      return R->getReturnValue() ? read(R->getReturnValue(), S)
                                 : constant(0, S);
    }
    if (auto *C = dyn_cast<CallInst>(&I))
      return call(*C, I.getType()->isVoidTy() ? D : save(&I, D));
    fail(Twine("unsupported instruction ") + I.getOpcodeName());
  }
  void compile() {
    Record R;
    R.Name = "host_return";
    R.Op = 26;
    R.Dest = {"success", 0};
    R.Second = 0;
    add(R);
    R = {};
    R.Name = "result_write";
    R.Kind = Squall::WRITE;
    R.Value = 1;
    add(R);
    R = {};
    R.Name = "success";
    R.Op = 27;
    R.Value = 1;
    R.Dest = {"status_write", 0};
    add(R);
    R = {};
    R.Name = "status_write";
    R.Kind = Squall::WRITE;
    R.Value = 2;
    add(R);
    jump("depth_error", constant(2, {"status_write", 0}));
    jump("memory_error", constant(3, {"status_write", 0}));
    for (auto &C : Functions) {
      Context = C.get();
      jump(C->entry(), {C->Blocks.lookup(&C->F->getEntryBlock()), 0});
      for (auto &B : *C->F) {
        SmallVector<Instruction *> Executable;
        for (auto &I : B)
          if (!isa<PHINode>(I))
            Executable.push_back(&I);
        jump(C->Blocks.lookup(&B),
             {C->Instructions.lookup(Executable.front()), 0});
        for (unsigned N = 0; N < Executable.size(); ++N) {
          Ref Next = N + 1 < Executable.size()
                         ? Ref{C->Instructions.lookup(Executable[N + 1]), 0}
                         : Ref{};
          Ref Start = lower(*Executable[N], Next);
          jump(C->Instructions.lookup(Executable[N]), Start);
        }
      }
    }
    Context = FunctionMap[M.getFunction(Entry)];
    SmallVector<std::pair<std::string, unsigned>> Joins;
    for (unsigned I = 1; I < Arguments.size(); ++I) {
      unsigned S = GlobalNext++;
      Initial[S] = 0;
      Joins.push_back({unique("bootstrap_join"), S});
      Bootstrap.push_back(S);
    }
    if (GlobalNext > 256)
      fail("bootstrap/global slots overlap code");
    for (unsigned I = 0; I < Joins.size(); ++I) {
      R = {};
      R.Name = Joins[I].first;
      R.Kind = Squall::ALUS;
      R.Op = 0;
      R.Rule = 0;
      R.Address = absolute(Joins[I].second);
      R.Dest = {I + 1 < Joins.size() ? Joins[I + 1].first : Context->entry(),
                0};
      add(R);
    }
    SmallVector<std::pair<Ref, int64_t>> Tokens;
    for (unsigned I = 0; I < Arguments.size(); ++I) {
      Ref Dest{Context->entry(), 0};
      if (!Joins.empty())
        Dest = I < 2 ? Ref{Joins[0].first, I} : Ref{Joins[I - 1].first, 1};
      R = {};
      R.Name = unique("argument");
      R.Kind = Squall::ALUS;
      R.Op = 27;
      R.Rule = 1;
      R.Address = frame(Context->Slots.lookup(Context->F->getArg(I)));
      R.Dest = Dest;
      Tokens.push_back(
          {add(R, 1),
           canonical(Arguments[I], width(Context->F->getArg(I)->getType()))});
    }
    if (Tokens.empty())
      Tokens.push_back({{Context->entry(), 0}, 0});
    Labels["halt"] = 0;
    for (unsigned I = 0; I < Records.size(); ++I)
      Labels[Records[I].Name] = 256 + I;
    if (256 + Records.size() >
        uint64_t(DataNext > DataBase ? DataBase : FrameBase))
      fail("code overlaps array or frame arena");
    Initial[FrameBase] = slot(Labels["host_return"]);
    for (unsigned I = 0; I < Tokens.size(); ++I) {
      auto [T, V] = Tokens[I];
      uint64_t At = (StackBase - 16 * I) / 8;
      Initial[At] =
          (Labels[T.Name] | uint64_t(FrameBase) << 29 | uint64_t(T.Port) << 58)
          << 2;
      Initial[At + 1] = V;
    }
    StackTop = StackBase - 16 * Tokens.size();
  }
  MCInst instruction(const Record &R, uint64_t Here) {
    MCInst I;
    I.setOpcode(R.Kind);
    auto imm = [&](int64_t V) { I.addOperand(MCOperand::createImm(V)); };
    auto relative = [&](Ref D) {
      auto P = Labels.find(D.Name);
      if (P == Labels.end())
        fail("undefined instruction label");
      imm(int64_t(P->second) - int64_t(Here));
      imm(D.Port);
    };
    if (R.Kind == Squall::WRITE) {
      imm(R.Value);
      return I;
    }
    if (R.Kind == Squall::ALUI || R.Kind == Squall::ALUS)
      imm(R.Op);
    relative(R.Dest);
    if (R.Kind == Squall::ALUI || R.Kind == Squall::ALUS)
      imm(R.Second);
    if (R.Kind == Squall::ALUI)
      imm(R.Value);
    else {
      if (R.Kind == Squall::EXTRACT)
        relative(R.Target);
      imm(R.Address.Label.empty()
              ? R.Address.Offset
              : int64_t(Labels.at(R.Address.Label)) - int64_t(Here));
      imm(R.Address.Mode);
      imm(R.Rule);
    }
    return I;
  }
  json::Object manifest() {
    json::Array A, B;
    for (auto V : Arguments)
      A.push_back(V);
    for (auto V : Bootstrap)
      B.push_back(V);
    json::Object G, F, S;
    for (auto &P : Globals)
      G[P.first->getName()] = P.second;
    for (auto &C : Functions) {
      json::Array Slots;
      for (auto &A : C->F->args())
        Slots.push_back(C->Slots.lookup(&A));
      json::Object Allocas;
      for (auto &P : C->Allocas)
        Allocas[P.first->getName()] = P.second.Offset;
      F[C->F->getName()] = json::Object{{"entry_word", Labels[C->entry()]},
                                        {"frame_words", C->Count},
                                        {"argument_slots", std::move(Slots)},
                                        {"alloca_slots", std::move(Allocas)}};
    }
    for (auto &P : Labels)
      S[P.first] = P.second;
    std::string IR;
    raw_string_ostream OS(IR);
    M.print(OS, nullptr);
    json::Array Mailboxes;
    if (MemoryHelpers)
      for (unsigned I = 3; I <= 6; ++I)
        Mailboxes.push_back(I);
    return json::Object{
        {"format", "squall-v2-image-1"},
        {"abi", "squall-cell-memory-2"},
        {"compiler", "LLVM native Squall IR-to-MC"},
        {"source_sha256", toHex(SHA256::hash(arrayRefFromStringRef(IR)), true)},
        {"entry", Entry.getValue()},
        {"arguments", std::move(A)},
        {"return_type", Context->F->getReturnType()->isVoidTy()       ? "void"
                        : Context->F->getReturnType()->isPointerTy()  ? "ptr"
                        : Context->F->getReturnType()->isIntegerTy(1) ? "i1"
                                                                      : "i32"},
        {"memory_bytes", MemoryBytes.getValue()},
        {"word_bytes", 8},
        {"stack_base", StackBase},
        {"stack_top", StackTop},
        {"root_frame_word", FrameBase},
        {"frame_stride_words", Stride},
        {"max_frames", Frames.getValue()},
        {"stack_token_capacity", StackTokens.getValue()},
        {"result_word", 1},
        {"status_word", 2},
        {"max_cycles", 1000000},
        {"initial_token_count", (StackBase - StackTop) / 16},
        {"bootstrap_slots", std::move(B)},
        {"globals", std::move(G)},
        {"functions", std::move(F)},
        {"symbols", std::move(S)},
        {"memory_model",
         json::Object{{"pointer_bits", 32},
                      {"logical_cell_bytes", 4},
                      {"physical_cell_bytes", 8},
                      {"pointer_to_word_shift", 2},
                      {"array_base_word", DataBase},
                      {"array_end_word", DataNext},
                      {"runtime_mailbox_words", std::move(Mailboxes)}}}};
  }
  void emit() {
    const Target &T = TM.getTarget();
    const Triple &TT = TM.getTargetTriple();
    MCTargetOptions Options;
    std::unique_ptr<MCRegisterInfo> MRI(T.createMCRegInfo(TT));
    std::unique_ptr<MCAsmInfo> MAI(T.createMCAsmInfo(*MRI, TT, Options));
    std::unique_ptr<MCSubtargetInfo> STI(T.createMCSubtargetInfo(TT, "", ""));
    std::unique_ptr<MCInstrInfo> MII(T.createMCInstrInfo());
    MCContext C(TT, *MAI, *MRI, *STI);
    MCObjectFileInfo MOFI;
    MOFI.initMCObjectFileInfo(C, false);
    C.setObjectFileInfo(&MOFI);
    std::unique_ptr<MCAsmBackend> Backend(
        T.createMCAsmBackend(*STI, *MRI, Options));
    std::unique_ptr<MCCodeEmitter> Encoder(T.createMCCodeEmitter(*MII, C));
    std::unique_ptr<MCStreamer> Stream;
    if (FileType == CodeGenFileType::AssemblyFile) {
      std::unique_ptr<MCInstPrinter> P(
          T.createMCInstPrinter(TT, 0, *MAI, *MII, *MRI));
      Stream.reset(T.createAsmStreamer(
          C, std::make_unique<formatted_raw_ostream>(Out), std::move(P),
          std::move(Encoder), std::move(Backend)));
    } else {
      auto Writer = Backend->createObjectWriter(Out);
      Stream.reset(T.createMCObjectStreamer(TT, C, std::move(Backend),
                                            std::move(Writer),
                                            std::move(Encoder), *STI));
    }
    Stream->initSections(*STI);
    Stream->switchSection(C.getELFSection(".squall.image", ELF::SHT_PROGBITS,
                                          ELF::SHF_ALLOC | ELF::SHF_EXECINSTR));
    std::map<uint64_t, const Record *> Code;
    for (unsigned N = 0; N < Records.size(); ++N)
      Code[256 + N] = &Records[N];
    uint64_t Cursor = 0;
    std::map<uint64_t, uint64_t> Words = Initial;
    for (auto &P : Code)
      Words[P.first] = 0;
    for (auto &P : Words) {
      if (P.first * 8 > Cursor)
        Stream->emitZeros(P.first * 8 - Cursor);
      auto I = Code.find(P.first);
      if (I != Code.end()) {
        const Record &R = *I->second;
        Stream->emitLabel(C.getOrCreateSymbol(R.Name));
        if (R.Data)
          Stream->emitIntValue(R.Value, 8);
        else
          Stream->emitInstruction(instruction(R, P.first), *STI);
      } else
        Stream->emitIntValue(P.second, 8);
      Cursor = P.first * 8 + 8;
    }
    Stream->emitZeros(MemoryBytes - Cursor);
    Stream->switchSection(
        C.getELFSection(".squall.config", ELF::SHT_PROGBITS, 0));
    std::string Config;
    raw_string_ostream OS(Config);
    OS << json::Value(manifest());
    Stream->emitBytes(Config);
    Stream->finish();
    if (C.hadError())
      fail("MC emission failed; see assembler diagnostics");
  }

public:
  Lowering(Module &M, TargetMachine &TM, raw_pwrite_stream &O,
           CodeGenFileType F)
      : M(M), TM(TM), Out(O), FileType(F) {}
  void run() {
    if (verifyModule(M, &errs()))
      fail("invalid LLVM module");
    for (auto &A : Args) {
      int64_t V;
      if (StringRef(A).getAsInteger(0, V))
        fail("invalid -squall-arg integer");
      Arguments.push_back(V);
    }
    layout();
    compile();
    emit();
  }
};
class LoweringPass : public ModulePass {
  TargetMachine &TM;
  raw_pwrite_stream &Out;
  CodeGenFileType FileType;

public:
  static char ID;
  LoweringPass(TargetMachine &T, raw_pwrite_stream &O, CodeGenFileType F)
      : ModulePass(ID), TM(T), Out(O), FileType(F) {}
  StringRef getPassName() const override {
    return "Squall native token and frame lowering";
  }
  bool runOnModule(Module &M) override {
    Lowering(M, TM, Out, FileType).run();
    return false;
  }
};
char LoweringPass::ID = 0;
} // namespace
ModulePass *llvm::createSquallLoweringPass(TargetMachine &T,
                                           raw_pwrite_stream &O,
                                           CodeGenFileType F) {
  return new LoweringPass(T, O, F);
}
