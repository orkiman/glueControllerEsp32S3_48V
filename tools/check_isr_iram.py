#!/usr/bin/env python3
"""Check that everything an interrupt can reach lives in IRAM / DRAM.

The GPIO, PCNT and timer interrupts are registered with ESP_INTR_FLAG_IRAM, so
they keep running while Core 0 writes flash (program / config autosave).  At
that moment the flash cache is off: any call into flash code (0x42xxxxxx) or
read of flash rodata / PSRAM (0x3Cxxxxxx-0x3Dxxxxxx) panics with "Cache
disabled but cached memory region accessed" and the controller reboots.

This script disassembles the IRAM text of the firmware ELF, starts at the
interrupt entry points, follows every direct call / tail jump and every
literal that points at another IRAM function, and reports any call target or
literal value inside flash or PSRAM.

Usage:  python tools/check_isr_iram.py [path/to/firmware.elf]
Exit code 0 = clean, 1 = problems found.
"""
import os
import re
import subprocess
import sys
from collections import deque

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DEFAULT_ELF = os.path.join(ROOT, ".pio", "build", "esp32s3_devkitc_n16r8", "firmware.elf")
OBJDUMP = os.path.expanduser(
    "~/.platformio/packages/toolchain-xtensa-esp32s3/bin/xtensa-esp32s3-elf-objdump")

# Interrupt entry points and functions reached through function pointers that
# a static walk cannot see (callbacks stored in variables).
ROOTS = [
    "seq::peakIsr(void*)",
    "seq::onCloseAlarm()",
    "seq::onSupervisorTick()",
    "pattern::tickHook(int*)",
    "rttimer::closeIsr(void*)",
    "rttimer::tickIsr(void*)",
    "encoder::photocellIsr(void*)",
    "encoder::pcntOverflowIsr(void*)",
    "fault::faultIsr(void*)",
    "net::onNetEvent(evt::Event const&, void*)",
]

FLASH_CODE = (0x42000000, 0x44000000)
EXT_DATA = (0x3C000000, 0x3E000000)     # flash rodata and PSRAM data
IRAM = (0x40370000, 0x403E0000)

# Failure paths: they only run once something is already broken and end in a
# panic anyway.  Their message strings live in flash by design (ESP-IDF asserts),
# so literals that only feed these calls are not reported and they are not
# walked into.
TERMINAL = {"__assert_func", "abort", "esp_system_abort", "panic_abort"}

# Flash functions whose address appears in IRAM code on a path we never take:
#   compare_and_set_extram - ESP-IDF spinlock path for locks stored in PSRAM;
#                            every portMUX in this firmware is in internal RAM.
ALLOWED_FLASH_REFS = {"compare_and_set_extram"}
NM = OBJDUMP.replace("objdump", "nm")


def in_range(addr, rng):
    return rng[0] <= addr < rng[1]


def run(args):
    return subprocess.run(args, capture_output=True, text=True, check=True).stdout


