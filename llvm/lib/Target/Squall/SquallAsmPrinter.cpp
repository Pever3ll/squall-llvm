//===-- SquallAsmPrinter.cpp - Squall image and instruction emission ----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Squall instructions name their successor explicitly; there is no program
// counter. Each code generation pseudo expands to a chain of encoded words
// whose last token continues at the word after the chain, so straight-line
// machine code still runs in layout order. Branches name a basic block.
//
// The module is emitted as one dense physical memory image (word addresses
// in parentheses):
//
//   (0)        halt destination; tokens sent here are discarded
//   (1), (2)   result and status written by the host return stub
//   (3)-(6)    mailboxes of the serial dynamic memory-access helper
//   (7)        join slot of parallel lanes; empty between joins
//   (8)-(15)   entry argument slots, written by the loader
//   (16)...    globals, in module order
//   (>=256)    runtime stubs, then functions
//   arena      activation frames, starting with the root frame
//   stack      the token stack, holding one start token
//
// A `.squall.config` section describes the layout for loaders and simulators.
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/SquallMCTargetDesc.h"
#include "Squall.h"
#include "SquallMachineFunctionInfo.h"
#include "SquallSubtarget.h"
#include "TargetInfo/SquallTargetInfo.h"
#include "llvm/Analysis/ConstantFolding.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/CodeGen/AsmPrinter.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Module.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCSectionELF.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/VCSRevision.h"
#include "llvm/Target/TargetMachine.h"
#include <numeric>
#include <set>

using namespace llvm;

#define DEBUG_TYPE "asm-printer"

static cl::opt<std::string>
    EntryName("squall-entry", cl::init("main"),
              cl::desc("Function started by the Squall image's start token"));
static cl::opt<uint64_t>
    MemoryBytes("squall-memory-bytes", cl::init(512 * 1024),
                cl::desc("Physical memory size of the Squall image in bytes"));
static cl::opt<unsigned>
    StackTokens("squall-stack-tokens", cl::init(1024),
                cl::desc("Token stack capacity of the Squall image"));

StringRef llvm::Squall::entryName() { return EntryName; }

namespace {

// ALU operation numbers of the Squall v2 ISA.
enum AluOp : int {
  Add = 4,
  Sub = 5,
  Mul = 6,
  Div = 7,
  Divu = 8,
  Rem = 9,
  And = 16,
  Or = 17,
  Xor = 18,
  Shl = 19,
  Shrl = 20,
  Shra = 21,
  Dupl = 26, // Result is the left (token) operand.
  Dupr = 27, // Result is the right (slot or immediate) operand.
  Beqz = 32, // Primary result if the token is nonzero, else the secondary.
  Beql = 33, // Primary result if the operands differ, else the secondary.
};
// Squall v2 field layout, as implemented by Isacomp squall-v2-rebase.
constexpr unsigned TagFPShift = 32; // Tag: ip 28:0, fp 60:32, port 61.
constexpr int64_t TagIPMask = (INT64_C(1) << 29) - 1;
constexpr unsigned FullWordMask = 0xff; // Write byte enables.
enum SlotMode : unsigned { FrameSlot = 0, CodeSlot = 1, AbsoluteSlot = 2 };
enum MatchRule : unsigned { Once = 0, Const = 1, Read = 2 };
enum Port : unsigned { Left = 0, Right = 1 };

constexpr unsigned ResultWord = 1, StatusWord = 2, FirstMailbox = 3,
                   JoinWord = 7, ArgumentWord = 8, MaxEntryArgs = 8,
                   FirstGlobalWord = 16, MinCodeWord = 256;
constexpr int64_t StatusSuccess = 1, StatusDepth = 2, StatusMemory = 3,
                  StatusTrap = 4;
// A token tag packs the instruction word address in bits 0-28, the frame
// pointer word in bits 32-60, and the entry port in bit 61, so a tag fits the
// 62-bit payload of a slot.
//
// A slot read zero-extends that payload, so a value round-trips through a
// slot only if it is a nonnegative number below 2^62. Registers therefore
// hold their 32-bit values zero-extended, and signed operations extend their
// operands in the token, never in a slot.

/// A matching-slot word holding a persistent constant: presence bits 0b10
/// and a 62-bit payload, read back zero-extended.
uint64_t constantSlot(uint64_t Value) {
  assert(isUInt<62>(Value) && "slot payload out of range");
  return UINT64_C(2) << 62 | Value;
}

/// A successor destination.
struct Dest {
  enum Kind { Next, End, Item, Symbol } K = Next;
  int Index = 0;
  const MCSymbol *Sym = nullptr;
  static Dest end() { return {End}; }
  static Dest item(int I) { return {Item, I}; }
  static Dest symbol(const MCSymbol *S) { return {Symbol, 0, S}; }
};

/// One word of an expansion chain.
struct Word {
  unsigned Opcode = Squall::ALUI;
  int Alu = Dupr;
  Dest D;
  bool Secondary = false; // Also send the secondary result to the next word.
  int64_t Imm = 0;        // ALUI immediate or WRITE address.
  unsigned Mode = FrameSlot;
  int64_t Slot = 0;
  int CodeData = -1; // Code-relative slot: index of a data word.
  unsigned Rule = Read;
  Dest Target; // EXTRACT: the instruction whose tag is produced.
  Port Entry = Left;
  bool Detached = false; // Reached only through an explicit destination.
  bool IsData = false;
  uint64_t Data = 0;
  const MCSymbol *Label = nullptr;
};

/// Builds a chain of words and resolves its intra-chain destinations. Words
/// default to continuing at the next attached word, or after the chain.
class Chain {
  SmallVector<Word, 24> Words;
  const MCSymbol *Halt;
  unsigned EndPort = Left; // Entry port of the word after the chain.
  unsigned Scratch;        // Frame word for intermediate values.

  int nextAttached(int I) const {
    for (int J = I + 1, E = Words.size(); J < E; ++J)
      if (!Words[J].IsData && !Words[J].Detached)
        return J;
    return Words.size();
  }

  void destination(MCInst &Inst, int Here, const Dest &D, MCContext &Ctx) {
    int Target;
    switch (D.K) {
    case Dest::Symbol:
      Inst.addOperand(
          MCOperand::createExpr(MCSymbolRefExpr::create(D.Sym, Ctx)));
      Inst.addOperand(MCOperand::createImm(Left));
      return;
    case Dest::Next:
      Target = nextAttached(Here);
      break;
    case Dest::End:
      Target = Words.size();
      break;
    case Dest::Item:
      Target = D.Index;
      break;
    }
    Inst.addOperand(MCOperand::createImm(Target - Here));
    Inst.addOperand(MCOperand::createImm(entryPort(Target)));
  }

  unsigned entryPort(int I) const {
    if (I >= int(Words.size()))
      return EndPort;
    assert(!Words[I].IsData && "destination is a data word");
    return Words[I].Entry;
  }

public:
  explicit Chain(const MCSymbol *Halt, unsigned Scratch = Squall::ScratchWord)
      : Halt(Halt), Scratch(Scratch) {}

  unsigned scratch() const { return Scratch; }

  Word &operator[](int I) { return Words[I]; }
  int size() const { return Words.size(); }
  unsigned entry() const { return Words.front().Entry; }
  void setEndPort(unsigned Port) { EndPort = Port; }

