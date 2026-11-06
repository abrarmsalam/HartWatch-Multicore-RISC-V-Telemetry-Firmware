"""Map a program counter to a function, file and line, using the firmware ELF."""
from __future__ import annotations

import bisect
from dataclasses import dataclass
from pathlib import Path
from typing import BinaryIO

from elftools.elf.elffile import ELFFile

UNKNOWN = "(unknown)"


@dataclass(frozen=True)
class Function:
    start: int
    size: int
    name: str


@dataclass(frozen=True)
class Location:
    function: str
    file: str | None
    line: int | None


def read_functions(elf: ELFFile) -> list[Function]:
    """Every sized function in the symbol table, sorted by address."""
    table = elf.get_section_by_name(".symtab")
    if table is None:
        return []
    found = {
        (s["st_value"], s["st_size"], s.name)
        for s in table.iter_symbols()
        if s["st_info"]["type"] == "STT_FUNC" and s["st_size"] > 0
    }
    return [Function(*row) for row in sorted(found)]


def find_function(functions: list[Function], starts: list[int], pc: int) -> str:
    index = bisect.bisect_right(starts, pc) - 1
    if index >= 0 and pc < functions[index].start + functions[index].size:
        return functions[index].name
    return UNKNOWN


def _file_name(program, index: int) -> str | None:
    """DWARF 5 numbers files from 0, earlier versions from 1."""
    entries = program["file_entry"]
    position = index if program.header["version"] >= 5 else index - 1
    if not 0 <= position < len(entries):
        return None
    return entries[position].name.decode()


def read_lines(elf: ELFFile) -> list[tuple[int, str | None, int | None]]:
    """(address, file, line) rows sorted by address. A None file marks the end of a range."""
    if not elf.has_dwarf_info():
        return []
    dwarf = elf.get_dwarf_info()
    rows: list[tuple[int, str | None, int | None]] = []
    for unit in dwarf.iter_CUs():
        program = dwarf.line_program_for_CU(unit)
        if program is None:
            continue
        for entry in program.get_entries():
            state = entry.state
            if state is None:
                continue
            if state.end_sequence:
                rows.append((state.address, None, None))
            else:
                rows.append((state.address, _file_name(program, state.file), state.line))
    rows.sort(key=lambda row: (row[0], row[1] is not None))
    return rows


def find_line(rows: list[tuple[int, str | None, int | None]], addresses: list[int],
              pc: int) -> tuple[str | None, int | None]:
    index = bisect.bisect_right(addresses, pc) - 1
    if index < 0:
        return None, None
    return rows[index][1], rows[index][2]


class Symbolizer:
    """Answers pc lookups for one ELF. Repeated addresses are cached."""

    def __init__(self, stream: BinaryIO) -> None:
        elf = ELFFile(stream)
        self.functions = read_functions(elf)
        self.starts = [f.start for f in self.functions]
        self.lines = read_lines(elf)
        self.addresses = [row[0] for row in self.lines]
        self._cache: dict[int, Location] = {}

    @classmethod
    def from_path(cls, path: Path) -> "Symbolizer":
        with path.open("rb") as stream:
            return cls(stream)

    def lookup(self, pc: int) -> Location:
        if pc not in self._cache:
            file, line = find_line(self.lines, self.addresses, pc)
            self._cache[pc] = Location(find_function(self.functions, self.starts, pc), file, line)
        return self._cache[pc]
