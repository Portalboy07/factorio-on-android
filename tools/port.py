#!/usr/bin/env python3
"""Port Factorio's Linux ARM64 executable into a bionic-loadable shared object.

Android's libc is bionic, while the game is linked against glibc and carries a
statically linked SDL3.  This script performs the ELF-level surgery that the
iPad reference project performs on its Mach-O target:

  * drop PT_INTERP / DF_1_PIE so the image can be dlopen()ed by bionic
  * rewrite DT_NEEDED to bionic library names and to the compat shim
  * remove glibc symbol versioning so imports resolve by name
  * relocate local-exec TLS accesses by a fixed page offset (see PLAN.md §3.2)
  * redirect every public SDL3 function entry to the Android SDL3 build via
    12-byte thunks and a pointer-slot table placed in extra .bss

The structural transformation and verification results are recorded in a
machine-readable report for build diagnostics.

Usage (analysis only, no output written):
    python3 tools/port.py --source /path/to/factorio --analyze

Full transform:
    python3 tools/port.py --source /path/to/factorio
"""
from __future__ import annotations

import argparse
import bisect
import json
import re
import struct
import sys
from pathlib import Path

from elf import (DT_DEBUG, DT_FLAGS_1, DT_NEEDED, DT_NULL, DF_1_PIE, DT_SONAME,
                 DT_VERDEF, DT_VERDEFNUM, DT_VERNEED, DT_VERNEEDNUM, DT_VERSYM,
                 ELF, ELFError, PT_INTERP, PT_LOAD, PT_NULL, PT_TLS, SHT_DYNSYM,
                 SHT_NOBITS)

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUTPUT = ROOT / "build/libfactorio_game.so"
DEFAULT_REPORT = ROOT / "build/port-report.json"

# The shim is also the resolver for SDL slot pointers; libresolv is not present
# on Android, so its NEEDED slot becomes ours.
NEEDED_MAP = {
    "libc.so.6": "libc.so",
    "libm.so.6": "libm.so",
    "libresolv.so.2": "libfshim.so",
}

# Imports whose glibc ABI differs from bionic's.  The game's references are
# renamed in place (same length) to libfshim.so wrappers that translate the
# structures.  Defining the original names in the shim would not work:
# app_process exports ART's sigchain sigaction/signal globally, and the
# global group wins over the game's own DT_NEEDED libraries.
IMPORT_RENAMES = {
    "sigaction": "fs_sigact",   # glibc struct sigaction: 152 bytes, handler first
    "signal": "fs_sig",         # same ABI; wrapped to log fatal-signal crashes
}

EXPECTED_SDL = (3, 4, 14)  # SDL version used by this Android wrapper build
MRS_TPIDR_EL0_MASK = 0xFFFFFFE0
MRS_TPIDR_EL0 = 0xD53BD040
MOV_XD_XZR = 0xAA1F03E0  # orr Xd, xzr, xzr  (kept for future use)

DEFAULT_TLS_BASE = 0x10000  # page-aligned; room for bionic's TCB/TLS area


# --------------------------------------------------------------------- helpers
def align(value: int, alignment: int) -> int:
    return (value + alignment - 1) & ~(alignment - 1)


def cstring(data: bytearray, offset: int) -> str:
    end = data.find(b"\0", offset)
    if end < 0:
        raise ELFError("unterminated dynamic string")
    return bytes(data[offset:end]).decode()


# ------------------------------------------------------------------ ELF stages
def remove_interp(elf: ELF) -> dict:
    removed = []
    for interp in elf.segment(PT_INTERP):
        index = interp["index"]
        old = bytes(elf.data[interp["offset"]:interp["offset"] + interp["filesz"]])
        elf.put_u32(interp["header_offset"], PT_NULL)
        removed.append({"old_interpreter": old.rstrip(b"\0").decode(), "phdr": index})
    if not removed:
        raise ELFError("no PT_INTERP: already transformed?")
    return {"removed": removed}