  /// The frame word whose value the token carries when it leaves the chain:
  /// set when the only way out is a final store, whose result is the stored
  /// value.
  std::optional<unsigned> storedWord() const {
    const Word &Last = Words.back();
    if (Last.IsData || Last.Opcode != Squall::ALUS || Last.Alu != Dupr ||
        Last.Mode != FrameSlot || Last.Rule != Const || Last.Detached ||
        Last.D.K != Dest::Next)
      return std::nullopt;
    for (const Word &W : Words)
      if (W.D.K == Dest::End || W.Target.K == Dest::End)
        return std::nullopt;
    return Last.Slot;
  }
  /// Whether the chain starts by reading \p FrameWord into the token.
  bool readsFirst(unsigned FrameWord) const {
    const Word &First = Words.front();
    if (Words.size() < 2 || First.IsData || First.Label ||
        First.Opcode != Squall::ALUS || First.Alu != Dupr ||
        First.Mode != FrameSlot || First.Rule != Read ||
        First.Slot != FrameWord)
      return false;
    return none_of(Words, [](const Word &W) {
      return (W.D.K == Dest::Item && W.D.Index == 0) ||
             (W.Target.K == Dest::Item && W.Target.Index == 0);
    });
  }
  /// Remove the first word, which must not be a destination.
  void dropFirst() {
    Words.erase(Words.begin());
    for (Word &W : Words) {
      if (W.D.K == Dest::Item)
        --W.D.Index;
      if (W.Target.K == Dest::Item)
        --W.Target.Index;
      if (W.CodeData >= 0)
        --W.CodeData;
    }
  }

  int add(Word W) {
    Words.push_back(W);
    return Words.size() - 1;
  }
  int imm(int Alu, int64_t Value) {
    Word W;
    W.Alu = Alu;
    W.Imm = Value;
    return add(W);
  }
  int slotOp(int Alu, unsigned Mode, int64_t Slot, unsigned Rule = Read) {
    Word W;
    W.Opcode = Squall::ALUS;
    W.Alu = Alu;
    W.Mode = Mode;
    W.Slot = Slot;
    W.Rule = Rule;
    if (Rule == Const)
      W.Entry = Right; // A right token installs the constant.
    return add(W);
  }
  int read(unsigned FrameWord) { return slotOp(Dupr, FrameSlot, FrameWord); }
  int readAbs(unsigned Address) { return slotOp(Dupr, AbsoluteSlot, Address); }
  int store(unsigned FrameWord) {
    return slotOp(Dupr, FrameSlot, FrameWord, Const);
  }
  int storeAbs(unsigned Address) {
    return slotOp(Dupr, AbsoluteSlot, Address, Const);
  }
  int jump(Dest D) {
    int I = imm(Dupl, 0);
    Words[I].D = D;
    return I;
  }
  int data(uint64_t Value) {
    Word W;
    W.IsData = true;
    W.Data = Value;
    return add(W);
  }
  /// Produce the packed tag of \p Target with the current frame pointer.
  int extract(Dest Target) {
    Word W;
    W.Opcode = Squall::EXTRACT;
    W.Target = Target;
    return add(W);
  }
  /// Send the token value to the packed tag (the left operand) read from a
  /// slot when entered on the right, or held in the token on the left.
  int send(unsigned Mode, int64_t Slot, Port Entry) {
    Word W;
    W.Opcode = Squall::SEND;
    W.Mode = Mode;
    W.Slot = Slot;
    W.D = Dest::symbol(Halt);
    W.Entry = Entry;
    return add(W);
  }
  int write(int64_t Address) {
    Word W;
    W.Opcode = Squall::WRITE;
    W.Imm = Address;
    return add(W);
  }
  /// The token's low 32 bits, sign-extended to 64. The result may be
  /// negative, so it must not be stored in a slot.
  void signExtend32() {
    imm(Shl, 32);
    imm(Shra, 32);
  }
  /// The token's low 32 bits, zero-extended: the register representation.
  void zeroExtend32() {
    imm(Shl, 32);
    imm(Shrl, 32);
  }
  /// The negation of the token: 0 - token, entered on the right so that the
  /// literal is the left operand.
  int negate() {
    int I = imm(Sub, 0);
    Words[I].Entry = Right;
    return I;
  }
  /// Leave the secondary result (a borrow, for Sub) in the scratch word: the
  /// secondary token is consumed first and captured, then the primary token
  /// resumes by reading it back.
  int captureSecondary(int Producer) {
    Words[Producer].Secondary = true;
    int Capture = store(Scratch);
    Words[Capture].D = Dest::symbol(Halt);
    Words[Capture].Detached = true;
    int Resume = read(Scratch);
    Words[Producer].D = Dest::item(Resume);
    return Resume;
  }
  /// Continue at \p Taken when the token is nonzero, else fall to the next
  /// attached word through a stub that jumps to \p NotTaken (or falls through
  /// if \p NotTaken is Next).
  void branch(Dest Taken) {
    int Test = imm(Beqz, 0);
    Words[Test].D = Taken;
    Words[Test].Secondary = true;
  }

  void emit(MCStreamer &Out, const MCSubtargetInfo &STI, MCContext &Ctx) {
    for (int I = 0, E = Words.size(); I < E; ++I) {
      Word &W = Words[I];
      if (W.Label)
        Out.emitLabel(const_cast<MCSymbol *>(W.Label));
      if (W.IsData) {
        Out.emitIntValue(W.Data, 8);
        continue;
      }
      MCInst Inst;
      Inst.setOpcode(W.Opcode);
      auto imm = [&](int64_t V) { Inst.addOperand(MCOperand::createImm(V)); };
      if (W.Opcode == Squall::WRITE) {
        imm(W.Imm);
        imm(FullWordMask);
        imm(0); // No acknowledgement.
        imm(0);
        imm(0);
        Out.emitInstruction(Inst, STI);
        continue;
      }
      bool Alu = W.Opcode == Squall::ALUI || W.Opcode == Squall::ALUS;
      if (Alu)
        imm(W.Alu);
      destination(Inst, I, W.D, Ctx);
      if (Alu) {
        if (W.Secondary) {
          assert(I + 1 == E || !Words[I + 1].IsData);
          imm(entryPort(I + 1));
        } else {
          imm(-1);
        }
      }
      if (W.Opcode == Squall::ALUI) {
        imm(W.Imm);
      } else if (W.Opcode == Squall::EXTRACT) {
        destination(Inst, I, W.Target, Ctx); // Monadic: no slot.
      } else {
        imm(W.Mode == CodeSlot ? W.CodeData - I : W.Slot);
        imm(W.Mode);
        imm(W.Rule);
      }
      Out.emitInstruction(Inst, STI);
    }
  }
};

class SquallAsmPrinter : public AsmPrinter {
  DenseMap<const GlobalVariable *, unsigned> GlobalWords;
  const Function *Entry = nullptr;
  MCSymbol *Halt = nullptr, *DepthError = nullptr, *MemoryError = nullptr,
           *Trap = nullptr;
  uint64_t CodeStart = 0, FrameBase = 0, ArenaEnd = 0, StackBase = 0;
  /// Words emitted from CodeStart on: every chain word, instruction or data,
  /// is one eight-byte word.
  uint64_t CodeWords = 0;
  /// The previous instruction's chain, held back so that a following read of
  /// the register it just stored can be dropped: the token still carries it.
  std::optional<Chain> Pending;
  bool LaidOut = false;
  unsigned GlobalBase = FirstGlobalWord; // First word of the globals.
  void emitImageHeader(Module &M);
  void countPadding(Align Alignment);
  unsigned HostReturnWord = 0, StartWord = 0;
  /// Parallel lanes (see SquallForkJoin): the scratch word of the lane being
  /// emitted, and the labels of each region's second lane and join.
  unsigned Scratch = Squall::ScratchWord;
  DenseMap<int64_t, std::pair<MCSymbol *, MCSymbol *>> Lanes;
  bool Forked = false;              // Whether any lanes were emitted.
  std::set<unsigned> UsedJoinWords; // Join words of the emitted lanes.

public:
  static char ID;
  SquallAsmPrinter(TargetMachine &TM, std::unique_ptr<MCStreamer> Streamer)
      : AsmPrinter(TM, std::move(Streamer), ID) {}

  StringRef getPassName() const override { return "Squall Assembly Printer"; }

