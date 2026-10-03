#!/usr/bin/env python3
"""Gate: every call into libctru's linear heap allocator must go through the locked wrappers.

Background (SUBTITLE_CRASH_DELIVERY.md): libctru's linearAlloc/linearMemAlign/linearFree/linearSpaceFree (MemPool +
address rbtree) take no lock. r6 serialised only the app's own linearAlloc_concurrent()/linearFree_concurrent(), while
citro3d (C3D_TexInit on the thumbnail thread), Tex3DS, citro2d, mvdstd and linearSpaceFree() callers reached the
allocator directly from other threads. The Makefile now links with -Wl,--wrap=linearAlloc,... and
source/system/libctru_wrapper.cpp serialises the __wrap_* entry points.

Checks on the linked ELF (static call graph from the disassembly):
  1. each __wrap_* function exists and calls its original (a wrapper --gc-sections dropped because its original has
     no caller outside linear.o passes; linearAlloc/linearFree wrappers are always required);
  2. no branch or literal-pool address reference to an original comes from anything but its __wrap_* function or
     libctru's own linear.o (linearAlloc -> linearMemAlign stays inside that object and inside the lock);
  3. positive control: C3D_TexInitWithParams reaches the allocator through __wrap_linearAlloc.

Usage: check_linear_lock.py <elf>   (read-only; needs devkitARM binutils in $DEVKITARM/bin or on PATH)
Exit 0 = PASS, 1 = FAIL, 2 = usage / tool error.
"""
import collections
import os
import re
import subprocess
import sys

DEVKITPRO = os.environ.get("DEVKITPRO", "/opt/devkitpro")
ORIGINALS = ["linearAlloc", "linearMemAlign", "linearFree", "linearSpaceFree"]
# calls between the originals themselves happen inside libctru's linear.o (not wrapped by ld, already under the lock)
INTERNAL = {("linearAlloc", "linearMemAlign")}
# referenced by citro3d/citro2d/Tex3DS in every build, so their wrappers must be linked
REQUIRED_WRAPS = {"linearAlloc", "linearFree"}

results = []


def report(status, name, detail):
    results.append(status)
    print("%-4s %-34s %s" % (status, name, detail))


def tool(name):
    d = os.path.join(os.environ.get("DEVKITARM", os.path.join(DEVKITPRO, "devkitARM")), "bin")
    p = os.path.join(d, "arm-none-eabi-" + name)
    return p if os.path.exists(p) else "arm-none-eabi-" + name


def run(*args):
    p = subprocess.run(args, capture_output=True, text=True)
    if p.returncode != 0:
        sys.stderr.write("tool failed: %s\n%s\n" % (" ".join(args), p.stderr))
        sys.exit(2)
    return p.stdout


def main():
    if len(sys.argv) != 2:
        sys.stderr.write(__doc__)
        return 2
    elf = sys.argv[1]
    syms = {}
    for line in run(tool("nm"), elf).splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] in "Tt":
            syms[parts[2]] = int(parts[0], 16)
    missing = [n for n in ORIGINALS if n not in syms]
    if missing:
        report("FAIL", "originals present", "missing " + ",".join(missing))
        return 1
    addr_to_orig = {syms[n]: n for n in ORIGINALS}

    callers = collections.defaultdict(set)  # original -> {caller function}
    fn = None
    fn_re = re.compile(r"^[0-9a-f]+ <([^>]+)>:$")
    br_re = re.compile(r"\t(?:bl|blx|b)(?:eq|ne|cs|cc|mi|pl|vs|vc|hi|ls|ge|lt|gt|le|al)?\t([0-9a-f]+) <")
    word_re = re.compile(r"\t\.word\t0x([0-9a-f]+)")
    for line in run(tool("objdump"), "-d", elf).splitlines():
        m = fn_re.match(line)
        if m:
            fn = m.group(1)
            continue
        m = br_re.search(line) or word_re.search(line)
        if m:
            target = int(m.group(1), 16)
            if target in addr_to_orig:
                callers[addr_to_orig[target]].add(fn)

    for orig in ORIGINALS:
        wrap = "__wrap_" + orig
        external = [c for c in callers[orig] if c != wrap and (c, orig) not in INTERNAL]
        if wrap not in syms and orig not in REQUIRED_WRAPS and not external:
            # --gc-sections drops a wrapper nobody references; then there is nothing to route either
            report("PASS", wrap + " present", "not linked: %s has no caller outside linear.o" % orig)
        elif wrap not in syms:
            report("FAIL", wrap + " present", "missing: the allocator is not wrapped (Makefile --wrap / "
                   "libctru_wrapper.cpp)")
        elif wrap not in callers[orig]:
            report("FAIL", wrap + " -> " + orig, "wrapper does not call the original")
        else:
            report("PASS", wrap + " -> " + orig, "present")
        bad = sorted(c for c in callers[orig] if c != wrap and (c, orig) not in INTERNAL)
        if bad:
            report("FAIL", "direct callers of " + orig, ", ".join(bad))
        else:
            report("PASS", "direct callers of " + orig, "none outside %s / linear.o" % wrap)

    tex_init = syms.get("C3D_TexInitWithParams")
    wrapped_alloc = syms.get("__wrap_linearAlloc")
    if tex_init is None:
        report("FAIL", "C3D_TexInitWithParams", "symbol not found (positive control)")
    else:
        text = run(tool("objdump"), "-d", "--start-address=0x%x" % tex_init, "--stop-address=0x%x" % (tex_init + 0x400),
                   elf)
        if wrapped_alloc is not None and re.search(r"\tbl\t%x <__wrap_linearAlloc>" % wrapped_alloc, text):
            report("PASS", "C3D_TexInitWithParams", "calls __wrap_linearAlloc")
        else:
            report("FAIL", "C3D_TexInitWithParams", "does not call __wrap_linearAlloc")

    return 0 if all(s == "PASS" for s in results) else 1


if __name__ == "__main__":
    sys.exit(main())
