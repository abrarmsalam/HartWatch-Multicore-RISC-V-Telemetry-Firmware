"""Symbolization tests against the real firmware ELF, cross-checked with binutils' addr2line."""
from __future__ import annotations

import os
import shutil
import subprocess
from pathlib import Path

import pytest

import symbols as sym

ELF = Path(os.environ.get("HW_BUILD", "build")) / "hartwatch.elf"


@pytest.fixture(scope="module")
def symbolizer() -> sym.Symbolizer:
    return sym.Symbolizer.from_path(ELF)


def start_of(s: sym.Symbolizer, name: str) -> sym.Function:
    return next(f for f in s.functions if f.name == name)


def test_table_contains_c_and_assembly_functions(symbolizer: sym.Symbolizer) -> None:
    names = {f.name for f in symbolizer.functions}
    assert {"hart_main", "hart0_main", "timer_irq", "sampler_tick", "wl_alu", "wl_store", "trap_entry"} <= names


def test_table_is_sorted(symbolizer: sym.Symbolizer) -> None:
    assert symbolizer.starts == sorted(symbolizer.starts)


@pytest.mark.parametrize("name", ["wl_alu", "wl_branch", "wl_stride", "wl_store", "timer_irq", "hart_main"])
def test_every_byte_of_a_function_resolves_to_it(symbolizer: sym.Symbolizer, name: str) -> None:
    f = start_of(symbolizer, name)
    assert symbolizer.lookup(f.start).function == name
    assert symbolizer.lookup(f.start + f.size - 1).function == name


def test_address_outside_every_function_is_unknown(symbolizer: sym.Symbolizer) -> None:
    assert symbolizer.lookup(0).function == sym.UNKNOWN
    assert symbolizer.lookup(0xFFFFFFFF).function == sym.UNKNOWN
    last = symbolizer.functions[-1]
    assert symbolizer.lookup(last.start + last.size + 4096).function == sym.UNKNOWN


def test_dwarf_line_for_c_code(symbolizer: sym.Symbolizer) -> None:
    where = symbolizer.lookup(start_of(symbolizer, "timer_irq").start)
    assert where.file == "timer.c"
    source = Path("fw/src/timer.c").read_text().splitlines()
    assert source[where.line - 1] == "{" and "timer_irq" in source[where.line - 2]


def test_dwarf_line_for_assembly(symbolizer: sym.Symbolizer) -> None:
    where = symbolizer.lookup(start_of(symbolizer, "wl_alu").start)
    assert where.file == "workloads.S"
    assert "li" in Path("fw/src/workloads.S").read_text().splitlines()[where.line - 1]


ADDR2LINE = shutil.which("riscv64-unknown-elf-addr2line")


@pytest.mark.skipif(ADDR2LINE is None, reason="RISC-V addr2line not installed")
def test_lines_agree_with_addr2line(symbolizer: sym.Symbolizer) -> None:
    """An independent implementation: binutils, not pyelftools."""
    checked = 0
    for f in symbolizer.functions:
        for pc in range(f.start, f.start + f.size, 2):
            if checked >= 2000:
                break
            out = subprocess.run([ADDR2LINE, "-e", str(ELF), hex(pc)], capture_output=True, text=True).stdout.strip()
            path, _, line = out.partition(":")
            if line.split(" ")[0] in ("?", "0", ""):
                continue
            where = symbolizer.lookup(pc)
            assert Path(path).name == where.file, hex(pc)
            assert int(line.split(" ")[0]) == where.line, hex(pc)
            checked += 1
    assert checked > 500