def rewrite_dynamic(elf: ELF, *, drop_versions: bool = True) -> dict:
    segment = elf.dynamic_segment()
    entries = elf.dynamic()
    kept: list[dict] = []
    dropped = []
    renamed = []
    pie_cleared = False
    for entry in entries:
        tag, value = entry["tag"], entry["value"]
        if tag == DT_NULL:
            continue
        if drop_versions and tag in (DT_VERSYM, DT_VERNEED, DT_VERNEEDNUM,
                                     DT_VERDEF, DT_VERDEFNUM):
            dropped.append(hex(tag & 0xFFFFFFFF))
            continue
        if tag == DT_DEBUG:
            dropped.append("DT_DEBUG")
            continue
        if tag == DT_FLAGS_1 and value & DF_1_PIE:
            value &= ~DF_1_PIE
            pie_cleared = True
        if tag == DT_NEEDED:
            name = elf.dynamic_string(value)
            replacement = NEEDED_MAP.get(name, name)
            if replacement != name:
                encoded = replacement.encode() + b"\0"
                if len(encoded) > len(name) + 1:
                    raise ELFError(f"NEEDED {name} -> {replacement} does not fit in place")
                start = elf.dynamic_strtab_offset() + value
                elf.put_bytes(start, encoded + bytes(len(name) + 1 - len(encoded)))
                renamed.append({"from": name, "to": replacement})
                value = value
        kept.append({"tag": tag, "value": value})

    kept.append({"tag": DT_NULL, "value": 0})
    capacity = segment["filesz"] // 16
    if len(kept) > capacity:
        raise ELFError("dynamic table overflow")
    for i, entry in enumerate(kept):
        struct.pack_into("<qQ", elf.data, segment["offset"] + i * 16,
                         entry["tag"], entry["value"])
    for i in range(len(kept), capacity):
        struct.pack_into("<qQ", elf.data, segment["offset"] + i * 16, DT_NULL, 0)

    # Neutralise .gnu.version as well: every entry becomes VER_NDX_GLOBAL.
    zeroed = 0
    for section in elf.sections(0x6FFFFFFF):  # SHT_GNU_VERSYM
        for i in range(section["size"] // 2):
            struct.pack_into("<H", elf.data, section["offset"] + i * 2, 1)
            zeroed += 1
    return {"dropped_tags": dropped, "needed_renamed": renamed,
            "pie_flag_cleared": pie_cleared, "versym_entries_neutralised": zeroed,
            "dynamic_entries": len(kept) - 1}


def rename_imports(elf: ELF) -> dict:
    """Point selected undefined dynamic symbols at fshim wrappers."""
    tables = elf.sections(SHT_DYNSYM)
    if len(tables) != 1:
        raise ELFError("expected exactly one .dynsym")
    table = tables[0]
    strtab = elf.shdrs[table["link"]]["offset"]
    entries = []
    for i in range(table["size"] // 24):
        name, _info, _other, shndx, _value, _size = struct.unpack_from(
            "<IBBHQQ", elf.data, table["offset"] + i * 24)
        entries.append((name, shndx))
    referenced = {name for name, _ in entries if name}
    referenced |= {e["value"] for e in elf.dynamic() if e["tag"] in (DT_NEEDED, DT_SONAME)}
    renamed = []
    for name_off, shndx in entries:
        if not name_off or shndx != 0:
            continue
        name = cstring(elf.data, strtab + name_off)
        new = IMPORT_RENAMES.get(name)
        if new is None:
            continue
        if len(new) != len(name):
            raise ELFError(f"import rename {name} -> {new} must keep the length")
        # The string must be its own: not the merged tail of a longer name,
        # and with no other reference pointing into it.
        if elf.data[strtab + name_off - 1] != 0 or any(
                name_off < other <= name_off + len(name) for other in referenced):
            raise ELFError(f"import {name} shares .dynstr storage; cannot rename in place")
        elf.put_bytes(strtab + name_off, new.encode())
        renamed.append({"from": name, "to": new})
    done = {r["from"] for r in renamed}
    return {"renamed": renamed, "not_imported": sorted(set(IMPORT_RENAMES) - done)}


def extend_bss(elf: ELF, extra: int) -> int:
    """Grow the last writable PT_LOAD and return the vaddr of the new area."""
    candidates = [p for p in elf.segment(PT_LOAD) if p["flags"] & 2]
    if not candidates:
        raise ELFError("no writable PT_LOAD")
    last = max(candidates, key=lambda p: p["vaddr"] + p["memsz"])
    if any(p is not last and p["vaddr"] >= last["vaddr"] + last["memsz"]
           and p["vaddr"] < last["vaddr"] + last["memsz"] + extra
           for p in elf.segment(PT_LOAD)):
        raise ELFError("extending .bss would overlap another segment")
    base = last["vaddr"] + last["memsz"]
    grown = align(last["memsz"] + extra, 0x1000)
    struct.pack_into("<Q", elf.data, last["header_offset"] + 40, grown)
    return align(base, 8)


# ------------------------------------------------------------------- TLS pass
IMM = r"(0x[0-9a-f]+|\d+)"
ADD_RE = re.compile(rf"^add (x\d+), (x\d+), #{IMM}(, lsl #12)?$")
ADD_REG_RE = re.compile(r"^add (x\d+), (x\d+), (x\d+)$")
MEM_RE = re.compile(rf"^(ldr|str|ldrb|strb|ldrh|strh|ldrsb|ldrsh|ldrsw) "
                    rf"(?:[a-z]\d+), \[(x\d+)(?:, #{IMM})?\]$")
MEM_REG_RE = re.compile(r"^(ldr|str|ldrb|strb|ldrh|strh|ldrsb|ldrsh|ldrsw) "
                        r"(?:[a-z]\d+), \[(x\d+), (x\d+)\]$")
MOVZ_RE = re.compile(rf"^movz (x\d+), #{IMM}, lsl #16$")
MOVK_RE = re.compile(rf"^movk (x\d+), #{IMM}$")


def parse_imm(text: str) -> int:
    return int(text, 16) if text.startswith("0x") else int(text, 10)


def detect_sdl_version(elf: ELF) -> dict:
    """Decode the constant returned by the game's static SDL_GetVersion_REAL.

    The game links SDL3 with its dynapi enabled, so this also tells us which
    SDL3 release the runtime override library has to be built from.
    """
    symbol = next((s for s in elf.symbols()
                   if s["name"] == "SDL_GetVersion_REAL"), None)
    if not symbol:
        return {"detected": False}
    offset = elf.vaddr_to_offset(symbol["value"])
    words = struct.unpack_from("<4I", elf.data, offset)
    value = None
    for i, word in enumerate(words[:3]):
        if (word & 0xFFE0001F) == 0x52800000:  # movz w0, #imm16
            value = (word >> 5) & 0xFFFF
        elif (word & 0xFF80001F) == 0x72800000 and value is not None:
            halfword = (word >> 21) & 3
            value |= (((word >> 5) & 0xFFFF) << (16 * halfword))
    if value is None:
        # Fall back to the canonical SDL_VERSIONNUM encoding.
        return {"detected": False}
    major, minor, patch = value // 1_000_000, (value // 1000) % 1000, value % 1000
    return {"detected": True, "number": value,
            "version": f"{major}.{minor}.{patch}",
            "matches_expected": [major, minor, patch] == list(EXPECTED_SDL)}


class TLSRewriter:
    def __init__(self, elf: ELF, base: int, verbose: bool = False):
        self.elf = elf
        self.base = base
        self.verbose = verbose
        self.text = elf.section(".text")
        self.tls = elf.segment(PT_TLS)
        if len(self.tls) != 1:
            raise ELFError("expected exactly one PT_TLS")
        self.tls_memsz = self.tls[0]["memsz"]
        self.patches: list[dict] = []
        self.unsupported: list[dict] = []
        self.unknown: list[dict] = []
        self.accesses: list[dict] = []
        try:
            import capstone
            self._capstone = capstone
        except ImportError:
            raise ELFError("python 'capstone' is required for TLS analysis "
                           "(python3 -m pip install capstone)")
        self._md = self._capstone.Cs(self._capstone.CS_ARCH_ARM64,
                                     self._capstone.CS_MODE_ARM)
        self._md.detail = True
        # TP often lives in a callee-saved register for the whole function and
        # is used again far away from the `mrs`, so accesses have to be found
        # with function-scoped control-flow analysis.  The image carries a
        # symtab with sizes, which gives us exact function bounds.
        text_start = self.text["addr"]
        text_end = text_start + self.text["size"]
        functions: dict[int, tuple[int, int, str]] = {}
        for symbol in elf.symbols():
            if symbol["type"] != 2 or not symbol["size"]:
                continue
            if symbol["shndx"] != self.text["index"]:
                continue
            start = symbol["value"]
            end = min(start + symbol["size"], text_end)
            if start < text_start or end <= start:
                continue
            known = functions.get(start)
            if known is None or end > known[1]:
                functions[start] = (start, end, symbol["name"])
        self.functions = sorted(functions.values())
        self._function_starts = [function[0] for function in self.functions]
        self._instructions: dict[int, list] = {}

    def _function_for(self, address: int):
        index = bisect.bisect_right(self._function_starts, address) - 1
        if index >= 0:
            function = self.functions[index]
            if function[0] <= address < function[1]:
                return function
        return None

    def _function_instructions(self, function) -> list:
        cached = self._instructions.get(function[0])
        if cached is not None:
            return cached
        start, end, _ = function
        offset = self.elf.vaddr_to_offset(start)
        window = bytes(self.elf.data[offset:offset + (end - start)])
        insns = list(self._md.disasm(window, start))
        self._instructions[function[0]] = insns
        return insns

    def _writes_register(self, insn, register: str) -> bool:
        for operand in insn.operands:
            if operand.type != self._capstone.arm64.ARM64_OP_REG:
                continue
            if not operand.access & self._capstone.CS_AC_WRITE:
                continue
            if insn.reg_name(operand.reg) == register:
                return True
        return False

    def _copy_register(self, insn):
        if insn.mnemonic != "mov" or len(insn.operands) != 2:
            return None
        source, dest = insn.operands
        arm64 = self._capstone.arm64
        if source.type != arm64.ARM64_OP_REG or dest.type != arm64.ARM64_OP_REG:
            return None
        lhs = insn.reg_name(dest.reg)
        rhs = insn.reg_name(source.reg)
        if not lhs.startswith("x") or not rhs.startswith("x"):
            return None
        return rhs, lhs

    def _branch_target(self, insn):
        arm64 = self._capstone.arm64
        targets = [operand.imm for operand in insn.operands
                   if operand.type == arm64.ARM64_OP_IMM]
        return targets[-1] if targets else None

    def _classify_constant(self, insns: list, seed_index: int, dst: str) -> list[dict]:
        """Handle TLS offsets assembled with movz/movk instead of add pairs.

        __cxa_get_globals(), __cxa_get_globals_fast() and __once_proxy() build
        the offset in a scratch register; the base is added to the high half of
        that constant.
        """
        results: list[dict] = []
        const_regs: dict[str, list[int]] = {}
        for insn in insns[seed_index + 1:seed_index + 65]:
            text = f"{insn.mnemonic} {insn.op_str}"
            if insn.mnemonic in ("ret", "br", "brk", "udf"):
                break
            movz = MOVZ_RE.match(text)
            if movz:
                const_regs[movz.group(1)] = [parse_imm(movz.group(2)) << 16,
                                             insn.address]
                continue
            movk = MOVK_RE.match(text)
            if movk and movk.group(1) in const_regs:
                const_regs[movk.group(1)][0] |= parse_imm(movk.group(2))
                continue
            if const_regs:
                used = None
                reg_add = ADD_REG_RE.match(text)
                reg_mem = MEM_REG_RE.match(text)
                if reg_add and reg_add.group(2) == dst \
                        and reg_add.group(3) in const_regs:
                    used = const_regs[reg_add.group(3)]
                elif reg_mem and reg_mem.group(2) == dst \
                        and reg_mem.group(3) in const_regs:
                    used = const_regs[reg_mem.group(3)]
                if used:
                    results.append({"kind": "const", "tls_offset": used[0],
                                    "patch_at": used[1]})
        return results

    def _classify_accesses(self, function, insns: list, seed: int) -> list[dict]:
        """Return every TP-relative access reachable from one `mrs` site.

        A single `mrs xN, tpidr_el0` can feed the whole function: fmain keeps
        TP in x21 and uses it again in out-of-line cold paths tens of KB away
        (the lazy-init block for CrashHooks::hooks at 0x3525ab0).  Analysing a
        small window after the `mrs` leaves those blocks pointing at bionic's
        TLS, which splits the guard and the object it protects.  Walk the
        containing function's CFG instead, tracking every register that still
        holds TP.
        """
        by_address = {insn.address: index for index, insn in enumerate(insns)}
        seed_index = by_address.get(seed)
        if seed_index is None:
            return []
        dst = insns[seed_index].op_str.split(",")[0].strip()
        if not re.match(r"^x\d+$", dst):
            return []
        results: list[dict] = []
        seen: set[tuple[int, int]] = set()

        def add(kind: str, total: int, at: int):
            key = (at, total)
            if key not in seen:
                seen.add(key)
                results.append({"kind": kind, "tls_offset": total,
                                "patch_at": at})

        function_start, function_end, _ = function
        caller_saved = {f"x{number}" for number in range(19)}
        work = [(seed + 4, frozenset({dst}))]
        visited: set[tuple[int, frozenset[str]]] = set()
        steps = 0
        while work and steps < 200000:
            pc, registers = work.pop()
            while steps < 200000:
                if (pc, registers) in visited:
                    break
                visited.add((pc, registers))
                index = by_address.get(pc)
                if index is None:
                    break
                insn = insns[index]
                steps += 1
                mnemonic = insn.mnemonic
                if mnemonic in ("ret", "br", "brk", "udf"):
                    break
                text = f"{mnemonic} {insn.op_str}"
                match = ADD_RE.match(text)
                if match and match.group(2) in registers and match.group(4):
                    target = match.group(1)
                    total = parse_imm(match.group(3)) << 12
                    for follow in insns[index + 1:index + 24]:
                        ftext = f"{follow.mnemonic} {follow.op_str}"
                        if follow.mnemonic in ("ret", "br", "brk", "udf"):
                            break
                        fadd = ADD_RE.match(ftext)
                        if fadd and fadd.group(2) == target:
                            if not fadd.group(4):
                                total += parse_imm(fadd.group(3))
                            break
                        fmem = MEM_RE.match(ftext)
                        if fmem and fmem.group(2) == target:
                            total += parse_imm(fmem.group(3) or "0")
                            break
                    add("hi", total, insn.address)

                # Follow copies (mov xD, xS) and forget TP as soon as a
                # register is overwritten.  Only caller-saved registers die at
                # a call boundary.
                copy = self._copy_register(insn)
                updated = set(registers)
                if copy:
                    source, dest = copy
                    if source in registers:
                        updated.add(dest)
                    else:
                        updated.discard(dest)
                for register in list(updated):
                    if copy and register == copy[1]:
                        continue
                    if self._writes_register(insn, register):
                        updated.discard(register)
                registers = frozenset(updated)

                if mnemonic == "b":
                    target = self._branch_target(insn)
                    if target is not None \
                            and function_start <= target < function_end:
                        work.append((target, registers))
                    break
                if mnemonic.startswith("b.") or mnemonic in (
                        "cbz", "cbnz", "tbz", "tbnz"):
                    target = self._branch_target(insn)
                    if target is not None \
                            and function_start <= target < function_end:
                        work.append((target, registers))
                elif mnemonic in ("bl", "blr"):
                    registers = frozenset(register for register in registers
                                          if register not in caller_saved)
                pc += 4

        if not results:
            results.extend(self._classify_constant(insns, seed_index, dst))
        return results

    def run(self) -> dict:
        if self.base % 4096 or (self.base >> 12) > 0xFFF:
            raise ELFError("TLS base must be page-aligned and <= 0xFFFF000")
        data = self.elf.data
        start = self.text["offset"]
        end = start + self.text["size"]
        sites = []
        for offset in range(start, end - 3, 4):
            word = struct.unpack_from("<I", data, offset)[0]
            if (word & MRS_TPIDR_EL0_MASK) == MRS_TPIDR_EL0:
                sites.append(offset)
        patched_hi = set()
        text_start = self.text["addr"]
        text_end = text_start + self.text["size"]
        for offset in sites:
            seed = self.elf.offset_to_vaddr(offset)
            function = self._function_for(seed)
            if function is None:
                # No symbol covers this code; fall back to a local window.
                function = (seed, min(seed + 0x1000, text_end), "<no symbol>")
            for result in self._classify_accesses(
                    function, self._function_instructions(function), seed):
                kind, total, insn_address = (result["kind"], result["tls_offset"],
                                             result["patch_at"])
                if not (0 <= total < self.tls_memsz + 0x1000):
                    self.unknown.append({"site": hex(seed), "kind": kind,
                                         "offset": hex(total)})
                    continue
                entry = {"site": hex(seed), "kind": kind,
                         "tls_offset": hex(total), "patch_at": hex(insn_address)}
                self.accesses.append(entry)
                patch_offset = self.elf.vaddr_to_offset(insn_address)
                if patch_offset in patched_hi:
                    entry["shared_patch"] = True
                    continue
                original = struct.unpack_from("<I", data, patch_offset)[0]
                if kind == "hi":
                    new = (original & 0xFFC003FF) | ((self.base >> 12) << 10)
                elif kind == "const":
                    # movz xN, #imm16, lsl #16 + movk low: add the base's high
                    # half so the pair yields TP + base + offset.
                    if self.base & 0xFFFF or (self.base >> 16) > 0xFFFF:
                        entry["reason"] = "TLS base not encodable in MOVZ"
                        self.unsupported.append(entry)
                        continue
                    imm16 = (original >> 5) & 0xFFFF
                    if imm16 + (self.base >> 16) > 0xFFFF:
                        entry["reason"] = "MOVZ immediate overflow"
                        self.unsupported.append(entry)
                        continue
                    new = original + ((self.base >> 16) << 5)
                else:
                    self.unsupported.append(entry)
                    continue
                struct.pack_into("<I", data, patch_offset, new)
                patched_hi.add(patch_offset)
                entry["original_word"] = f"0x{original:08x}"
                entry["patched_word"] = f"0x{new:08x}"
                self.patches.append(entry)
        return {"sites": len(sites), "tls_accesses": len(self.accesses),
                "patched": len(self.patches), "unsupported": self.unsupported,
                "unknown": self.unknown, "base": hex(self.base),
                "tls_memsz": hex(self.tls_memsz),
                "details": self.patches}


# ------------------------------------------------------------- SDL redirects
def sdl_public_symbols(header_dir: Path) -> set[str]:
    names: set[str] = set()
    pattern = re.compile(r"SDLCALL\s+(SDL_[A-Za-z0-9_]+)\s*\(")
    for header in sorted(header_dir.rglob("*.h")):
        text = header.read_text(errors="replace")
        names.update(pattern.findall(text))
    if not names:
        raise ELFError(f"no SDL public symbols found under {header_dir}")
    return names


def encode_thunk(pc: int, slot: int) -> bytes:
    pages = ((slot & ~0xFFF) - (pc & ~0xFFF)) >> 12
    if not -(1 << 20) <= pages < (1 << 20):
        raise ELFError("slot is out of adrp range")
    adrp = 0x90000010 | ((pages & 3) << 29) | (((pages >> 2) & 0x7FFFF) << 5)
    ldr = 0xF9400210 | (((slot & 0xFFF) // 8) << 10)
    return struct.pack("<4I", adrp, ldr, 0xD61F0200, 0xD503201F)


def install_redirects(elf: ELF, public: set[str], slot_base: int) -> dict:
    text = elf.section(".text")
    text_start, text_end = text["offset"], text["offset"] + text["size"]
    # The game's SDL3 is built with dynapi: the callable stubs are named
    # SDL_X_DEFAULT, not SDL_X. Accept either spelling.
    by_name = {symbol["name"]: symbol for symbol in elf.defined_function_symbols()}
    targets = []
    seen_addresses = set()
    for name in sorted(public):
        symbol = by_name.get(name) or by_name.get(name + "_DEFAULT")
        if not symbol:
            continue
        if symbol["shndx"] != text["index"]:
            continue
        if symbol["value"] in seen_addresses:
            continue
        seen_addresses.add(symbol["value"])
        targets.append(symbol)
    targets.sort(key=lambda s: s["value"])

    skipped = []
    records = []
    slot = slot_base
    for symbol in targets:
        if symbol["size"] < 12:
            skipped.append({"name": symbol["name"], "size": symbol["size"]})
            continue
        offset = elf.vaddr_to_offset(symbol["value"])
        if not (text_start <= offset < text_end) or offset % 4:
            skipped.append({"name": symbol["name"], "reason": "not aligned text"})
            continue
        original = bytes(elf.data[offset:offset + 12])
        thunk = encode_thunk(symbol["value"], slot)
        elf.put_bytes(offset, thunk)
        records.append({"name": symbol["name"], "address": hex(symbol["value"]),
                        "file_offset": hex(offset), "slot": hex(slot),
                        "original_bytes": original.hex()})
        slot += 8
    return {"count": len(records), "skipped": skipped, "records": records,
            "slots_end": hex(slot)}


# ----------------------------------------------------------------------- main
def verify_output(elf: ELF, expected_redirects: int, expected_tls: int) -> dict:
    problems = []
    if any(struct.unpack_from("<I", elf.data, p["header_offset"])[0] == PT_INTERP
           for p in elf.phdrs):
        problems.append("PT_INTERP still present")
    dynamic = elf.dynamic()
    tags = [e["tag"] for e in dynamic]
    if DT_VERSYM in tags or DT_VERNEED in tags:
        problems.append("versioning tags survived")
    needed = [elf.dynamic_string(e["value"]) for e in dynamic if e["tag"] == DT_NEEDED]
    if "libfshim.so" not in needed:
        problems.append("shim is not a DT_NEEDED")
    segment = elf.dynamic_segment()
    raw = [struct.unpack_from("<qQ", elf.data, segment["offset"] + i * 16)[0]
           for i in range(segment["filesz"] // 16)]
    if DT_NULL not in raw:
        problems.append("dynamic table not terminated")
    return {"needed": needed, "problems": problems,
            "redirects_expected": expected_redirects,
            "tls_patches_expected": expected_tls}


def emit_constants(elf: ELF, path: Path, tls_base: int) -> dict:
    main_symbol = next((s for s in elf.symbols()
                        if s["name"] == "main" and s["type"] == 2), None)
    if not main_symbol:
        raise ELFError("cannot find main() in the symbol table")
    text = elf.section(".text")
    header = (
        "/* Generated by tools/port.py. Do not edit. */\n"
        "#pragma once\n"
        f"#define FACTORIO_MAIN_VADDR 0x{main_symbol['value']:x}UL\n"
        f"#define FACTORIO_TLS_BASE 0x{tls_base:x}UL\n"
        f"#define FACTORIO_ENTRY_VADDR 0x{elf.e_entry:x}UL\n")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(header)
    return {"main": hex(main_symbol["value"]), "main_size": main_symbol["size"],
            "entry": hex(elf.e_entry), "header": str(path)}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--out", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--report", type=Path, default=DEFAULT_REPORT)
    parser.add_argument("--constants", type=Path,
                        default=ROOT / "build/game_constants.h")
    parser.add_argument("--game-version", default="unknown",
                        help="version from the selected Factorio archive (report metadata only)")
    parser.add_argument("--sdl-headers", type=Path,
                        help="SDL3 include directory (only needed for --sdl-redirects)")
    parser.add_argument("--sdl-redirects", action="store_true",
                        help="fallback: overwrite static SDL entry points with "
                             "thunks into a separate SDL3 build. Normally not "
                             "needed: the game's dynapi loads SDL3_DYNAMIC_API.")
    parser.add_argument("--tls-base", type=lambda s: int(s, 0),
                        default=DEFAULT_TLS_BASE,
                        help="page-aligned base offset added to local-exec TLS")
    parser.add_argument("--analyze", action="store_true",
                        help="report TLS/ELF facts without writing an image")
    parser.add_argument("--skip-tls", action="store_true")
    parser.add_argument("--allow-unsupported-tls", action="store_true",
                        help="continue even if an access cannot be rewritten")
    args = parser.parse_args(argv)

    if not args.source.is_file():
        parser.error(f"source binary not found: {args.source}")

    elf = ELF(args.source)
    report: dict = {
        "source": str(args.source),
        "version": args.game_version,
        "elf": {"type": elf.e_type, "machine": elf.e_machine,
                "phdrs": len(elf.phdrs), "sections": len(elf.shdrs),
                "needed": [elf.dynamic_string(e["value"]) for e in elf.dynamic()
                           if e["tag"] == DT_NEEDED]},
    }

    redirects = {"count": 0, "records": [], "skipped": [], "slots_end": None}
    public: set[str] = set()
    report["sdl"] = detect_sdl_version(elf)
    if report["sdl"].get("detected"):
        print(f"embedded SDL version: {report['sdl']['version']} "
              f"(expected {'.'.join(map(str, EXPECTED_SDL))})")
        if not report["sdl"]["matches_expected"]:
            print("warning: build the dynapi override from the version above",
                  file=sys.stderr)
    if args.sdl_redirects:
        if not args.sdl_headers:
            parser.error("--sdl-redirects requires --sdl-headers")
        public = sdl_public_symbols(args.sdl_headers)
        report["sdl"]["public_symbols"] = len(public)
        report["sdl"]["headers"] = str(args.sdl_headers)

    tls = None
    if not args.skip_tls:
        print("analysing local-exec TLS ...")
        rewriter = TLSRewriter(elf, args.tls_base)
        tls = rewriter.run()
        report["tls"] = {k: v for k, v in tls.items() if k != "details"}
        report["tls"]["patched_details"] = tls["details"]
        print(f"  tls accesses: {tls['tls_accesses']}, patched: {tls['patched']}, "
              f"unsupported: {len(tls['unsupported'])}, unknown: {len(tls['unknown'])}")
        if tls["unsupported"] and not args.allow_unsupported_tls:
            print("error: some TLS accesses cannot be relocated; see report",
                  file=sys.stderr)
            for item in tls["unsupported"][:10]:
                print("  ", item, file=sys.stderr)
            return 3

    if args.analyze:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, indent=2) + "\n")
        print(f"analysis written to {args.report}")
        return 0

    report["symbols"] = emit_constants(elf, args.constants, args.tls_base)
    print(f"main() at {report['symbols']['main']}; "
          f"constants written to {args.constants}")

    print("patching ELF ...")
    report["interp"] = remove_interp(elf)
    if public:
        # Slots live in a new zero-filled tail of the last writable segment.
        probe_slots = len(public) * 8 + 0x1000
        slot_base = extend_bss(elf, probe_slots)
        redirects = install_redirects(elf, public, slot_base)
        # Shrink nothing: the tail is already mapped zero-filled.
        report["sdl"]["redirects"] = redirects["count"]
        report["sdl"]["skipped"] = redirects["skipped"][:50]
        report["sdl"]["slots_base"] = hex(slot_base)
        report["sdl"]["records"] = redirects["records"]
        print(f"  SDL redirects: {redirects['count']} "
              f"(skipped {len(redirects['skipped'])}), slots at {hex(slot_base)}")
    report["imports"] = rename_imports(elf)
    print("  imports renamed: " + (", ".join(
        f"{r['from']}->{r['to']}" for r in report["imports"]["renamed"]) or "none"))
    report["dynamic"] = rewrite_dynamic(elf)
    print(f"  dynamic: dropped {len(report['dynamic']['dropped_tags'])}, "
          f"renamed {len(report['dynamic']['needed_renamed'])}")

    print("verifying ...")
    report["verify"] = verify_output(elf, redirects["count"],
                                     tls["patched"] if tls else 0)
    elf.write(args.out)
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2) + "\n")
    print(f"wrote {args.out}")
    print(f"report {args.report}")
    if report["verify"]["problems"]:
        for problem in report["verify"]["problems"]:
            print(f"  PROBLEM: {problem}", file=sys.stderr)
        return 4
    return 0


if __name__ == "__main__":
    sys.exit(main())
