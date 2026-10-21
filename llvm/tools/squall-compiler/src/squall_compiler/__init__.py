"""Squall v2 compiler tools. See README.md for the deliberately bounded ABI."""

from .assembler import AssemblyError, Program, assemble

__all__ = ["AssemblyError", "Program", "assemble"]
