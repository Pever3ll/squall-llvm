"""Labelled, one-word-per-record Squall assembly with checked word addressing."""
from __future__ import annotations

import re
import shlex
from dataclasses import dataclass, field

MASK64 = (1 << 64) - 1
OPS = dict(Nop=0, Add=4, Sub=5, Mul=6, Div=7, Divu=8, Rem=9, Mod=10,
           And=16, Or=17, Xor=18, Shl=19, Shrl=20, Shra=21, Not=24,
           Rev=25, Dupl=26, Dupr=27, Beqz=32, Beql=33, Beqr=34)
PORTS = {"L": 0, "R": 1}
MATCH = {"Once": 0, "Const": 1, "Read": 2}


class AssemblyError(ValueError):
    pass


def integer(value, context="integer"):
    try:
        return int(str(value), 0)
    except ValueError as exc:
        raise AssemblyError(f"invalid {context}: {value!r}") from exc


def unsigned(value, bits, context):
    value = integer(value, context)
    if not 0 <= value < 1 << bits:
        raise AssemblyError(f"{context}: {value} does not fit unsigned {bits} bits")
    return value


def signed(value, bits, context):
    value = integer(value, context)
    if not -(1 << (bits - 1)) <= value < 1 << (bits - 1):
        raise AssemblyError(f"{context}: {value} does not fit signed {bits} bits")
    return value & ((1 << bits) - 1)


def slot_word(value):
    return (2 << 62) | signed(value, 62, "constant slot payload")


def tag_word(ip, fp, port="L"):
    if port not in PORTS:
        raise AssemblyError(f"invalid operand port {port}")
    return unsigned(ip, 29, "tag IP") | unsigned(fp, 29, "tag FP") << 29 | PORTS[port] << 58


def token_words(ip, fp, port="L", acc=0):
    tag = tag_word(ip, fp, port)
    value = integer(acc)
    if not -(1 << 63) <= value <= MASK64:
        raise AssemblyError("token accumulator does not fit 64 bits")
    return tag << 2, value & MASK64


@dataclass
class Record:
    label: str
    kind: str
    fields: dict[str, str] = field(default_factory=dict)
    line: int = 0


@dataclass
class Program:
    records: list[Record] = field(default_factory=list)

    def add(self, label, kind, **fields):
        self.records.append(Record(label, kind, {k: str(v) for k, v in fields.items()}))

    def text(self):
        """Print the team syntax; legacy key=value input remains accepted."""
        lines = []
        for index, r in enumerate(self.records):
            f = r.fields
            if r.kind in (".slot", ".word"):
                body = f"{r.kind} {f['value']}"
            elif r.kind == "write":
                body = f"write {f['addr']}"
            else:
                op = f.get("op", "Nop").lower() if r.kind in ("monadic", "dyadic") else r.kind
                if r.kind != "monadic":
                    op += {"Once": "", "Read": ".r", "Const": ".c"}[f.get("match", "Read")]
                destination = f.get("dest", "halt.L")
                destination = destination[:-1] + destination[-1].lower()
                if "second" in f:
                    name, _, port = f["second"].rpartition(".")
                    following = self.records[index + 1].label if index + 1 < len(self.records) else None
                    if name != following and name != "(1)":
                        raise AssemblyError("secondary destination must be the next word")
                    destination += "." + port.lower()
                args = [destination]
                if r.kind == "extract":
                    args.append(f["target"][:-1] + f["target"][-1].lower())
                if r.kind == "monadic":
                    args.append("#" + f.get("imm", "0"))
                else:
                    mode, _, value = f["addr"].partition(":")
                    if mode == "code" and value.startswith("("):
                        value = value[1:-1]
                    args.append({"frame": f"[{value}]", "code": f"({value})", "abs": value}[mode])
                body = op + " " + " ".join(args)
            lines.append(f"{r.label}: {body}")
        return "\n".join(lines) + "\n"

    @classmethod
    def parse(cls, text):
        program = cls()
        pending = None
        explicit = set(re.findall(r"^\s*([A-Za-z_][\w.$-]*):", text, re.M))
        for line, raw in enumerate(text.splitlines(), 1):
            # '#' introduces an immediate in the team syntax. Older key=value
            # assembly used it for comments; retain that input convention.
            raw = raw.split(";", 1)[0].strip()
            if not raw or raw.startswith("#"):
                continue
            if re.search(r"(?:^|\s)(?:monadic|dyadic|send|extract|write|\.slot|\.word)\s+\w+=", raw):
                raw = raw.split("#", 1)[0].strip()
            found = re.match(r"^([A-Za-z_][\w.$-]*):(?:\s|$)", raw)
            if found:
                if pending:
                    raise AssemblyError(f"line {line}: label {pending} has no record")
                pending = found[1]
                raw = raw[found.end():].strip()
            if not raw:
                continue
            if pending is None:
                if re.match(r"(?:monadic|dyadic)\b", raw):
                    raise AssemblyError(f"line {line}: legacy record needs a label")
                pending = f"_line{line}"
                while pending in explicit:
                    pending += "_"
                explicit.add(pending)
            try:
                parts = shlex.split(raw)
                if parts[0] in ("monadic", "dyadic") or any("=" in item for item in parts[1:]):
                    fields = {}
                    for item in parts[1:]:
                        if "=" not in item:
                            raise AssemblyError(f"expected key=value, got {item}")
                        key, value = item.split("=", 1)
                        if key in fields:
                            raise AssemblyError(f"duplicate operand {key}")
                        fields[key] = value
                    kind = parts[0]
                else:
                    kind, fields = parse_team(parts)
                program.records.append(Record(pending, kind, fields, line))
            except (ValueError, KeyError) as exc:
                raise AssemblyError(f"line {line}: {exc}") from exc
            pending = None
        if pending:
            raise AssemblyError(f"label {pending} has no record")
        return program


