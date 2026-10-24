"""A conservative, control-token LLVM lowering for Squall v2.

SSA values live in per-activation Constant slots. A control token sequences IR
effects; it is never split across successor basic blocks. This deliberately
favors a checkable baseline over parallel scheduling and register allocation.
"""
from __future__ import annotations

import hashlib
import json
import re
from dataclasses import dataclass, field
from pathlib import Path

import llvmlite.binding as llvm

from .assembler import Program, assemble, slot_word, tag_word, token_words
from .memory import parse_type, initializer


class CompileError(ValueError):
    pass


def bits(ty):
    if str(ty) == "ptr":
        return 32
    if str(ty) not in ("i1", "i32"):
        raise CompileError(f"unsupported value type {ty}; supported integers are i1 and i32")
    return int(str(ty)[1:])


def canonical(value, width):
    value &= (1 << width) - 1
    return value - (1 << width) if width == 32 and value & (1 << 31) else value


@dataclass
class Function:
    value: object
    index: int
    slots: dict = field(default_factory=dict)
    pointers: dict = field(default_factory=dict)
    instructions: dict = field(default_factory=dict)
    blocks: dict = field(default_factory=dict)
    count: int = 1  # slot zero is the packed return tag
    scratch: int = 0
    phi_scratch: list = field(default_factory=list)
    gep_scratch: int = 0

    def allocate(self, value=None, cells=1, alignment=1):
        self.count = (self.count + alignment - 1) // alignment * alignment
        result = self.count
        self.count += cells
        if value is not None:
            self.slots[value] = result
        return result

    @property
    def entry(self):
        return f"fn{self.index}"