  /// The image is laid out when the first function is emitted: the pass
  /// manager initializes the printer before running the IR passes, and those
  /// may add globals (SquallLowerAlloca) and delete functions
  /// (SquallLowerFloat).
  bool runOnMachineFunction(MachineFunction &MF) override {
    if (!LaidOut) {
      LaidOut = true;
      emitImageHeader(*MF.getFunction().getParent());
    }
    return AsmPrinter::runOnMachineFunction(MF);
  }
  void emitEndOfAsmFile(Module &M) override;
  // Globals are placed with the image header in emitImageHeader.
  void emitGlobalVariable(const GlobalVariable *) override {}
  void emitInstruction(const MachineInstr *MI) override;
  void emitBasicBlockEnd(const MachineBasicBlock &MBB) override;
  void emitFunctionEntryLabel() override;
  void emitBasicBlockStart(const MachineBasicBlock &MBB) override;
  // An aligned block is entered by a jump over its padding, so it needs its
  // label even when only its layout predecessor reaches it.
  bool isBlockOnlyReachableByFallthrough(
      const MachineBasicBlock *MBB) const override {
    return MBB->getAlignment() <= Align(8) &&
           AsmPrinter::isBlockOnlyReachableByFallthrough(MBB);
  }

private:
  MCSection *imageSection() {
    return OutContext.getELFSection(".squall.image", ELF::SHT_PROGBITS,
                                    ELF::SHF_ALLOC | ELF::SHF_WRITE |
                                        ELF::SHF_EXECINSTR);
  }
  /// A memory size that fits Words of code and globals and still leaves the
  /// frames and token stack the share a default layout gives them, three
  /// quarters of memory: one frame's reach alone would fail at the first
  /// deep call.
  uint64_t suggestedMemory(uint64_t Words) const {
    return alignTo(Words * 8 + MemoryBytes - MemoryBytes / 4, 65536);
  }
  void error(const Twine &Message) {
    OutContext.reportError(SMLoc(), Twine("Squall: ") + Message);
  }
  unsigned word(const MachineOperand &MO) const {
    return MF->getSubtarget().getRegisterInfo()->getEncodingValue(MO.getReg());
  }
  unsigned globalWord(const MachineOperand &MO);
  DenseMap<const Function *, MCSymbol *> EntryLabels;
  /// A local label at a function's entry. References to the (possibly
  /// global) function symbol would become relocations, and there is no
  /// Squall linker to apply them.
  MCSymbol *entryLabel(const Function &F) {
    // Symbol names may end in .l/.r, which the assembler interprets as
    // destination ports. Use an opaque local name, independent of the user.
    auto &Label = EntryLabels[&F];
    if (!Label)
      Label = OutContext.createTempSymbol("squall_entry");
    return Label;
  }
  /// Function pointers in the initializer being emitted: cell, function.
  SmallVector<std::pair<uint64_t, const Function *>, 4> FunctionCells;
  /// Write a constant's little-endian bytes at Offset in Bytes.
  void fillBytes(const Constant *C, const DataLayout &DL,
                 MutableArrayRef<uint8_t> Bytes, uint64_t Offset);
  /// Emit a global's initializer as constant slots, four bytes per cell.
  void emitCells(const Constant *C, const DataLayout &DL);
  void expandBinary(Chain &C, const MachineInstr &MI, int Alu, bool Normalize);
  void expandSigned(Chain &C, const MachineInstr &MI, int Alu);
  void expandShiftRightArith(Chain &C, const MachineInstr &MI);
  void expandCompare(Chain &C, const MachineInstr &MI);
  void expandMemory(Chain &C, const MachineInstr &MI, bool IsStore,
                    std::optional<unsigned> PointerReg, int64_t Pointer);
  void expandCall(Chain &C, const MachineInstr &MI);
  /// The chain of encoded words for one machine instruction.
  Chain expand(const MachineInstr &MI, unsigned Scratch);
  /// Emit a chain after the pending one, dropping a redundant first read.
  void append(Chain C, bool ForwardRead = true);
  void emitLaneMarker(const MachineInstr &MI);
  json::Object manifest(const Module &M) const;
  void flush(unsigned Port) {
    if (!Pending)
      return;
    Pending->setEndPort(Port);
    Pending->emit(*OutStreamer, TM.getMCSubtargetInfo(), OutContext);
    CodeWords += Pending->size();
    Pending.reset();
  }
};

} // namespace

unsigned SquallAsmPrinter::globalWord(const MachineOperand &MO) {
  const auto *G = cast<GlobalVariable>(MO.getGlobal());
  assert(MO.getOffset() % 4 == 0 && "slot access must be a whole cell");
  return GlobalWords.lookup(G) + MO.getOffset() / 4;
}

void SquallAsmPrinter::fillBytes(const Constant *C, const DataLayout &DL,
                                 MutableArrayRef<uint8_t> Bytes,
                                 uint64_t Offset) {
  if (isa<ConstantAggregateZero>(C) || isa<UndefValue>(C) ||
      isa<ConstantPointerNull>(C))
    return;
  if (isa<ConstantInt>(C) || isa<ConstantFP>(C)) {
    unsigned Size = DL.getTypeStoreSize(C->getType());
    APInt Value = isa<ConstantFP>(C)
                      ? cast<ConstantFP>(C)->getValueAPF().bitcastToAPInt()
                      : cast<ConstantInt>(C)->getValue().zext(Size * 8);
    for (unsigned B = 0; B < Size; ++B)
      Bytes[Offset + B] = Value.extractBitsAsZExtValue(8, 8 * B);
    return;
  }
  if (auto *S = dyn_cast<StructType>(C->getType())) {
    const StructLayout *L = DL.getStructLayout(S);
    for (unsigned I = 0; I < S->getNumElements(); ++I)
      fillBytes(C->getAggregateElement(I), DL, Bytes,
                Offset + L->getElementOffset(I));
    return;
  }
  if (auto *A = dyn_cast<ArrayType>(C->getType())) {
    uint64_t Stride = DL.getTypeAllocSize(A->getElementType());
    for (unsigned I = 0; I < A->getNumElements(); ++I)
      fillBytes(C->getAggregateElement(I), DL, Bytes, Offset + I * Stride);
    return;
  }
  // The byte address of a global, plus a constant offset.
  GlobalValue *GV = nullptr;
  APInt Extra(64, 0);
  if (!IsConstantOffsetFromGlobal(const_cast<Constant *>(C), GV, Extra, DL))
    return;
  if (const auto *G = dyn_cast<GlobalVariable>(GV)) {
    support::endian::write32le(
        &Bytes[Offset],
        uint32_t(GlobalWords.lookup(G) * 4 + Extra.getSExtValue()));
  } else if (const auto *F = dyn_cast<Function>(GV)) {
    // Code addresses are known only after layout; the cell is an expression.
    if (Offset % 4 || !Extra.isZero())
      error(Twine("function pointer to '") + F->getName() +
            "' must occupy a whole aligned cell in an initializer");
    else
      FunctionCells.push_back({Offset / 4, F});
  }
}

void SquallAsmPrinter::emitCells(const Constant *C, const DataLayout &DL) {
  SmallVector<uint8_t, 64> Bytes(alignTo(DL.getTypeAllocSize(C->getType()), 4));
  FunctionCells.clear();
  fillBytes(C, DL, Bytes, 0);
  for (unsigned I = 0; I < Bytes.size(); I += 4) {
    auto Code = find_if(FunctionCells,
                        [&](const auto &Cell) { return Cell.first == I / 4; });
    if (Code == FunctionCells.end()) {
      OutStreamer->emitIntValue(
          constantSlot(support::endian::read32le(&Bytes[I])), 8);
      continue;
    }
    // A constant slot holding the entry's word address.
    MCContext &Ctx = OutContext;
    const MCExpr *Entry = MCBinaryExpr::createDiv(
        MCBinaryExpr::createSub(
            MCSymbolRefExpr::create(entryLabel(*Code->second), Ctx),
            MCSymbolRefExpr::create(Halt, Ctx), Ctx),
        MCConstantExpr::create(8, Ctx), Ctx);
    OutStreamer->emitValue(
        MCBinaryExpr::createOr(
            Entry, MCConstantExpr::create(int64_t(constantSlot(0)), Ctx), Ctx),
        8);
  }
}