def parse_team(parts):
    mnemonic, *args = parts
    if mnemonic in (".slot", ".word", "write"):
        if len(args) != 1:
            raise AssemblyError(f"{mnemonic} needs one operand")
        return mnemonic, {"addr" if mnemonic == "write" else "value": args[0]}
    op, dot, rule = mnemonic.partition(".")
    opcodes = {name.lower(): name for name in OPS}
    if op not in opcodes and op not in ("send", "extract"):
        raise AssemblyError(f"unknown opcode {op}")
    if dot and rule not in ("r", "c"):
        raise AssemblyError(f"invalid matching suffix .{rule}")
    if len(args) not in (2, 3, 4):
        raise AssemblyError(f"{mnemonic} needs a destination and an operand")
    match = re.fullmatch(r"(.+?)\.([lLrR])(?:\.([lLrR]))?", args.pop(0))
    if not match:
        raise AssemblyError("destination must end in .l or .r, optionally followed by a secondary port")
    fields = {"dest": match[1] + "." + match[2].upper()}
    second = match[3]
    # Accept Lyssia's separated-port spelling as an input alias.
    if args and args[0] in ("l", "r", ".l", ".r"):
        if second:
            raise AssemblyError("duplicate secondary port")
        second = args.pop(0)[-1]
    if second:
        fields["second"] = "(1)." + second.upper()
    if op == "extract":
        if not args or not re.fullmatch(r".+\.[lLrR]", args[0]):
            raise AssemblyError("extract needs a target tag destination")
        target = args.pop(0)
        fields["target"] = target[:-1] + target[-1].upper()
    if len(args) != 1:
        raise AssemblyError(f"unexpected operands for {mnemonic}")
    operand = args[0]
    if op in ("send", "extract") and second:
        raise AssemblyError(f"{op} cannot emit a secondary ALU token")
    if operand.startswith("#"):
        if dot or op not in opcodes:
            raise AssemblyError("immediates are only valid on ALU operations without a matching suffix")
        fields.update(op=opcodes[op], imm=operand[1:])
        return "monadic", fields
    if operand.startswith("[") and operand.endswith("]"):
        addr = "frame:" + operand[1:-1]
    elif operand.startswith("(") and operand.endswith(")"):
        value = operand[1:-1]
        # Parenthesized numeric operands are literal relative word offsets.
        addr = "code:" + ("(" + value + ")" if re.fullmatch(r"[+-]?(?:0[xX][0-9a-fA-F]+|[0-9]+)", value) else value)
    else:
        addr = "abs:" + operand
    fields.update(addr=addr, match={"": "Once", "r": "Read", "c": "Const"}[rule])
    if op in opcodes:
        fields["op"] = opcodes[op]
        return "dyadic", fields
    return op, fields


