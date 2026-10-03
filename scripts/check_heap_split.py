#!/usr/bin/env python3
"""Gate: the diagnostic ELF must run with the heap/linear heap split of the bundled libctru fork.

Background (PLAYBACK_CRASH_DELIVERY.md): upstream FourthTube links the bundled libctru fork, whose
__system_allocateHeaps caps the application heap at 30 MiB / the linear heap at 50 MiB; libctru 2.7.0 (linked by
diagnostic builds for ABI pairing) uses 24 MiB / 32 MiB. Revision 3 therefore ran a New 3DS with ~18 MiB less linear
memory than upstream. Diagnostic builds wrap __system_allocateHeaps (source/system/heap_split.cpp).

Usage: check_heap_split.py <elf>   (read-only; needs devkitARM binutils on PATH or in $DEVKITARM/bin)
Exit 0 = PASS, 1 = FAIL, 2 = usage / tool error.
"""
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEVKITPRO = os.environ.get("DEVKITPRO", "/opt/devkitpro")
FORK_ARCHIVE = os.path.join(ROOT, "library/libctru/lib/libctru.a")
INSTALLED_ARCHIVE = os.path.join(DEVKITPRO, "libctru/lib/libctru.a")


def tool(name):
    for d in (os.path.join(os.environ.get("DEVKITARM", os.path.join(DEVKITPRO, "devkitARM")), "bin"),):
        p = os.path.join(d, "arm-none-eabi-" + name)
        if os.path.exists(p):
            return p
    return "arm-none-eabi-" + name


def run(*args):
    return subprocess.run(args, capture_output=True, text=True, check=True).stdout


def split_caps(disasm):
    """(max_heap, max_linear) of allocateHeaps: the two cmp immediates of the split branch, in code order."""
    caps = [int(v, 16) for v in re.findall(r"\bcmp\s+r\d+, #\d+\s+@ (0x[0-9a-f]{6,})", disasm)]
    return tuple(caps[:2]) if len(caps) >= 2 else None


def archive_caps(archive):
    with tempfile.TemporaryDirectory() as d:
        subprocess.run([tool("ar"), "x", archive, "allocateHeaps.o"], cwd=d, check=True)
        return split_caps(run(tool("objdump"), "-d", os.path.join(d, "allocateHeaps.o")))


def func_disasm(elf, syms, name):
    if name not in syms:
        return None
    addr, size = syms[name]
    return run(tool("objdump"), "-d", "--start-address=0x%x" % addr, "--stop-address=0x%x" % (addr + size), elf)


COND = r"(eq|ne|cs|hs|cc|lo|mi|pl|vs|vc|hi|ls|ge|lt|gt|le)"
FLAG_OPS = {"add", "sub", "sbc", "adc", "orr", "and", "eor", "mov", "rsb", "bic", "mvn", "lsl", "lsr", "asr"}