void SquallAsmPrinter::emitImageHeader(Module &M) {
  const DataLayout &DL = M.getDataLayout();
  Halt = OutContext.getOrCreateSymbol("__squall_halt");
  DepthError = OutContext.getOrCreateSymbol("__squall_depth_error");
  MemoryError = OutContext.getOrCreateSymbol("__squall_memory_error");
  Trap = OutContext.getOrCreateSymbol("__squall_trap");
  // This runs after the IR passes: a module that failed IR validation has
  // unreachable function bodies, and is still laid out so that code
  // generation completes quietly after the diagnostics.
  if (MemoryBytes < 64 * 1024 || MemoryBytes % 16 ||
      MemoryBytes > UINT64_C(1) << 32) {
    error("-squall-memory-bytes must be a multiple of 16 between 64 KiB and "
          "4 GiB");
    return;
  }
  const Function *F = M.getFunction(EntryName);
  if (!F || F->isDeclaration()) {
    error(Twine("entry function '") + EntryName +
          "' is not defined in this module (select one with -squall-entry)");
    return;
  }
  auto IsEntryType = [](const Type *T) {
    return T->isPointerTy() ||
           (T->isIntegerTy() && T->getIntegerBitWidth() <= 32);
  };
  bool Valid =
      F->arg_size() <= MaxEntryArgs &&
      (F->getReturnType()->isVoidTy() || IsEntryType(F->getReturnType()));
  for (const Argument &A : F->args())
    Valid &=
        IsEntryType(A.getType()) && !A.hasByValAttr() && !A.hasStructRetAttr();
  if (!Valid) {
    error(Twine("entry function '") + EntryName +
          "' must take at most 8 integer or pointer arguments of at most 32 "
          "bits and return such a value or void");
    return;
  }
  Entry = F;

  // Assign every global a word in the low data region, after the call join
  // words, which stay empty.
  GlobalBase = FirstGlobalWord;
  if (Squall::isForkJoinEnabled())
    GlobalBase += Squall::NumCallJoinWords;
  uint64_t Next = GlobalBase;
  for (const GlobalVariable &G : M.globals()) {
    if (!G.hasInitializer() || G.getName().starts_with("llvm."))
      continue;
    uint64_t Words = alignTo(DL.getTypeAllocSize(G.getValueType()), 4) / 4;
    unsigned AlignWords =
        std::max<uint64_t>(1, DL.getPreferredAlign(&G).value() / 4);
    Next = alignTo(Next, AlignWords);
    GlobalWords[&G] = Next;
    Next += Words;
  }
  uint64_t MemoryWords = MemoryBytes / 8;
  CodeStart = std::max<uint64_t>(MinCodeWord, alignTo(Next, 8));
  // The frame arena starts a quarter of the way into memory, or after the
  // code when the code is larger; emitEndOfAsmFile moves it once the code's
  // size is known. Nothing in the code depends on where it starts.
  FrameBase = alignTo(MemoryWords / 4, 4);
  StackBase = MemoryBytes - 16;
  // The token stack sits below StackBase, and at least one frame's reach
  // must remain between the default arena start and the stack. (64 KiB
  // memory always leaves that much room, so this cannot underflow.)
  uint64_t MaxStackTokens =
      (StackBase - (FrameBase + Squall::MaxSlotWord + 1) * 8) / 16;
  if (StackTokens == 0 || StackTokens > MaxStackTokens) {
    error(Twine("-squall-stack-tokens must be between 1 and ") +
          Twine(MaxStackTokens) + " for a " + Twine(MemoryBytes / 1024) +
          " KiB image");
    Entry = nullptr;
    return;
  }
  ArenaEnd = (StackBase - uint64_t(StackTokens) * 16) / 8;
  if (CodeStart + Squall::MaxSlotWord + 1 > ArenaEnd ||
      FrameBase + Squall::MaxSlotWord + 1 > ArenaEnd) {
    error(Twine("the program's globals take ") + Twine(CodeStart * 8 / 1024) +
          " KiB, leaving no room for frames in the " +
          Twine(MemoryBytes / 1024) + " KiB image; use -squall-memory-bytes=" +
          Twine(suggestedMemory(CodeStart)) + " or more");
    Entry = nullptr;
    return;
  }

  OutStreamer->switchSection(imageSection());
  OutStreamer->emitLabel(Halt);
  for (unsigned W = 0; W < FirstGlobalWord; ++W) {
    bool Slot = (W >= FirstMailbox && W < FirstMailbox + 4) ||
                (W >= ArgumentWord && W < ArgumentWord + MaxEntryArgs);
    OutStreamer->emitIntValue(Slot ? constantSlot(0) : 0, 8);
  }
  for (const GlobalVariable &G : M.globals()) {
    auto It = GlobalWords.find(&G);
    if (It == GlobalWords.end())
      continue;
    OutStreamer->emitValueToOffset(
        MCConstantExpr::create(It->second * 8, OutContext), 0, SMLoc());
    OutStreamer->emitLabel(getSymbol(&G));
    emitCells(G.getInitializer(), DL);
  }
  OutStreamer->emitValueToOffset(
      MCConstantExpr::create(CodeStart * 8, OutContext), 0, SMLoc());

  // Runtime stubs. The entry function returns to HostReturn, which writes
  // the result and a success status. Error stubs write only a status.
  Chain Runtime(Halt);
  int HostReturn = Runtime.imm(Dupl, 0);
  Runtime[HostReturn].Label =
      OutContext.getOrCreateSymbol("__squall_host_return");
  int ResultWrite = Runtime.write(ResultWord);
  int Success = Runtime.imm(Dupr, StatusSuccess);
  int StatusWrite = Runtime.write(StatusWord);
  Runtime[HostReturn].D = Dest::item(Success);
  Runtime[HostReturn].Secondary = true; // Copy of the result to ResultWrite.
  (void)ResultWrite;
  Runtime[Success].D = Dest::item(StatusWrite);
  int Depth = Runtime.imm(Dupr, StatusDepth);
  Runtime[Depth].Label = DepthError;
  Runtime[Depth].D = Dest::item(StatusWrite);
  int Memory = Runtime.imm(Dupr, StatusMemory);
  Runtime[Memory].Label = MemoryError;
  Runtime[Memory].D = Dest::item(StatusWrite);
  int Trapped = Runtime.imm(Dupr, StatusTrap);
  Runtime[Trapped].Label = Trap;
  Runtime[Trapped].D = Dest::item(StatusWrite);
  // The start token runs in the root frame: copy the loader's argument slots
  // into the entry function's argument registers, then jump to it.
  int Start = Runtime.size();
  for (unsigned I = 0; I < Entry->arg_size(); ++I) {
    int R = Runtime.readAbs(ArgumentWord + I);
    if (I == 0)
      Start = R;
    Runtime.store(Squall::FirstRegWord + I);
  }
  int Jump = Runtime.jump(Dest::symbol(entryLabel(*Entry)));
  if (Entry->arg_size() == 0)
    Start = Jump;
  Runtime[Start].Label = OutContext.getOrCreateSymbol("__squall_start");
  HostReturnWord = CodeStart + HostReturn;
  StartWord = CodeStart + Start;
  Runtime.emit(*OutStreamer, TM.getMCSubtargetInfo(), OutContext);
  CodeWords += Runtime.size();
}

