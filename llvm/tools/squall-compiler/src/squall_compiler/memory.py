"""The restricted LLVM memory ABI: one aligned i32/ptr cell per Squall slot."""
from __future__ import annotations
from dataclasses import dataclass
import re


@dataclass(frozen=True)
class CellType:
    count: int = 1
    element: CellType | None = None
    pointer: bool = False

    @property
    def cells(self):
        return self.count * (self.element.cells if self.element else 1)

    @property
    def size(self):
        return 4 * self.cells


def parse_type(text):
    text = text.lstrip()
    scalar = re.match(r'(i32|ptr)\b', text)
    if scalar:
        return CellType(pointer=scalar[1] == 'ptr'), text[scalar.end():].lstrip()
    array = re.match(r'\[\s*(\d+)\s+x\s+', text)
    if array:
        element, rest = parse_type(text[array.end():])
        if not rest.startswith(']') or int(array[1]) < 1:
            raise ValueError('memory arrays need a positive size and a closing bracket')
        return CellType(int(array[1]), element), rest[1:].lstrip()
    raise ValueError('memory supports aligned i32/ptr cells and fixed arrays of those cells')


def initializer(ty, text):
    text = text.lstrip()
    if text.startswith('zeroinitializer'):
        return [0] * ty.cells, text[len('zeroinitializer'):]
    if ty.element:
        if not text.startswith('['):
            raise ValueError('array storage requires a constant initializer')
        text, values = text[1:].lstrip(), []
        for index in range(ty.count):
            element, text = parse_type(text)
            if element != ty.element:
                raise ValueError('initializer element type differs from array type')
            chunk, text = initializer(element, text)
            values.extend(chunk)
            text = text.lstrip()
            delimiter = ',' if index + 1 < ty.count else ']'
            if not text.startswith(delimiter):
                raise ValueError('incorrect number of array initializer elements')
            text = text[1:].lstrip()
        return values, text
    if ty.pointer:
        if not text.startswith('null'):
            raise ValueError('pointer globals currently require null initialization')
        return [0], text[4:]
    number = re.match(r'-?\d+\b', text)
    if not number:
        raise ValueError('i32 storage requires an integer initializer')
    return [int(number[0])], text[number.end():]