def result_tested(disasm, call, stop_addr):
    """True if the r0 result of `bl <call>` sets flags that a conditional branch consumes before stop_addr.
    Follows the value through register moves and sp-relative spills/reloads; calls clobber r0-r3/ip."""
    insns = []
    for line in disasm.splitlines():
        m = re.match(r"\s*([0-9a-f]+):\s+(\S+)\s*(.*)$", line)
        if m:
            insns.append((int(m.group(1), 16), m.group(2), re.sub(r"\s*@.*", "", m.group(3)).strip()))
    start = next((i for i, (_, mn, ops) in enumerate(insns) if mn == "bl" and ("<%s>" % call) in ops), None)
    if start is None:
        return False
    tracked, pending = {"r0"}, False
    for addr, mn, ops in insns[start + 1:]:
        if addr >= stop_addr:
            return False
        regs = re.findall(r"\b(r\d+|ip|lr)\b", ops)
        if mn == "bl":
            tracked -= {"r0", "r1", "r2", "r3", "ip"}
            continue
        if re.fullmatch("b" + COND, mn):
            if pending:
                return True
            continue
        if mn in ("cmp", "cmn", "tst", "teq") or (mn.endswith("s") and mn[:-1] in FLAG_OPS):
            pending = any(r in tracked for r in (regs if mn in ("cmp", "cmn", "tst", "teq") else regs[1:]))
        spill = re.match(r"(r\d+), \[sp, #(\d+)\]$", ops)
        if mn == "str" and spill:
            slot = "sp+" + spill.group(2)
            (tracked.add if spill.group(1) in tracked else tracked.discard)(slot)
        elif mn == "ldr" and spill:
            (tracked.add if ("sp+" + spill.group(2)) in tracked else tracked.discard)(spill.group(1))
        elif mn == "mov" and len(regs) == 2 and "#" not in ops:
            (tracked.add if regs[1] in tracked else tracked.discard)(regs[0])
        elif regs and mn not in ("cmp", "cmn", "tst", "teq", "str", "strd", "push", "vstr"):
            tracked.discard(regs[0])  # destination overwritten
    return False


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    elf = sys.argv[1]
    syms = {}
    for line in run(tool("nm"), "-S", elf).splitlines():
        p = line.split()
        if len(p) == 4:
            syms[p[3]] = (int(p[0], 16), int(p[1], 16))
    results = []

    def check(name, ok, detail):
        results.append(ok)
        print("%s %-28s %s" % ("PASS" if ok else "FAIL", name, detail))

    fork = archive_caps(FORK_ARCHIVE)
    inst = archive_caps(INSTALLED_ARCHIVE)
    fmt = lambda c: "n/a" if not c else "heap<=0x%x linear<=0x%x (%d/%d MiB)" % (c[0], c[1], c[0] >> 20, c[1] >> 20)
    print("INFO fork allocateHeaps.o       %s  [%s]" % (fmt(fork), FORK_ARCHIVE))
    print("INFO installed allocateHeaps.o  %s  [%s]" % (fmt(inst), INSTALLED_ARCHIVE))
    check("archive-constants", fork is not None and inst is not None, "fork %s, installed %s" % (fmt(fork), fmt(inst)))

    init = func_disasm(elf, syms, "__libctru_init") or ""
    calls = re.findall(r"\bbl\s+[0-9a-f]+ <([^>]+)>", init)
    alloc_calls = [c for c in calls if "allocateHeaps" in c]
    check("init-calls-wrapper", alloc_calls == ["__wrap___system_allocateHeaps"],
          "__libctru_init calls %s" % (alloc_calls or "nothing named *allocateHeaps*"))

    wrap = func_disasm(elf, syms, "__wrap___system_allocateHeaps")
    if wrap is None:
        check("wrapper-present", False, "__wrap___system_allocateHeaps not in the ELF (heap split = installed libctru)")
    else:
        check("wrapper-present", True, "0x%x size %d" % syms["__wrap___system_allocateHeaps"])
        chained = re.findall(r"\bb(?:l)?\s+[0-9a-f]+ <(__system_allocateHeaps)>", wrap)
        check("wrapper-chains-real", bool(chained), "wrapper branches to __system_allocateHeaps: %s" % bool(chained))
        plain = run(tool("objdump"), "-d", "--no-show-raw-insn", "--start-address=0x%x" % syms["__wrap___system_allocateHeaps"][0],
                    "--stop-address=0x%x" % sum(syms["__wrap___system_allocateHeaps"]), elf)
        cap_addrs = [int(a, 16) for a, _ in re.findall(r"\s*([0-9a-f]+):\s+\S+\s+.*#(31457280|0x1e00000)\b", plain)]
        stop = min(cap_addrs) if cap_addrs else 0
        for call in ("svcGetResourceLimitLimitValues", "svcGetResourceLimitCurrentValues"):
            check("wrapper-tests-" + call[len("svcGetResourceLimit"):], result_tested(plain, call, stop),
                  "%s result tested by a conditional branch before the split arithmetic (0x%x)" % (call, stop))
        wrap_caps = split_caps(wrap)
        check("wrapper-uses-fork-caps", fork is not None and wrap_caps == fork,
              "wrapper %s vs fork %s" % (fmt(wrap_caps), fmt(fork)))

    real = func_disasm(elf, syms, "__system_allocateHeaps") or ""
    real_caps = split_caps(real)
    print("INFO linked __system_allocateHeaps %s" % fmt(real_caps))

    n_fail = results.count(False)
    print("RESULT: %s (%d PASS, %d FAIL)" % ("PASS" if n_fail == 0 else "FAIL", results.count(True), n_fail))
    return 0 if n_fail == 0 else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (subprocess.CalledProcessError, OSError) as e:
        print("ERROR %s" % e)
        sys.exit(2)