// Registers hold 32-bit values zero-extended to the 64-bit token width (a
// slot read zero-extends). An operation whose low 32 bits are right but whose
// upper bits may not be zero is normalized before the result is stored.
void SquallAsmPrinter::expandBinary(Chain &C, const MachineInstr &MI, int Alu,
                                    bool Normalize) {
  unsigned D = word(MI.getOperand(0)), A = word(MI.getOperand(1));
  const MachineOperand &B = MI.getOperand(2);
  C.read(A);
  if (B.isImm()) {
    C.imm(Alu, B.getImm());
    // A negative literal is sign-extended to 64 bits.
    Normalize |= (Alu == Or || Alu == Xor) && B.getImm() < 0;
  } else {
    C.slotOp(Alu, FrameSlot, word(B));
  }
  if (Normalize)
    C.zeroExtend32();
  C.store(D);
}

// Signed division, remainder and high product. Both operands must be
// sign-extended to 64 bits, but a negative value cannot sit in a slot, so the
// slot operand is the divisor's magnitude and the token carries the dividend,
// negated when the divisor is negative:
//   a / b == (-a) / |b|,  a % b == a % |b|,  a * b == (-a) * |b|  for b < 0.
// All of these are exact in 64 bits for 32-bit operands.
void SquallAsmPrinter::expandSigned(Chain &C, const MachineInstr &MI,
                                    int Alu) {
  unsigned D = word(MI.getOperand(0)), A = word(MI.getOperand(1));
  const MachineOperand &B = MI.getOperand(2);
  auto Finish = [&] {
    if (Alu == Mul)
      C.imm(Shrl, 32); // The high half, zero-extended.
    else
      C.zeroExtend32();
    C.store(D);
  };
  if (B.isImm()) {
    assert(Alu != Mul && "no high-product immediate form");
    C.read(A);
    C.signExtend32();
    C.imm(Alu, B.getImm()); // The literal is already sign-extended.
    Finish();
    return;
  }
  unsigned Divisor = word(B), Magnitude = C.scratch();
  bool NegateDividend = Alu != Rem;
  // Branch on the divisor's sign.
  C.read(Divisor);
  C.imm(Shl, 32);
  C.imm(Shra, 63);
  int Test = C.imm(Beqz, 0);
  C[Test].Secondary = true; // Nonnegative: fall into the next word.
  // b >= 0: |b| is b.
  C.read(Divisor);
  C.store(Magnitude);
  C.read(A);
  C.signExtend32();
  int PosOp = C.slotOp(Alu, FrameSlot, Magnitude);
  // b < 0: reached only from the test.
  int NegStart = C.read(Divisor);
  C.negate();
  C.zeroExtend32(); // |b|; 2^31 for INT32_MIN.
  C.store(Magnitude);
  C.read(A);
  C.signExtend32();
  if (NegateDividend)
    C.negate();
  int NegOp = C.slotOp(Alu, FrameSlot, Magnitude);
  for (int I = NegStart; I <= NegOp; ++I) {
    C[I].Detached = true;
    if (I < NegOp)
      C[I].D = Dest::item(I + 1);
  }
  C[Test].D = Dest::item(NegStart);
  int Join = C.size();
  Finish();
  C[PosOp].D = Dest::item(Join);
  C[NegOp].D = Dest::item(Join);
}

// An arithmetic shift of the value moved to the top of the token: the top 32
// bits are the 32-bit result, which a logical shift brings back down.
void SquallAsmPrinter::expandShiftRightArith(Chain &C,
                                             const MachineInstr &MI) {
  unsigned D = word(MI.getOperand(0)), A = word(MI.getOperand(1));
  const MachineOperand &B = MI.getOperand(2);
  C.read(A);
  C.imm(Shl, 32);
  if (B.isImm())
    C.imm(Shra, B.getImm());
  else
    C.slotOp(Shra, FrameSlot, word(B));
  C.imm(Shrl, 32);
  C.store(D);
}

void SquallAsmPrinter::expandCompare(Chain &C, const MachineInstr &MI) {
  unsigned D = word(MI.getOperand(0)), A = word(MI.getOperand(1)),
           B = word(MI.getOperand(2));
  auto CC = Squall::CondCode(MI.getOperand(3).getImm());
  if (CC == Squall::EQ || CC == Squall::NE) {
    C.read(A);
    int Test = C.slotOp(Beql, FrameSlot, B);
    C[Test].Secondary = true;
    int Stub = C.jump(Dest());
    C[Stub].Detached = true;
    int One = C.imm(Dupr, 1);
    C[One].Detached = true;
    int Zero = C.imm(Dupr, 0);
    int Store = C.store(D);
    C[One].D = Dest::item(Store);
    // Beql's primary result means the operands differ.
    C[Test].D = Dest::item(CC == Squall::NE ? One : Zero);
    C[Stub].D = Dest::item(CC == Squall::NE ? Zero : One);
    return;
  }
  // a < b is the borrow of a - b, computed on values whose order as 64-bit
  // unsigned numbers matches the requested comparison. Registers are
  // zero-extended, which orders them as unsigned; a signed comparison first
  // maps each value to value + 2^31 (flipping bit 31), kept zero-extended so
  // that it can be stored.
  bool Swap = CC == Squall::GT || CC == Squall::LE || CC == Squall::UGT ||
              CC == Squall::ULE;
  bool Invert = CC == Squall::LE || CC == Squall::GE || CC == Squall::ULE ||
                CC == Squall::UGE;
  bool Signed = CC == Squall::LT || CC == Squall::LE || CC == Squall::GT ||
                CC == Squall::GE;
  if (Swap)
    std::swap(A, B);
  unsigned Right = B;
  auto Bias = [&] {
    C.imm(Xor, INT32_MIN);
    C.zeroExtend32();
  };
  if (Signed) {
    C.read(B);
    Bias();
    C.store(C.scratch());
    Right = C.scratch();
  }
  C.read(A);
  if (Signed)
    Bias();
  C.captureSecondary(C.slotOp(Sub, FrameSlot, Right));
  if (Invert)
    C.imm(Xor, 1);
  C.store(D);
}

// Loads and stores through a pointer. A helper word placed in the chain is
// sent a token whose frame pointer is the target word, so it can address any
// memory word as frame slot 0, then return through a saved continuation tag.
// The pointer is checked for alignment, the reserved low words, and the
// physical memory bound before the access. The mailboxes are shared, which
// is safe because expanded code runs with a single control token.
void SquallAsmPrinter::expandMemory(Chain &C, const MachineInstr &MI,
                                    bool IsStore,
                                    std::optional<unsigned> PointerReg,
                                    int64_t Pointer) {
  const unsigned ReturnBox = FirstMailbox, ValueBox = FirstMailbox + 1,
                 HelperBox = FirstMailbox + 2, PointerBox = FirstMailbox + 3;
  int SaveReturn = C.extract(Dest::end()); // Patched below for loads.
  C.storeAbs(ReturnBox);
  if (IsStore) {
    C.read(word(MI.getOperand(0)));
    C.storeAbs(ValueBox);
  }
  int HelperTag = C.extract(Dest::end()); // Patched below.
  C.imm(And, (INT64_C(1) << 29) - 1);     // Keep the helper IP only.
  C.storeAbs(HelperBox);
  if (PointerReg)
    C.read(*PointerReg);
  else
    C.imm(Dupr, Pointer);
  C.storeAbs(PointerBox);

  C.imm(And, 3);
  C.branch(Dest::symbol(MemoryError));
  int Stub = C.jump(Dest());
  C[Stub].Detached = true;
  C[Stub].D = Dest::item(C.readAbs(PointerBox));
  // Below the globals are the runtime's words, including the call join
  // words, which no pointer may reach.
  C.captureSecondary(C.imm(Sub, GlobalBase * 4));
  C.branch(Dest::symbol(MemoryError)); // Borrow: a reserved low word.
  Stub = C.jump(Dest());
  C[Stub].Detached = true;
  C[Stub].D = Dest::item(C.readAbs(PointerBox));
  // The frame arena is the last region a pointer may name. Above it is the
  // token stack: a load there reads tokens, and a store overwrites one (or
  // parks on a word whose presence bits are empty and never returns).
  uint64_t Limit = ArenaEnd * 4; // Logical byte address of the arena's end.
  int Check;
  if (Limit < UINT64_C(1) << 31) {
    Check = C.imm(Sub, Limit);
  } else {
    Check = C.slotOp(Sub, CodeSlot, 0);
  }
  C.captureSecondary(Check);
  C.branch(Dest()); // Borrow: in bounds, continue below.
  Stub = C.jump(Dest::symbol(MemoryError));
  C[Stub].Detached = true;

  C.readAbs(PointerBox);
  // Logical byte address << 30 == word << 32, the FP field.
  C.imm(Shl, TagFPShift - 2);
  C.slotOp(Or, AbsoluteSlot, HelperBox);
  C.send(AbsoluteSlot, ValueBox, Left);

  int Helper;
  if (IsStore) {
    Helper = C.slotOp(Dupr, AbsoluteSlot, ValueBox);
    C[Helper].Detached = true;
    int Write = C.store(0);
    C[Write].Detached = true;
    C[Helper].D = Dest::item(Write);
  } else {
    Helper = C.read(0);
    C[Helper].Detached = true;
  }
  int Return = C.send(AbsoluteSlot, ReturnBox, Right);
  C[Return].Detached = true;
  C[IsStore ? Helper + 1 : Helper].D = Dest::item(Return);
  if (!IsStore) {
    int Store = C.store(word(MI.getOperand(0)));
    C[SaveReturn].Target = Dest::item(Store);
  }
  C[HelperTag].Target = Dest::item(Helper);
  if (Check >= 0 && C[Check].Opcode == Squall::ALUS) {
    int Data = C.data(constantSlot(Limit));
    C[Check].CodeData = Data;
  }
}