class Compiler:
    def __init__(self, ir, entry="main", arguments=(), *, memory_bytes=512 * 1024,
                 frames=32, stack_tokens=1024):
        try:
            self.module = llvm.parse_assembly(ir)
            self.module.verify()
        except RuntimeError as exc:
            raise CompileError(f"invalid LLVM IR: {exc}") from exc
        self.ir = ir
        self.entry_name = entry
        self.arguments = list(arguments)
        self.memory_bytes = memory_bytes
        self.frames = frames
        self.stack_tokens = stack_tokens
        self.program = Program()
        self.serial = 0
        self.functions = {}
        self.globals = {}
        self.initial = {1: 0, 2: 0}
        self.global_next = 16
        self.context = None
        self.memory_helpers = False
        self._layout()

    def unique(self, stem="v"):
        self.serial += 1
        return f"{stem}{self.serial}"

    def emit(self, kind, *, port="L", **fields):
        label = self.unique()
        self.program.add(label, kind, **fields)
        return f"{label}.{port}"

    def jump(self, label, dest):
        self.program.add(label, "monadic", op="Dupl", dest=dest)

    def constant(self, value, dest):
        return self.emit("monadic", op="Dupr", imm=value, dest=dest)

    def normalize(self, width, dest, unsigned=False):
        if width == 1:
            return self.emit("monadic", op="And", imm=1, dest=dest)
        last = self.emit("monadic", op="Shrl" if unsigned else "Shra", imm=32, dest=dest)
        return self.emit("monadic", op="Shl", imm=32, dest=last)

    def read_slot(self, addr, dest):
        return self.emit("dyadic", op="Dupr", addr=addr, match="Read", dest=dest)

    def store_slot(self, addr, dest):
        return self.emit("dyadic", port="R", op="Dupr", addr=addr, match="Const", dest=dest)

    def read(self, value, dest):
        if value.value_kind.name == "constant_pointer_null":
            return self.constant(0, dest)
        if value in self.globals:
            return self.constant(int(self.globals[value].split(":")[1]) * 4, dest)
        address = self.static_pointer(value)
        if address is not None:
            mode, offset = address.split(":")
            if mode == "abs":
                return self.constant(int(offset) * 4, dest)
            add = self.emit("monadic", op="Add", imm=int(offset),
                            dest=self.emit("monadic", op="Shl", imm=2, dest=dest))
            shift = self.emit("monadic", op="Shrl", imm=29, dest=add)
            return self.emit("extract", target="halt.L", addr="frame:0", match="Read", dest=shift)
        if value.value_kind.name == "constant_int":
            return self.constant(canonical(value.get_constant_value(), bits(value.type)), dest)
        if value not in self.context.slots:
            raise CompileError(f"unsupported operand: {value}")
        return self.read_slot(f"frame:{self.context.slots[value]}", dest)

    def save(self, value, dest):
        store = self.store_slot(f"frame:{self.context.slots[value]}", dest)
        return self.normalize(bits(value.type), store, unsigned=str(value.type) == "ptr")

    def literal_slot(self, value, *, op="Dupr", dest):
        label, data = self.unique(), self.unique("constant")
        self.program.add(label, "dyadic", op=op, addr=f"code:{data}", match="Read", dest=dest)
        self.program.add(data, ".slot", value=value)
        return f"{label}.L"

    def secondary(self, op, dest, *, addr=None, imm=None):
        label, capture, resume = self.unique(), self.unique("secondary"), self.unique("resume")
        # The secondary token executes first on the LIFO machine. Store its
        # value and drain its halt token before the primary token resumes.
        # Sending the primary to halt instead would accumulate one pending
        # token per comparison, eventually overflowing on a long-running loop.
        fields = dict(op=op, dest=f"{resume}.L", second=f"{capture}.R")
        if addr is not None:
            fields.update(addr=addr, match="Read")
        else:
            fields["imm"] = imm
        self.program.add(label, "dyadic" if addr is not None else "monadic", **fields)
        slot = f"frame:{self.context.scratch}"
        self.program.add(capture, "dyadic", op="Dupr", addr=slot, match="Const", dest="halt.L")
        self.program.add(resume, "dyadic", op="Dupr", addr=slot, match="Read", dest=dest)
        return f"{label}.L"

    def branch(self, true_dest, false_dest):
        label, stub = self.unique(), self.unique("false")
        self.program.add(label, "monadic", op="Beqz", imm=0,
                         dest=true_dest, second=f"{stub}.L")
        self.jump(stub, false_dest)
        return f"{label}.L"

    def operands(self, a, b, action, *, unsigned=False, flip_sign=False):
        # The scratch is not a source-level value, so saving b cannot clobber a.
        sign_mask = 1 if bits(a.type) == 1 else -(1 << 31)
        if flip_sign:
            action = self.emit("monadic", op="Xor", imm=sign_mask, dest=action)
        if unsigned:
            action = self.normalize(32, action, unsigned=True)
        first = self.read(a, action)
        store = self.store_slot(f"frame:{self.context.scratch}", first)
        if flip_sign:
            store = self.emit("monadic", op="Xor", imm=sign_mask, dest=store)
        if unsigned:
            store = self.normalize(32, store, unsigned=True)
        return self.read(b, store)

    def binary(self, inst, dest):
        mapping = {"add": "Add", "sub": "Sub", "mul": "Mul", "sdiv": "Div",
                   "udiv": "Divu", "srem": "Rem", "urem": "Rem", "and": "And",
                   "or": "Or", "xor": "Xor", "shl": "Shl", "lshr": "Shrl", "ashr": "Shra"}
        op = mapping[inst.opcode]
        a, b = list(inst.operands)
        action = self.emit("dyadic", op=op, addr=f"frame:{self.context.scratch}",
                           match="Read", dest=dest)
        return self.operands(a, b, action, unsigned=inst.opcode in ("udiv", "urem", "lshr"))

    def compare(self, inst, dest):
        predicate = re.search(r"\bicmp\s+(\w+)\s", str(inst))[1]
        a, b = list(inst.operands)
        if predicate in ("eq", "ne"):
            yes, no = self.constant(1, dest), self.constant(0, dest)
            unequal, equal = (yes, no) if predicate == "ne" else (no, yes)
            label, stub = self.unique(), self.unique("equal")
            self.program.add(label, "dyadic", op="Beql", addr=f"frame:{self.context.scratch}",
                             match="Read", dest=unequal, second=f"{stub}.L")
            self.jump(stub, equal)
            return self.operands(a, b, f"{label}.L")
        if predicate not in ("ult", "ule", "ugt", "uge", "slt", "sle", "sgt", "sge"):
            raise CompileError(f"unsupported comparison {predicate}")
        if predicate.endswith(("gt", "le")):
            a, b = b, a
        if predicate.endswith(("le", "ge")):
            dest = self.emit("monadic", op="Xor", imm=1, dest=dest)
        action = self.secondary("Sub", dest, addr=f"frame:{self.context.scratch}")
        return self.operands(a, b, action, flip_sign=predicate.startswith("s"))

    def _layout(self):
        if self.memory_bytes < 4096 or self.memory_bytes % 16 or self.memory_bytes > 1 << 32:
            raise CompileError("memory size must be a multiple of 16 bytes in [4096, 2^32]")
        if self.frames < 1 or self.stack_tokens < 1:
            raise CompileError("frames and stack token capacity must be positive")
        alignments = [self.storage_alignment(str(g)) for g in self.module.global_variables]
        alignments.extend(self.storage_alignment(str(i)) for f in self.module.functions
                          for b in f.blocks for i in b.instructions if i.opcode == "alloca")
        self.storage_alignment_cells = max(alignments, default=1)
        frame_hint = self.memory_bytes // 4 // 8
        self.frame_base = ((frame_hint + self.storage_alignment_cells - 1) // self.storage_alignment_cells
                           * self.storage_alignment_cells)
        self.data_base = self.frame_base // 2
        self.data_next = self.data_base
        for glob in self.module.global_variables:
            try:
                ty, rest = parse_type(str(glob.global_value_type))
                if rest:
                    raise ValueError("unsupported global storage type")
                if ty.cells > self.frame_base - self.data_base:
                    raise ValueError("global array exceeds the available memory arena")
                match = re.search(r"\b(?:global|constant)\s+", str(glob))
                if not match:
                    raise ValueError("requires an integer initializer")
                actual, rest = parse_type(str(glob)[match.end():])
                values, _ = initializer(actual, rest)
            except ValueError as exc:
                raise CompileError(f"global @{glob.name}: {exc}") from exc
            alignment = self.storage_alignment(str(glob))
            if ty.element:
                self.data_next = (self.data_next + alignment - 1) // alignment * alignment
                address = self.data_next
                self.data_next += ty.cells
            else:
                self.global_next = (self.global_next + alignment - 1) // alignment * alignment
                address = self.global_next
                self.global_next += 1
            self.globals[glob] = f"abs:{address}"
            for index, value in enumerate(values):
                self.initial[address + index] = slot_word(canonical(value, 32))
        if self.data_next > self.frame_base:
            raise CompileError("global array arena overlaps the frame arena")
        for fn in self.module.functions:
            if fn.is_declaration:
                continue
            signature = fn.global_value_type
            if signature.is_function_vararg:
                raise CompileError(f"@{fn.name}: variadic functions are unsupported")
            ret = str(signature.get_function_return())
            if ret != "void":
                bits(signature.get_function_return())
            context = Function(fn, len(self.functions))
            self.functions[fn.name] = context
            for arg in fn.arguments:
                bits(arg.type)
                context.allocate(arg)
            for bi, block in enumerate(fn.blocks):
                context.blocks[block] = f"f{context.index}_b{bi}"
                for ii, inst in enumerate(block.instructions):
                    context.instructions[inst] = f"f{context.index}_b{bi}_i{ii}"
                    if str(inst.type) in ("i1", "i32", "ptr") and inst.opcode != "alloca":
                        context.allocate(inst)
                    elif str(inst.type) not in ("void", "ptr"):
                        raise CompileError(f"@{fn.name}: unsupported result type {inst.type}: {inst}")
                    if inst.opcode == "alloca":
                        if bi:
                            raise CompileError("alloca outside the entry block requires dynamic frame storage and is unsupported")
                        try:
                            ty, _ = parse_type(str(inst).split("alloca", 1)[1])
                        except ValueError as exc:
                            raise CompileError(str(exc)) from exc
                        operands = list(inst.operands)
                        if not operands or operands[0].value_kind.name != "constant_int":
                            raise CompileError(f"variable alloca is unsupported: {inst}")
                        count = operands[0].get_constant_value()
                        if not 1 <= count <= 1024 or ty.cells * count > 1024:
                            raise CompileError("alloca size must be in [1, 1024] memory cells")
                        context.pointers[inst] = f"frame:{context.allocate(cells=ty.cells * count, alignment=self.storage_alignment(str(inst)))}"
            context.scratch = context.allocate()
            if any(i.opcode == "getelementptr" or (i.opcode == "call" and list(i.operands)[-1].name.startswith("llvm.mem")) for b in fn.blocks for i in b.instructions):
                context.gep_scratch = context.allocate()
            max_phi = max((sum(i.opcode == "phi" for i in b.instructions) for b in fn.blocks), default=0)
            context.phi_scratch = [context.allocate() for _ in range(max_phi)]
        if self.entry_name not in self.functions:
            raise CompileError(f"entry function @{self.entry_name} has no definition")
        entry_args = list(self.functions[self.entry_name].value.arguments)
        if len(entry_args) != len(self.arguments):
            raise CompileError(f"@{self.entry_name} needs {len(entry_args)} arguments, got {len(self.arguments)}")
        if self.stack_tokens < max(2, len(entry_args)):
            raise CompileError("token stack needs capacity for all entry arguments and at least two tokens")
        for arg, value in zip(entry_args, self.arguments):
            if str(arg.type) == "ptr":
                raise CompileError("host pointer entry arguments are unsupported; use a wrapper with globals/local arrays")
            width = bits(arg.type)
            if width == 1 and value not in (0, 1):
                raise CompileError("i1 entry arguments must be 0 or 1")
            if width == 32 and not -(1 << 31) <= value < 1 << 32:
                raise CompileError("i32 entry argument does not fit 32 bits")
        self.stride = 1 << (max(self.storage_alignment_cells, max(c.count for c in self.functions.values())) - 1).bit_length()
        if self.stride > 1024:
            raise CompileError("activation frame exceeds 1024 words; call-relative offsets would not fit")
        self.stack_base = self.memory_bytes - 16
        stack_low = self.stack_base - self.stack_tokens * 16
        if (self.frame_base + self.frames * self.stride) * 8 > stack_low:
            raise CompileError("bounded frame arena overlaps reserved token-stack region")
        for frame in range(self.frames):
            for slot in range(self.stride):
                self.initial[self.frame_base + frame * self.stride + slot] = slot_word(0)

    def static_pointer(self, value):
        address = self.context.pointers.get(value) if self.context else None
        address = address or self.globals.get(value)
        if address and address.startswith("abs:") and int(address.split(":")[1]) >= 2048:
            return None
        return address

    def pointer_address(self, value, dest):
        # Globals in the array arena cannot use the 11-bit absolute slot field.
        if value in self.globals:
            return self.constant(int(self.globals[value].split(":")[1]) * 4, dest)
        return self.read(value, dest)

    def memory_access(self, pointer, dest, value=None, byte_offset=0):
        """Rebase FP for one access, then return through a saved packed tag.

        Mailboxes 3..6 are safe because the ABI has one serial source control
        token and helpers do not call each other. No source pointer aliases them.
        """
        self.memory_helpers = True
        self.initial.update({3: slot_word(0), 4: slot_word(0), 5: slot_word(0)})
        helper = self.unique("memory")
        return_send = self.emit("send", port="R", addr="abs:3", match="Read", dest="halt.L")
        if value is None:
            self.program.add(helper, "dyadic", op="Dupr", addr="frame:0", match="Read", dest=return_send)
        else:
            store = self.store_slot("frame:0", return_send)
            self.program.add(helper, "dyadic", op="Dupr", addr="abs:4", match="Read", dest=store)
        send = self.emit("send", addr="abs:4", match="Read", dest="halt.L")
        combine = self.emit("dyadic", op="Or", addr="abs:5", match="Read", dest=send)
        shift = self.emit("monadic", op="Shl", imm=27, dest=combine)
        # Check alignment, null, and physical bounds before changing FP. LLVM
        # still requires the pointer to designate a live object of adequate size.
        limit = self.memory_bytes // 8 * 4
        # secondary() overwrites the accumulator; reload the pointer for each test.
        reload_shift = self.read_slot("abs:6", shift)
        valid_high = self.branch(reload_shift, "memory_error.L")
        if limit < 1 << 31:
            compare_high = self.secondary("Sub", valid_high, imm=limit)
        else:
            constant = self.unique("memory_limit")
            self.program.add(constant, ".slot", value=limit)
            compare_high = self.secondary("Sub", valid_high, addr=f"code:{constant}")
        high = self.read_slot("abs:6", compare_high)
        valid_low = self.branch("memory_error.L", high)
        low = self.read_slot("abs:6", self.secondary("Sub", valid_low, imm=64))
        aligned = self.emit("monadic", op="And", imm=3, dest=self.branch("memory_error.L", low))
        setup = self.store_slot("abs:6", aligned)
        self.initial[6] = slot_word(0)
        if byte_offset:
            setup = self.emit("monadic", op="Add", imm=byte_offset, dest=setup)
        setup = self.pointer_address(pointer, setup)
        # Extract preserves caller FP; mask it away to get the helper IP alone.
        store_ip = self.store_slot("abs:5", setup)
        mask = self.emit("monadic", op="And", imm=(1 << 29) - 1, dest=store_ip)
        setup = self.emit("extract", target=f"{helper}.L", addr="frame:0", match="Read", dest=mask)
        if value is not None:
            store_value = self.store_slot("abs:4", setup)
            if isinstance(value, int):
                setup = self.constant(value, store_value)
            elif isinstance(value, str):
                setup = self.read_slot(value, store_value)
            else:
                setup = self.read(value, store_value)
        ret_store = self.store_slot("abs:3", setup)
        return self.emit("extract", target=dest, addr="frame:0", match="Read", dest=ret_store)

    def gep(self, inst, dest):
        text = re.sub(r"^.*?getelementptr\s+(?:(?:inbounds|nuw|nusw)\s+)*", "", str(inst))
        try:
            ty, _ = parse_type(text)
        except ValueError as exc:
            raise CompileError(str(exc)) from exc
        base, *indices = list(inst.operands)
        steps = []
        for index, operand in enumerate(indices):
            if str(operand.type) != "i32":
                raise CompileError("GEP indices must be i32 for the Squall 32-bit pointer ABI")
            if index:
                if ty.element is None:
                    raise CompileError("GEP cannot index into a scalar cell")
                ty = ty.element
            steps.append((operand, ty.size))
        # Accumulate the logical byte offset in a dedicated activation slot.
        slot = f"frame:{self.context.gep_scratch}"
        end = self.emit("dyadic", op="Add", addr=slot, match="Read", dest=dest)
        chain = self.pointer_address(base, end)
        for operand, scale in reversed(steps):
            store = self.store_slot(slot, chain)
            add = self.emit("dyadic", op="Add", addr=slot, match="Read", dest=store)
            mul = self.emit("monadic", op="Mul", imm=scale, dest=add)
            chain = self.read(operand, mul)
        return self.constant(0, self.store_slot(slot, chain))

    def edge(self, before, after):
        context = self.context
        # Branch operands and block iterators wrap the same LLVM pointer, but
        # llvmlite only exposes .instructions on the iterator's block view.
        after = next(block for block in context.value.blocks if block == after)
        phis = [i for i in after.instructions if i.opcode == "phi"]
        dest = f"{context.blocks[after]}.L"
        # Commit all phi destinations after saving every old source first.
        for phi, scratch in reversed(list(zip(phis, context.phi_scratch))):
            store = self.store_slot(f"frame:{context.slots[phi]}", dest)
            dest = self.read_slot(f"frame:{scratch}", store)
        for phi, scratch in reversed(list(zip(phis, context.phi_scratch))):
            sources = list(phi.operands)
            predecessors = list(phi.incoming_blocks)
            value = sources[predecessors.index(before)]
            dest = self.read(value, self.store_slot(f"frame:{scratch}", dest))
        return dest

    def intrinsic(self, inst, dest):
        operands = list(inst.operands)
        name = operands[-1].name
        if name.startswith(("llvm.lifetime.start.", "llvm.lifetime.end.")):
            return dest
        if not name.startswith(("llvm.memcpy.", "llvm.memset.")):
            raise CompileError(f"unsupported intrinsic: {name}")
        if len(operands) != 5 or operands[2].value_kind.name != "constant_int" or operands[3].value_kind.name != "constant_int" or operands[3].get_constant_value():
            raise CompileError("memory intrinsics require constant length and nonvolatile access")
        length = operands[2].get_constant_value()
        if length % 4 or length > 1024:
            raise CompileError("memory intrinsic length must be a multiple of 4, at most 1024 bytes")
        alignments = re.findall(r"\bptr\s+align\s+(\d+)", str(inst))
        required = 2 if name.startswith("llvm.memcpy.") else 1
        if len(alignments) < required or any(int(a) < 4 for a in alignments[:required]):
            raise CompileError("memory intrinsics require explicit alignment of at least 4")
        target, source = operands[:2]
        if name.startswith("llvm.memset."):
            if source.value_kind.name != "constant_int":
                raise CompileError("memset requires a constant fill byte")
            value = canonical((source.get_constant_value() & 255) * 0x01010101, 32)
        else:
            value = f"frame:{self.context.gep_scratch}"
        for offset in reversed(range(0, length, 4)):
            dest = self.memory_access(target, dest, value, byte_offset=offset)
            if name.startswith("llvm.memcpy."):
                dest = self.memory_access(source, self.store_slot(value, dest), byte_offset=offset)
        return dest

    def call(self, inst, dest):
        if list(inst.operands)[-1].name.startswith("llvm."):
            return self.intrinsic(inst, dest)
        if re.search(r"\bmusttail\b", str(inst)):
            raise CompileError("musttail calls require frame reuse and are unsupported")
        operands = list(inst.operands)
        callee = operands[-1]
        if callee.name not in self.functions:
            raise CompileError(f"unsupported external/indirect call: {inst}")
        target = self.functions[callee.name]
        args = list(target.value.arguments)
        if len(args) != len(operands) - 1:
            raise CompileError(f"call argument count mismatch: {inst}")
        if any(str(a.type) != str(b.type) for a, b in zip(args, operands[:-1])):
            raise CompileError(f"call argument types do not match: {inst}")
        continuation = dest
        # A Send uses the packed target on the left and frame[0] as an ignored
        # control value on the right. Its local copy goes to the halt sink.
        send = self.emit("send", addr="frame:0", match="Read", dest="halt.L")
        add = self.literal_slot(self.stride << 29, op="Add", dest=send)
        jump = self.emit("extract", target=f"{target.entry}.L", addr="frame:0", match="Read", dest=add)
        ret_store = self.store_slot(f"frame:{self.stride}", jump)
        setup = self.emit("extract", target=continuation, addr="frame:0", match="Read", dest=ret_store)
        for arg, value in reversed(list(zip(args, operands[:-1]))):
            store = self.store_slot(f"frame:{self.stride + target.slots[arg]}", setup)
            setup = self.read(value, store)
        # Unsigned subtraction's second result is current_fp < last_frame.
        guard = self.branch(setup, "depth_error.L")
        limit = self.frame_base + (self.frames - 1) * self.stride
        subtract = self.secondary("Sub", guard, imm=limit)
        shift = self.emit("monadic", op="Shrl", imm=29, dest=subtract)
        return self.emit("extract", target="halt.L", addr="frame:0", match="Read", dest=shift)

    @staticmethod
    def storage_alignment(text):
        match = re.search(r"\balign\s+(\d+)", text)
        alignment = int(match[1]) if match else 4
        if alignment < 4:
            alignment = 4  # stronger allocation alignment preserves the IR promise
        if alignment > 4096 or alignment & (alignment - 1):
            raise CompileError("storage alignment must be a power of two no larger than 4096 bytes")
        return alignment // 4

    @staticmethod
    def check_alignment(inst):
        align = re.search(r"\balign\s+(\d+)", str(inst))
        if align and int(align[1]) < 4:
            raise CompileError("memory accesses require at least 4-byte alignment")

    def lower(self, inst, block, next_dest):
        opcode = inst.opcode
        operands = list(inst.operands)
        arithmetic = {"add", "sub", "mul", "sdiv", "udiv", "srem", "urem",
                      "and", "or", "xor", "shl", "lshr", "ashr"}
        if opcode in arithmetic:
            return self.binary(inst, self.save(inst, next_dest))
        if opcode == "icmp":
            return self.compare(inst, self.save(inst, next_dest))
        if opcode == "select":
            save = self.save(inst, next_dest)
            true_path, false_path = self.read(operands[1], save), self.read(operands[2], save)
            return self.read(operands[0], self.branch(true_path, false_path))
        if opcode in ("trunc", "zext", "sext"):
            source_width, dest_width = bits(operands[0].type), bits(inst.type)
            save = self.save(inst, next_dest)
            if opcode == "sext" and source_width == 1 and dest_width == 32:
                save = self.emit("monadic", op="Mul", imm=-1, dest=save)
            return self.read(operands[0], save)
        if opcode == "alloca":
            return next_dest
        if opcode == "getelementptr":
            return self.gep(inst, self.save(inst, next_dest))
        if opcode == "load":
            if re.search(r"\b(volatile|atomic)\b", str(inst)):
                raise CompileError("volatile/atomic loads are unsupported")
            if str(inst.type) not in ("i32", "ptr"):
                raise CompileError("memory accesses must be aligned i32 or ptr")
            self.check_alignment(inst)
            target = self.save(inst, next_dest)
            address = self.static_pointer(operands[0])
            return self.read_slot(address, target) if address else self.memory_access(operands[0], target)
        if opcode == "store":
            if re.search(r"\b(volatile|atomic)\b", str(inst)):
                raise CompileError("volatile/atomic stores are unsupported")
            if str(operands[0].type) not in ("i32", "ptr"):
                raise CompileError("memory accesses must be aligned i32 or ptr")
            self.check_alignment(inst)
            address = self.static_pointer(operands[1])
            if address:
                return self.read(operands[0], self.store_slot(address, next_dest))
            return self.memory_access(operands[1], next_dest, operands[0])
        if opcode == "br":
            if len(operands) == 1:
                return self.edge(block, operands[0])
            # LLVM's C API exposes conditional branch operands as cond,false,true.
            return self.read(operands[0], self.branch(self.edge(block, operands[2]), self.edge(block, operands[1])))
        if opcode == "ret":
            send = self.emit("send", addr="frame:0", match="Read", dest="halt.L", port="R")
            return self.read(operands[0], send) if operands else self.constant(0, send)
        if opcode == "call":
            continuation = self.save(inst, next_dest) if str(inst.type) != "void" else next_dest
            return self.call(inst, continuation)
        raise CompileError(f"unsupported instruction @{self.context.value.name}: {inst}")

    def compile(self):
        # Host-return instructions are adjacent where the secondary edge requires it.
        # LIFO executes the secondary write before the primary continuation.
        # Publish the result before setting success, so a host cannot observe
        # a successful status paired with the previous result word.
        self.program.add("host_return", "monadic", op="Dupl",
                         dest="success.L", second="result_write.L")
        self.program.add("result_write", "write", addr=1)
        self.program.add("success", "monadic", op="Dupr", imm=1, dest="status_write.L")
        self.program.add("status_write", "write", addr=2)
        self.jump("depth_error", self.constant(2, "status_write.L"))
        self.jump("memory_error", self.constant(3, "status_write.L"))
        for context in self.functions.values():
            self.context = context
            blocks = list(context.value.blocks)
            self.jump(context.entry, f"{context.blocks[blocks[0]]}.L")
            for block in blocks:
                executable = [i for i in block.instructions if i.opcode != "phi"]
                if not executable:
                    raise CompileError("basic block has no executable terminator")
                self.jump(context.blocks[block], f"{context.instructions[executable[0]]}.L")
                for index, inst in enumerate(executable):
                    next_dest = (f"{context.instructions[executable[index + 1]]}.L"
                                 if index + 1 < len(executable) else "halt.L")
                    try:
                        entry = self.lower(inst, block, next_dest)
                    except CompileError as exc:
                        raise CompileError(f"@{context.value.name}, block {block.name or '<unnamed>'}: {exc}") from exc
                    self.jump(context.instructions[inst], entry)
        context = self.functions[self.entry_name]
        args = list(context.value.arguments)
        joins = []
        for index in range(max(0, len(args) - 1)):
            slot = self.global_next
            self.global_next += 1
            self.initial[slot] = 0  # Once join, NOT a Constant slot
            joins.append((self.unique("bootstrap_join"), slot))
        if self.global_next > 256:
            raise CompileError("globals/bootstrap joins overlap the code region")
        for index, (label, slot) in enumerate(joins):
            dest = f"{joins[index+1][0]}.L" if index + 1 < len(joins) else f"{context.entry}.L"
            self.program.add(label, "dyadic", op="Nop", addr=f"abs:{slot}", match="Once", dest=dest)
        tokens = []
        for index, (arg, value) in enumerate(zip(args, self.arguments)):
            if not joins:
                dest = f"{context.entry}.L"
            elif index < 2:
                dest = f"{joins[0][0]}.{'L' if index == 0 else 'R'}"
            else:
                dest = f"{joins[index-1][0]}.R"
            label = self.unique("argument")
            self.program.add(label, "dyadic", op="Dupr", addr=f"frame:{context.slots[arg]}", match="Const", dest=dest)
            tokens.append((label, "R", canonical(value, bits(arg.type))))
        if not tokens:
            tokens = [(context.entry, "L", 0)]
        code, labels = assemble(self.program)
        if max(code, default=256) >= (self.data_base if self.data_next > self.data_base else self.frame_base):
            raise CompileError("instruction image overlaps the frame arena")
        words = dict(self.initial)
        self._merge(words, code)
        words[self.frame_base] = slot_word(tag_word(labels["host_return"], 0))
        if len(tokens) > self.stack_tokens:
            raise CompileError("initial tokens exceed reserved stack capacity")
        for index, (label, port, acc) in enumerate(tokens):
            at = (self.stack_base - 16 * index) // 8
            low, high = token_words(labels[label], self.frame_base, port, acc)
            self._merge(words, {at: low, at + 1: high})
        if any(not 0 <= word < self.memory_bytes // 8 for word in words):
            raise CompileError("image exceeds physical memory capacity")
        manifest = {
            "format": "squall-v2-image-1", "abi": "squall-cell-memory-2",
            "source_sha256": hashlib.sha256(self.ir.encode()).hexdigest(),
            "llvm_parser_version": list(llvm.llvm_version_info),
            "entry": self.entry_name, "arguments": self.arguments,
            "return_type": str(context.value.global_value_type.get_function_return()),
            "memory_bytes": self.memory_bytes, "word_bytes": 8,
            "stack_base": self.stack_base, "stack_top": self.stack_base - 16 * len(tokens),
            "root_frame_word": self.frame_base, "frame_stride_words": self.stride,
            "max_frames": self.frames, "stack_token_capacity": self.stack_tokens,
            "result_word": 1, "status_word": 2, "status_codes": {"success": 1, "call_depth_exceeded": 2, "invalid_memory_address": 3},
            "max_cycles": 1000000, "initial_token_count": len(tokens),
            "memory_model": {"pointer_bits": 32, "logical_cell_bytes": 4, "physical_cell_bytes": 8,
                             "pointer_to_word_shift": 2, "array_base_word": self.data_base,
                             "array_end_word": self.data_next, "runtime_mailbox_words": list(range(3, 7)) if self.memory_helpers else []},
            "bootstrap_slots": [slot for _, slot in joins],
            "globals": {g.name: int(addr.split(':')[1]) for g, addr in self.globals.items()},
            "functions": {name: {"entry_word": labels[c.entry], "frame_words": c.count,
                                  "argument_slots": [c.slots[a] for a in c.value.arguments],
                                  "alloca_slots": {v.name: int(addr.split(":")[1]) for v, addr in c.pointers.items()}}
                          for name, c in self.functions.items()},
        }
        return Image(self.program, words, labels, manifest)

    @staticmethod
    def _merge(image, additions):
        for address, value in additions.items():
            if address in image:
                raise CompileError(f"image allocation overlap at word {address}")
            image[address] = value


@dataclass
class Image:
    assembly: Program
    words: dict
    symbols: dict
    manifest: dict

    def write(self, directory):
        directory = Path(directory)
        directory.mkdir(parents=True, exist_ok=True)
        (directory / "program.sqa").write_text(self.assembly.text())
        (directory / "memory.hex").write_text("".join(
            f"@{at:x} {value:016x}\n" for at, value in sorted(self.words.items())))
        (directory / "manifest.json").write_text(json.dumps(self.manifest, indent=2, sort_keys=True) + "\n")
        (directory / "symbols.json").write_text(json.dumps(self.symbols, indent=2, sort_keys=True) + "\n")
        # A dense binary contains explicit zero initialization for otherwise
        # unspecified SRAM locations. Do not depend on power-on contents.
        binary = bytearray(self.manifest["memory_bytes"])
        for at, value in self.words.items():
            binary[at * 8:at * 8 + 8] = value.to_bytes(8, "little")
        (directory / "memory.bin").write_bytes(binary)
        params = {"stack_base": self.manifest["stack_base"], "stack_top": self.manifest["stack_top"],
                  "result_addr": self.manifest["result_word"], "max_cycles": self.manifest["max_cycles"]}
        (directory / "params.txt").write_text("".join(f"{key}={value}\n" for key, value in params.items()))


def compile_ir(ir, entry="main", arguments=(), **options):
    return Compiler(ir, entry, arguments, **options).compile()