def assemble(program: str | Program, origin=256):
    program = Program.parse(program) if isinstance(program, str) else program
    origin = unsigned(origin, 29, "origin (word address)")
    if origin == 0:
        raise AssemblyError("instruction word zero is the halt sink")
    labels = {"halt": 0}
    for index, record in enumerate(program.records):
        if not re.fullmatch(r"[A-Za-z_][\w.$-]*", record.label) or re.search(r"\.[lLrR]$", record.label):
            raise AssemblyError(f"invalid label {record.label!r}; labels cannot end in a port suffix .l/.r")
        if record.label in labels:
            raise AssemblyError(f"duplicate/reserved label {record.label}")
        labels[record.label] = unsigned(origin + index, 29, "record address")

    def lookup(name):
        if name not in labels:
            raise AssemblyError(f"undefined label {name}")
        return labels[name]

    def destination(ref, here):
        name, sep, port = ref.rpartition(".")
        if not sep or port not in PORTS:
            raise AssemblyError(f"destination {ref!r} must end in .L or .R")
        delta = integer(name[1:-1]) if name.startswith("(") and name.endswith(")") else lookup(name) - here
        offset = signed(delta, 20, "destination offset in words")
        return offset, PORTS[port]

    words = {}
    for record in program.records:
        here = labels[record.label]
        f = record.fields
        try:
            allowed = {
                "monadic": {"op", "imm", "dest", "second"},
                "dyadic": {"op", "addr", "match", "dest", "second"},
                "send": {"addr", "match", "dest"},
                "extract": {"addr", "match", "dest", "target"},
                "write": {"addr"}, ".slot": {"value"}, ".word": {"value"},
            }
            if record.kind not in allowed:
                raise AssemblyError(f"unknown record kind {record.kind}")
            extra = f.keys() - allowed[record.kind]
            if extra:
                raise AssemblyError(f"unknown operands: {', '.join(sorted(extra))}")
            if record.kind == ".slot":
                value = slot_word(integer(f["value"]))
            elif record.kind == ".word":
                value = integer(f["value"])
                if not -(1 << 63) <= value <= MASK64:
                    raise AssemblyError("data word does not fit 64 bits")
                value &= MASK64
            elif record.kind == "write":
                value = 2 | unsigned(f["addr"], 29, "Write word address") << 2
            else:
                d, p = destination(f.get("dest", "halt.L"), here)
                value = d << 43 | p << 63
                if record.kind in ("monadic", "dyadic"):
                    op = f.get("op", "Nop")
                    if op not in OPS:
                        raise AssemblyError(f"unknown opcode {op}")
                    value |= OPS[op] << 2
                    if "second" in f:
                        second, second_port = destination(f["second"], here)
                        if second != 1 or here + 1 not in labels.values():
                            raise AssemblyError("secondary destination must be the next word")
                        value |= 1 << 8 | second_port << 9
                if record.kind == "monadic":
                    value |= signed(f.get("imm", "0"), 32, "immediate") << 10
                else:
                    mode, colon, addr = f["addr"].partition(":")
                    if not colon or mode not in ("frame", "code", "abs"):
                        raise AssemblyError("slot address must be frame:, code:, or abs:")
                    if mode == "code":
                        delta = integer(addr[1:-1]) if addr.startswith("(") and addr.endswith(")") else lookup(addr) - here
                        addr = signed(delta, 11, "code-relative slot offset")
                    else:
                        addr = unsigned(addr, 11, f"{mode} slot offset")
                    matching = f.get("match", "Read")
                    if matching not in MATCH:
                        raise AssemblyError(f"invalid matching rule {matching}")
                    value |= 1 | addr << 28 | {"frame": 0, "code": 1, "abs": 2}[mode] << 39
                    value |= MATCH[matching] << 41
                    if record.kind == "send":
                        value |= 3 << 26
                    elif record.kind == "extract":
                        offset, port = destination(f["target"], here)
                        value |= 2 << 26 | offset << 2 | port << 22
            words[here] = value
        except (AssemblyError, KeyError) as exc:
            raise AssemblyError(f"line {record.line or '?'} ({record.label}): {exc}") from exc
    return words, labels