// The caller places the callee's frame just past its own: it checks that the
// new frame fits in the arena, writes the arguments and its continuation tag
// into the callee frame, and sends a token to the callee with the frame
// pointer advanced. The callee returns by sending its result to that tag.
// An indirect call builds the callee's tag from a function pointer, the
// entry's word address, instead of minting it.
void SquallAsmPrinter::expandCall(Chain &C, const MachineInstr &MI) {
  unsigned FrameWords =
      MF->getInfo<SquallMachineFunctionInfo>()->getFrameWords();
  unsigned Opcode = MI.getOpcode();
  bool HasResult = Opcode == Squall::CALLR || Opcode == Squall::CALLIR;
  bool Indirect = Opcode == Squall::CALLI || Opcode == Squall::CALLIR;
  unsigned CalleeOp = HasResult ? 1 : 0;

  // A null function pointer is word 0, the discard destination: sending to
  // it would end the program with no result and no status. Trap instead.
  // (Bits 29-31 of a pointer fall in the tag's unused bits, and registers
  // hold nothing above bit 31, so no other masking is needed.)
  if (Indirect) {
    C.read(word(MI.getOperand(CalleeOp)));
    C.branch(Dest());
    int Null = C.jump(Dest::symbol(Trap));
    C[Null].Detached = true;
  }

  // New FP + MaxSlotWord must stay inside the arena: the callee may address
  // any word up to its FP + MaxSlotWord with a frame slot.
  int64_t Limit = int64_t(ArenaEnd) - (FrameWords + Squall::MaxSlotWord);
  C.extract(Dest::symbol(Halt));
  C.imm(Shrl, TagFPShift); // The frame pointer of the current token.
  C.captureSecondary(C.imm(Sub, Limit));
  C.branch(Dest());
  int Stub = C.jump(Dest::symbol(DepthError));
  C[Stub].Detached = true;

  unsigned Arg = 0;
  for (unsigned I = CalleeOp + 1, E = MI.getNumOperands(); I < E; ++I) {
    const MachineOperand &MO = MI.getOperand(I);
    if (!MO.isReg() || MO.isImplicit())
      continue;
    C.read(word(MO));
    C.store(FrameWords + Squall::FirstRegWord + Arg++);
  }
  int SaveReturn = C.extract(Dest::end());
  C.store(FrameWords + Squall::ReturnTagWord);
  if (Indirect) {
    // Callee tag: the pointer's entry word with the callee's frame pointer.
    C.extract(Dest::symbol(Halt));
    C.imm(Shrl, TagFPShift);
    C.imm(Add, FrameWords);
    C.imm(Shl, TagFPShift);
    C.slotOp(Or, FrameSlot, word(MI.getOperand(CalleeOp)));
    C.send(FrameSlot, Squall::ReturnTagWord, Left);
  } else {
    const auto *Callee = cast<Function>(MI.getOperand(CalleeOp).getGlobal());
    C.extract(Dest::symbol(entryLabel(*Callee)));
    int Advance = C.slotOp(Add, CodeSlot, 0);
    C.send(FrameSlot, Squall::ReturnTagWord, Left);
    C[Advance].CodeData =
        C.data(constantSlot(int64_t(FrameWords) << TagFPShift));
  }
  if (HasResult) {
    int Store = C.store(word(MI.getOperand(0)));
    C[SaveReturn].Target = Dest::item(Store);
  }
}

// Alignment padding before a function or block is emitted by the generic
// printer, not through chains. Count the most it can be, so the frame arena
// always starts past the code; the exact amount depends on the assembler.
void SquallAsmPrinter::countPadding(Align Alignment) {
  if (Alignment.value() > 8)
    CodeWords += Alignment.value() / 8 - 1;
}

// Padding is not executable: a word forwards a token on one fixed port, and
// fills the other operand with its immediate, so no padding word passes every
// token on unchanged. Code that would fall into an aligned block through its
// padding jumps over it to the block's label instead.
void SquallAsmPrinter::emitBasicBlockStart(const MachineBasicBlock &MBB) {
  if (MBB.getAlignment() > Align(8)) {
    if (const MachineBasicBlock *Prev = MBB.getPrevNode();
        Entry && Prev && Prev->isSuccessor(&MBB)) {
      Chain Over(Halt);
      Over.jump(Dest::symbol(MBB.getSymbol()));
      Over.emit(*OutStreamer, TM.getMCSubtargetInfo(), OutContext);
      CodeWords += Over.size();
    }
    countPadding(MBB.getAlignment());
  }
  AsmPrinter::emitBasicBlockStart(MBB);
  // Calls enter at the first block itself, after any padding before it.
  if (&MBB == &MF->front() && MBB.getAlignment() > Align(8))
    OutStreamer->emitLabel(entryLabel(MF->getFunction()));
}

void SquallAsmPrinter::emitFunctionEntryLabel() {
  // The function's alignment was emitted just before its label: up to its
  // preferred alignment (a .prefalign), which includes the minimum.
  countPadding(std::max(MF->getPreferredAlignment(),
                        MF->getFunction().getAlign().valueOrOne()));
  AsmPrinter::emitFunctionEntryLabel();
  // An aligned first block is padded after this point; emitBasicBlockStart
  // places the entry there instead.
  if (MF->front().getAlignment() <= Align(8))
    OutStreamer->emitLabel(entryLabel(MF->getFunction()));
}

void SquallAsmPrinter::emitInstruction(const MachineInstr *MI) {
  if (!Entry)
    return; // The image layout was rejected; an error is already reported.
  switch (MI->getOpcode()) {
  case Squall::FORK:
  case Squall::LANE:
  case Squall::JOIN:
    return emitLaneMarker(*MI);
  default:
    // A volatile load must read memory even when the preceding token happens
    // to carry the value last stored at that address.
    append(expand(*MI, Scratch), !MI->hasOrderedMemoryRef());
  }
}