def load_words(elf):
    """Map address -> 32-bit little-endian word for the IRAM text section."""
    dump = run([OBJDUMP, "-s", "-j", ".iram0.text", elf])
    data = {}
    for line in dump.splitlines():
        m = re.match(r"^\s*([0-9a-f]{8})\s((?:[0-9a-f]{2,8}\s?){1,4})", line)
        if not m:
            continue
        addr = int(m.group(1), 16)
        hexbytes = "".join(m.group(2).split())
        for i in range(0, len(hexbytes), 2):
            data[addr + i // 2] = int(hexbytes[i:i + 2], 16)
    return data


def word_at(data, addr):
    try:
        return data[addr] | data[addr + 1] << 8 | data[addr + 2] << 16 | data[addr + 3] << 24
    except KeyError:
        return None


def parse_functions(elf):
    """Return {name: (start, end, [instruction lines])} for .iram0.text."""
    text = run([OBJDUMP, "-d", "-C", "-j", ".iram0.text", elf])
    funcs = {}
    starts = []
    cur = None
    for line in text.splitlines():
        m = re.match(r"^([0-9a-f]{8}) <(.+)>:$", line)
        if m:
            cur = m.group(2)
            start = int(m.group(1), 16)
            funcs[cur] = [start, start, []]
            starts.append((start, cur))
            continue
        if cur is None:
            continue
        m = re.match(r"^\s*([0-9a-f]{8}):\s+(?:[0-9a-f]{2} ?)+\s+(.*)$", line)
        if m:
            addr = int(m.group(1), 16)
            funcs[cur][1] = addr
            funcs[cur][2].append((addr, m.group(2)))
    by_addr = {start: name for start, name in starts}
    return funcs, by_addr


def feeds_failure_call(insns, index, start, end, by_addr):
    """True if the instructions after insns[index] reach a call to a TERMINAL
    function within a few steps (following one unconditional jump), i.e. the
    literal loaded at insns[index] is only an assert / abort argument."""
    pos = {addr: i for i, (addr, _) in enumerate(insns)}
    i, jumps = index + 1, 0
    for _ in range(10):
        if i >= len(insns):
            return False
        ins = insns[i][1]
        m = re.search(r"\bcall(?:[048]|12)\s+(?:0x)?([0-9a-f]{8})", ins)
        if m:
            return by_addr.get(int(m.group(1), 16)) in TERMINAL
        m = re.match(r"j\s+(?:0x)?([0-9a-f]{8})", ins)
        if m:
            target = int(m.group(1), 16)
            if jumps or not (start <= target <= end) or target not in pos:
                return False
            i, jumps = pos[target], 1
            continue
        i += 1
    return False


def load_symbols(elf):
    """Map address -> symbol name for every symbol in the ELF."""
    syms = {}
    for line in run([NM, "-C", elf]).splitlines():
        parts = line.split(" ", 2)
        if len(parts) == 3:
            try:
                syms.setdefault(int(parts[0], 16), parts[2])
            except ValueError:
                pass
    return syms


def main():
    elf = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_ELF
    words = load_words(elf)
    funcs, by_addr = parse_functions(elf)
    symbols = load_symbols(elf)

    problems = []
    missing = [r for r in ROOTS if r not in funcs]
    seen = set()
    queue = deque((r, [r]) for r in ROOTS if r in funcs)

    while queue:
        name, path = queue.popleft()
        if name in seen:
            continue
        seen.add(name)
        start, end, insns = funcs[name]
        for index, (addr, ins) in enumerate(insns):
            op = ins.split()[0] if ins else ""
            targets = []
            m = re.search(r"\b(call[048]|call12|j)\s+(?:0x)?([0-9a-f]{8})", ins)
            if m:
                targets.append(int(m.group(2), 16))
            if op == "l32r":
                m = re.search(r"l32r\s+\w+,\s*(?:0x)?([0-9a-f]{8})", ins)
                if m:
                    value = word_at(words, int(m.group(1), 16))
                    if value is not None:
                        if in_range(value, FLASH_CODE) or in_range(value, EXT_DATA):
                            sym = symbols.get(value, "?")
                            if (sym not in ALLOWED_FLASH_REFS
                                    and not feeds_failure_call(insns, index, start, end, by_addr)):
                                problems.append(f"{' -> '.join(path)}: literal 0x{value:08x} <{sym}>"
                                                f" at 0x{addr:08x} ({ins})")
                        elif value in by_addr:
                            targets.append(value)
            for t in targets:
                if in_range(t, FLASH_CODE):
                    problems.append(f"{' -> '.join(path)}: call into flash 0x{t:08x}"
                                    f" <{symbols.get(t, '?')}> at 0x{addr:08x} ({ins})")
                elif start <= t <= end:
                    continue                      # branch inside the same function
                elif t in by_addr:
                    callee = by_addr[t]
                    if callee not in seen and callee not in TERMINAL:
                        queue.append((callee, path + [callee]))
                # ROM (0x4000xxxx) and other IRAM addresses are fine.

    print(f"checked {len(seen)} IRAM functions reachable from {len(ROOTS) - len(missing)} roots")
    for r in missing:
        print(f"WARNING: root not found in IRAM (inlined, renamed or in flash?): {r}")
    if problems:
        print("PROBLEMS:")
        for p in problems:
            print("  " + p)
        return 1
    print("OK: no flash or PSRAM reference reachable from an interrupt")
    return 0


if __name__ == "__main__":
    sys.exit(main())