Chain SquallAsmPrinter::expand(const MachineInstr &Instr, unsigned Scratch) {
  const MachineInstr *MI = &Instr;
  Chain C(Halt, Scratch);
  auto Op = [&](unsigned I) -> const MachineOperand & {
    return MI->getOperand(I);
  };
  switch (MI->getOpcode()) {
  case Squall::MOVrr:
    C.read(word(Op(1)));
    C.store(word(Op(0)));
    break;
  case Squall::MOVi:
    C.imm(Dupr, Op(1).getImm());
    if (Op(1).getImm() < 0)
      C.zeroExtend32(); // The literal is sign-extended to 64 bits.
    C.store(word(Op(0)));
    break;
  case Squall::MOVG:
    if (const auto *F = dyn_cast<Function>(Op(1).getGlobal())) {
      // A function pointer is the entry's word address: the instruction field
      // of a tag minted for the entry.
      C.extract(Dest::symbol(entryLabel(*F)));
      C.imm(And, TagIPMask);
    } else if (const auto *G = dyn_cast<GlobalVariable>(Op(1).getGlobal());
               G && GlobalWords.count(G)) {
      // A byte address: the offset need not be a whole cell.
      C.imm(Dupr, int64_t(GlobalWords.lookup(G)) * 4 + Op(1).getOffset());
    } else {
      // Aliases are resolved and other globals rejected before this point;
      // never let an unplaced global become address 0.
      error(Twine("'") + Op(1).getGlobal()->getName() +
            "' has no place in the image");
      C.imm(Dupr, 0);
    }
    C.store(word(Op(0)));
    break;
  case Squall::FIADDR:
    C.extract(Dest::symbol(Halt));
    C.imm(Shrl, TagFPShift);
    C.imm(Add, Op(1).getImm());
    C.imm(Shl, 2);
    C.store(word(Op(0)));
    break;
  case Squall::ADDrr:
  case Squall::ADDri:
    expandBinary(C, *MI, Add, true);
    break;
  case Squall::SUBrr:
  case Squall::SUBri:
    expandBinary(C, *MI, Sub, true);
    break;
  case Squall::MULrr:
  case Squall::MULri:
    expandBinary(C, *MI, Mul, true);
    break;
  case Squall::SDIVrr:
  case Squall::SDIVri:
    expandSigned(C, *MI, Div);
    break;
  case Squall::SREMrr:
  case Squall::SREMri:
    expandSigned(C, *MI, Rem);
    break;
  // Zero-extended operands are nonnegative, so a signed remainder of them is
  // the unsigned one.
  case Squall::UDIVrr:
  case Squall::UDIVri:
    expandBinary(C, *MI, Divu, false);
    break;
  case Squall::UREMrr:
  case Squall::UREMri:
    expandBinary(C, *MI, Rem, false);
    break;
  case Squall::ANDrr:
  case Squall::ANDri:
    expandBinary(C, *MI, And, false);
    break;
  case Squall::ORrr:
  case Squall::ORri:
    expandBinary(C, *MI, Or, false);
    break;
  case Squall::XORrr:
  case Squall::XORri:
    expandBinary(C, *MI, Xor, false);
    break;
  case Squall::MULHS:
    expandSigned(C, *MI, Mul);
    break;
  case Squall::MULHU:
    // The exact 64-bit product of two zero-extended values.
    C.read(word(Op(1)));
    C.slotOp(Mul, FrameSlot, word(Op(2)));
    C.imm(Shrl, 32);
    C.store(word(Op(0)));
    break;
  case Squall::SHLrr:
  case Squall::SHLri:
    expandBinary(C, *MI, Shl, true);
    break;
  case Squall::SRArr:
  case Squall::SRAri:
    expandShiftRightArith(C, *MI);
    break;
  case Squall::SRLrr:
  case Squall::SRLri:
    expandBinary(C, *MI, Shrl, false);
    break;
  case Squall::SETCC:
    expandCompare(C, *MI);
    break;
  case Squall::SELECT: {
    C.read(word(Op(1)));
    C.branch(Dest());
    int Stub = C.jump(Dest());
    C[Stub].Detached = true;
    int True = C.read(word(Op(2)));
    C[True].Detached = true;
    int False = C.read(word(Op(3)));
    int Store = C.store(word(Op(0)));
    C[True - 2].D = Dest::item(True); // The Beqz test.
    C[Stub].D = Dest::item(False);
    C[True].D = Dest::item(Store);
    break;
  }
  case Squall::LDF:
    C.read(Op(1).getImm());
    C.store(word(Op(0)));
    break;
  case Squall::STF:
    C.read(word(Op(0)));
    C.store(Op(1).getImm());
    break;
  case Squall::LDG: {
    unsigned W = globalWord(Op(1));
    if (W <= Squall::MaxSlotWord) {
      C.readAbs(W);
      C.store(word(Op(0)));
    } else {
      expandMemory(C, *MI, false, std::nullopt, int64_t(W) * 4);
    }
    break;
  }
  case Squall::STG: {
    unsigned W = globalWord(Op(1));
    if (W <= Squall::MaxSlotWord) {
      C.read(word(Op(0)));
      C.storeAbs(W);
    } else {
      expandMemory(C, *MI, true, std::nullopt, int64_t(W) * 4);
    }
    break;
  }
  case Squall::LDP:
    expandMemory(C, *MI, false, word(Op(1)), 0);
    break;
  case Squall::STP:
    expandMemory(C, *MI, true, word(Op(1)), 0);
    break;
  case Squall::BR:
    C.jump(Dest::symbol(Op(0).getMBB()->getSymbol()));
    break;
  case Squall::BRNZ:
    C.read(word(Op(0)));
    C.branch(Dest::symbol(Op(1).getMBB()->getSymbol()));
    break;
  case Squall::BRZ: {
    C.read(word(Op(0)));
    C.branch(Dest::end());
    int Stub = C.jump(Dest::symbol(Op(1).getMBB()->getSymbol()));
    C[Stub].Detached = true;
    break;
  }
  case Squall::CALL:
  case Squall::CALLR:
  case Squall::CALLI:
  case Squall::CALLIR:
    expandCall(C, *MI);
    break;
  case Squall::TRAP:
    C.jump(Dest::symbol(Trap));
    break;
  case Squall::EXIT:
    C.read(word(Op(0)));
    C.jump(Dest::symbol(OutContext.getOrCreateSymbol("__squall_host_return")));
    break;
  case Squall::RET:
    C.imm(Dupr, 0);
    C.send(FrameSlot, Squall::ReturnTagWord, Right);
    break;
  case Squall::RETV:
    C.read(word(Op(0)));
    C.send(FrameSlot, Squall::ReturnTagWord, Right);
    break;
  default:
    llvm_unreachable("unexpected machine instruction");
  }
  assert(C.size() && C.entry() == Left && "chains are entered on the left");
  return C;
}

void SquallAsmPrinter::append(Chain C, bool ForwardRead) {
  if (Pending) {
    if (std::optional<unsigned> Word = Pending->storedWord();
        ForwardRead && Word && C.readsFirst(*Word))
      C.dropFirst();
    flush(C.entry());
  }
  Pending = std::move(C);
}

void SquallAsmPrinter::emitBasicBlockEnd(const MachineBasicBlock &MBB) {
  flush(Left);
  AsmPrinter::emitBasicBlockEnd(MBB);
}

// FORK id; lane A; LANE id; lane B; JOIN id becomes
//
//   fork:    dupl laneB.l #0, secondary to lane A at the next word
//   lane A:  ...; dupl join.l #0
//   lane B:  ...; falling into join on the right
//   join:    dupl (Once) [JoinWord]
//
// The first token to reach the join fills the empty join slot and stops; the
// second empties it again and continues. Lanes without calls share one join
// word, since at most one such join is pending at a time; a lane that calls
// joins on a reserved word of its own (see SquallForkJoin). Each lane has its
// own scratch word.
void SquallAsmPrinter::emitLaneMarker(const MachineInstr &MI) {
  auto &[LaneB, Join] = Lanes[MI.getOperand(0).getImm()];
  if (!LaneB) {
    LaneB = OutContext.createTempSymbol("squall_lane");
    Join = OutContext.createTempSymbol("squall_join");
  }
  switch (MI.getOpcode()) {
  case Squall::FORK: {
    Chain Fork(Halt);
    int Split = Fork.jump(Dest::symbol(LaneB));
    Fork[Split].Secondary = true;
    append(std::move(Fork));
    Forked = true;
    return;
  }
  case Squall::LANE: {
    Chain ToJoin(Halt);
    ToJoin.jump(Dest::symbol(Join));
    append(std::move(ToJoin));
    flush(Left);
    OutStreamer->emitLabel(LaneB);
    Scratch = Squall::LaneScratchWord;
    return;
  }
  default: {
    flush(Right);
    unsigned Index = MI.getOperand(1).getImm();
    unsigned Word = Index ? FirstGlobalWord + Index - 1 : JoinWord;
    UsedJoinWords.insert(Word);
    Chain Meet(Halt);
    int Slot = Meet.slotOp(Dupl, AbsoluteSlot, Word, Once);
    Meet[Slot].Label = Join;
    Pending = std::move(Meet);
    Scratch = Squall::ScratchWord;
    return;
  }
  }
}

json::Object SquallAsmPrinter::manifest(const Module &M) const {
  auto TypeName = [](const Type *T) -> std::string {
    if (T->isVoidTy())
      return "void";
    if (T->isPointerTy())
      return "ptr";
    return "i" + std::to_string(T->getIntegerBitWidth());
  };
  // A narrow argument arrives in a 32-bit register, and the entry function
  // relies on its extension attribute: the loader truncates and extends it.
  json::Array ArgumentWords, ArgumentTypes, ArgumentExtensions;
  for (const Argument &A : Entry->args()) {
    ArgumentWords.push_back(ArgumentWord + A.getArgNo());
    ArgumentTypes.push_back(TypeName(A.getType()));
    ArgumentExtensions.push_back(A.hasZExtAttr()   ? "zext"
                                 : A.hasSExtAttr() ? "sext"
                                                   : "none");
  }
  json::Object Globals;
  for (const GlobalVariable &G : M.globals())
    if (auto It = GlobalWords.find(&G); It != GlobalWords.end())
      Globals[G.getName()] = It->second;
  // Provenance: which compiler built the image, and which RTL revision its
  // encoding, tag layout and slot semantics are those of. A loader can then
  // refuse an image built for another core.
  std::string Compiler = "LLVM " LLVM_VERSION_STRING;
#ifdef LLVM_REVISION
  Compiler += " " LLVM_REVISION;
#endif
  return json::Object{
      {"format", "squall-v2-image-2"},
      {"abi", "squall-frame-slots-2"},
      {"compiler", Compiler},
      {"isa", json::Object{{"repository", "isacomp"},
                           {"branch", "squall-v2-rebase"},
                           {"commit", Squall::IsaCommit}}},
      // What the hardware must be given besides the memory contents: the
      // reset values of the two stack pointer registers, and the memory size
      // the image fills. Every word of memory must be loaded: the frame
      // arena is not zero (see frame_arena_fill).
      {"reset",
       json::Object{{"stack_base", StackBase}, {"stack_top", StackBase - 16}}},
      // Words [root_frame_word + 1, frame_arena_end_word) all hold this
      // value, an empty constant slot, so a loader may fill them instead of
      // transferring them. A hexadecimal string: it exceeds 2^53.
      {"frame_arena_fill", "0x8000000000000000"},
      {"entry", Entry->getName()},
      {"argument_words", std::move(ArgumentWords)},
      {"argument_types", std::move(ArgumentTypes)},
      {"argument_extensions", std::move(ArgumentExtensions)},
      {"return_type", TypeName(Entry->getReturnType())},
      {"memory_bytes", MemoryBytes.getValue()},
      {"word_bytes", 8},
      {"stack_base", StackBase},
      {"stack_top", StackBase - 16},
      {"stack_token_capacity", StackTokens.getValue()},
      {"initial_token_count", 1},
      {"root_frame_word", FrameBase},
      {"frame_arena_end_word", ArenaEnd},
      {"code_start_word", CodeStart},
      {"start_word", StartWord},
      {"host_return_word", HostReturnWord},
      {"result_word", ResultWord},
      {"status_word", StatusWord},
      {"status_codes", json::Object{{"success", StatusSuccess},
                                    {"call_depth_exceeded", StatusDepth},
                                    {"invalid_memory_address", StatusMemory},
                                    {"trap", StatusTrap}}},
      {"max_cycles", 1000000},
      // Parallel lanes may interleave differently on different cores; only
      // the multiset of executed tokens is fixed.
      {"parallel_lanes", Forked},
      {"join_words",
       json::Array(UsedJoinWords.empty() ? std::set<unsigned>{JoinWord}
                                         : UsedJoinWords)},
      {"globals", std::move(Globals)},
      {"memory_model",
       json::Object{{"pointer_bits", 32},
                    {"logical_cell_bytes", 4},
                    {"physical_cell_bytes", 8},
                    {"pointer_to_word_shift", 2},
                    {"runtime_mailbox_words",
                     json::Array{FirstMailbox, FirstMailbox + 1,
                                 FirstMailbox + 2, FirstMailbox + 3}}}}};
}

void SquallAsmPrinter::emitEndOfAsmFile(Module &M) {
  // A module left without code still gets its header, which reports the
  // missing entry function instead of producing an object with no image.
  if (!LaidOut) {
    LaidOut = true;
    emitImageHeader(M);
  }
  if (!Entry)
    return;
  OutStreamer->switchSection(imageSection());
  FrameBase = std::max(FrameBase, alignTo(CodeStart + CodeWords, 4));
  // The arena must hold at least one frame's reach (MaxSlotWord + 1 words).
  if (FrameBase + Squall::MaxSlotWord + 1 > ArenaEnd) {
    error(Twine("the program's code and globals take ") +
          Twine((CodeStart + CodeWords) * 8 / 1024) +
          " KiB, leaving no room "
          "for frames in the " +
          Twine(MemoryBytes / 1024) + " KiB image; use -squall-memory-bytes=" +
          Twine(suggestedMemory(CodeStart + CodeWords)) + " or more");
    return;
  }
  // Frame arena: the root frame's return tag names the host return stub, and
  // every other word starts as an empty constant slot.
  OutStreamer->emitValueToOffset(
      MCConstantExpr::create(FrameBase * 8, OutContext), 0, SMLoc());
  OutStreamer->emitIntValue(constantSlot(HostReturnWord), 8);
  // A Const store into an empty slot consumes its token, so every frame word
  // must start as a constant. `.fill` cannot repeat an eight-byte pattern.
  uint64_t Count = ArenaEnd - FrameBase - 1;
  if (OutStreamer->hasRawTextSupport()) {
    OutStreamer->emitRawText(Twine("\t.rept\t") + Twine(Count));
    OutStreamer->emitRawText(Twine("\t.quad\t") +
                             Twine(int64_t(constantSlot(0))));
    OutStreamer->emitRawText("\t.endr");
  } else {
    for (uint64_t I = 0; I < Count; ++I)
      OutStreamer->emitIntValue(constantSlot(0), 8);
  }
  // The token stack holds the start token, a packed tag (instruction pointer,
  // frame pointer, port) and its value.
  OutStreamer->emitValueToOffset(MCConstantExpr::create(StackBase, OutContext),
                                 0, SMLoc());
  OutStreamer->emitIntValue(StartWord | FrameBase << TagFPShift, 8);
  OutStreamer->emitIntValue(0, 8);

  MCSection *Config =
      OutContext.getELFSection(".squall.config", ELF::SHT_PROGBITS, 0);
  OutStreamer->switchSection(Config);
  std::string Text;
  raw_string_ostream OS(Text);
  OS << json::Value(manifest(M));
  OutStreamer->emitBytes(Text);
}

char SquallAsmPrinter::ID = 0;

INITIALIZE_PASS(SquallAsmPrinter, "squall-asm-printer",
                "Squall Assembly Printer", false, false)

extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void
LLVMInitializeSquallAsmPrinter() {
  RegisterAsmPrinter<SquallAsmPrinter> X(getTheSquallTarget());
}
