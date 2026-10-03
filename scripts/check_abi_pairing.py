#!/usr/bin/env python3
"""Binary / link-layout regression gate for the FourthTube diagnostic build.

Background (FOURTHTUBE-STARTUP-FIX-32506): the delivered revision-2 ELF crashed at startup in
libctru's romfs_open -> navigateToDir because the BUNDLED libctru fork (library/libctru, built with an
older devkitARM/newlib) reads `struct _reent::deviceData` at 0x13c while the INSTALLED newlib/libsysbase
(`_open_r`) stores it at 0x1fc.  This gate fails on that class of mismatch:

  1. every devoptab entry point that dereferences `r->deviceData` (romfs_*, archive_*) and libsysbase's
     `_open_r` must use the SAME struct offset, and that offset must equal offsetof(struct _reent,
     deviceData) as the installed toolchain headers define it (compiled probe, not a hard-coded number);
  2. the link map must not pull any member from the bundled fork archive; libctru device code must come
     from $DEVKITPRO/libctru/lib; libsysbase's pthread.o and the bundled mbedtls-2.16 libcurl must be absent;
  3. the ELF must not carry the fork's debug source paths (provenance check);
  4. TLS support the pthread shim relies on must be present (.tbss, one __aeabi_read_tp).

Usage: scripts/check_abi_pairing.py <elf> <map> [--devkitpro DIR] [--devkitarm DIR]
Exit status 0 = all checks PASS, 1 = at least one FAIL, 2 = usage / tool error.
Only reads its inputs; writes a probe object into a temporary directory.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile

ARCH = ["-march=armv6k", "-mtune=mpcore", "-mfloat-abi=hard", "-mtp=soft", "-D__3DS__"]
# devoptab entry points that read r->deviceData; static symbols, so we find them by name in the ELF
DEVICE_FUNCS = [
    "romfs_open", "romfs_chdir", "romfs_diropen", "romfs_stat",
    "archive_open", "archive_chdir", "archive_diropen", "archive_mkdir", "archive_rename",
    "archive_rmdir", "archive_stat", "archive_unlink",
]
LIBC_FUNCS = ["_open_r"]
FORK_ARCHIVE_SUFFIX = "library/libctru/lib/libctru.a"
FORK_DEBUG_PATH = b"library/libctru/source/libctru"
BUNDLED_CURL_SUFFIX = "library/libcurl/lib/libcurl.a"

results = []


def report(status, name, detail):
    results.append((status, name, detail))
    print("%-4s %-28s %s" % (status, name, detail))


def run(cmd):
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if p.returncode != 0:
        sys.stderr.write("tool failed: %s\n%s\n" % (" ".join(cmd), p.stderr))
        sys.exit(2)
    return p.stdout


def tool(bindir, name):
    path = os.path.join(bindir, name) if bindir else name
    return path


def probe_expected_offset(gcc, nm, devkitpro):
    """offsetof(struct _reent, deviceData) and sizeof(struct _reent) from the installed headers."""
    src = (
        "#include <stddef.h>\n#include <sys/reent.h>\n"
        "const char abi_probe_deviceData[offsetof(struct _reent, deviceData)] = {1};\n"
        "const char abi_probe_sizeof_reent[sizeof(struct _reent)] = {1};\n"
    )
    with tempfile.TemporaryDirectory() as td:
        c = os.path.join(td, "probe.c")
        o = os.path.join(td, "probe.o")
        with open(c, "w") as f:
            f.write(src)
        run([gcc] + ARCH + ["-I" + os.path.join(devkitpro, "libctru", "include"), "-c", c, "-o", o])
        out = run([nm, "-S", o])
    sizes = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 4:
            sizes[parts[3]] = int(parts[1], 16)
    return sizes["abi_probe_deviceData"], sizes["abi_probe_sizeof_reent"]


def elf_symbols(nm, elf):
    """sorted list of (addr, type, name) for code symbols"""
    syms = []
    for line in run([nm, "-n", elf]).splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] in "tTwW":
            syms.append((int(parts[0], 16), parts[1], parts[2]))
    return syms


def function_range(syms, name):
    for i, (addr, _t, n) in enumerate(syms):
        if n == name:
            end = None
            for j in range(i + 1, len(syms)):
                if syms[j][0] > addr:
                    end = syms[j][0]
                    break
            return addr, end
    return None


IMM_RE = re.compile(r"\b(ldr|str)\s+(r\d+|sl|fp|ip|lr|sp),\s*\[(r\d+|sl|fp|ip|sp),\s*#(\d+)\]")


def struct_offsets(objdump, elf, start, end, lo=0x100, hi=0x3FF):
    """immediates of non-pc-relative ldr/str [reg, #imm] in [start,end) within the struct _reent range"""
    out = run([objdump, "-d", "--start-address=0x%x" % start, "--stop-address=0x%x" % end, elf])
    imms = []
    for line in out.splitlines():
        m = IMM_RE.search(line)
        if m:
            imm = int(m.group(4))
            if lo <= imm <= hi:
                imms.append(imm)
    return imms


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elf")
    ap.add_argument("map")
    ap.add_argument("--devkitpro", default=os.environ.get("DEVKITPRO", "/opt/devkitpro"))
    ap.add_argument("--devkitarm", default=os.environ.get("DEVKITARM", ""))
    a = ap.parse_args()
    devkitarm = a.devkitarm or os.path.join(a.devkitpro, "devkitARM")
    bindir = os.path.join(devkitarm, "bin")
    gcc, nm, objdump, readelf = (tool(bindir, "arm-none-eabi-" + t) for t in ("gcc", "nm", "objdump", "readelf"))
    for t in (gcc, nm, objdump, readelf):
        if not os.access(t, os.X_OK):
            sys.stderr.write("missing tool %s\n" % t)
            return 2

    print("gate: %s\nelf:  %s\nmap:  %s" % (os.path.basename(__file__), a.elf, a.map))

    # --- 1. struct _reent::deviceData offset agreement -------------------------------------------
    expected, reent_size = probe_expected_offset(gcc, nm, a.devkitpro)
    report("INFO", "toolchain-probe", "offsetof(struct _reent, deviceData)=0x%x sizeof(struct _reent)=0x%x (%s)"
           % (expected, reent_size, gcc))
    syms = elf_symbols(nm, a.elf)
    per_func = {}
    missing = []
    for fn in LIBC_FUNCS + DEVICE_FUNCS:
        r = function_range(syms, fn)
        if r is None or r[1] is None:
            missing.append(fn)
            continue
        per_func[fn] = sorted(set(struct_offsets(objdump, a.elf, r[0], r[1])))
    if missing:
        report("FAIL", "symbols-present", "not found in ELF: %s" % ", ".join(missing))
    else:
        report("PASS", "symbols-present", "%d device/libc functions located" % len(per_func))
    libc_set = set()
    for fn in LIBC_FUNCS:
        libc_set |= set(per_func.get(fn, []))
    dev_set = set()
    for fn in DEVICE_FUNCS:
        dev_set |= set(per_func.get(fn, []))
    detail = "libsysbase _open_r uses %s; devoptab code uses %s; header says 0x%x" % (
        "/".join("0x%x" % i for i in sorted(libc_set)) or "none",
        "/".join("0x%x" % i for i in sorted(dev_set)) or "none", expected)
    for fn in LIBC_FUNCS + DEVICE_FUNCS:
        if fn in per_func:
            print("     %-22s %s" % (fn, " ".join("0x%x" % i for i in per_func[fn]) or "(no struct-range access)"))
    ok = bool(libc_set) and bool(dev_set) and libc_set == {expected} and dev_set == {expected} and not missing
    report("PASS" if ok else "FAIL", "deviceData-offset", detail)

    # --- 2. link map provenance ------------------------------------------------------------------
    with open(a.map, errors="replace") as f:
        mp = f.read()
    members = {}
    for m in re.finditer(r"(\S+\.a)\(([^)\s]+)\)", mp):
        members.setdefault(m.group(2), set()).add(m.group(1))
    archives = sorted({p for s in members.values() for p in s})
    fork_members = sorted(k for k, v in members.items() if any(p.endswith(FORK_ARCHIVE_SUFFIX) for p in v))
    report("PASS" if not fork_members else "FAIL", "no-bundled-libctru",
           "members from %s: %d%s" % (FORK_ARCHIVE_SUFFIX, len(fork_members),
                                      (" (" + ", ".join(fork_members[:6]) + (", ..." if len(fork_members) > 6 else "") + ")") if fork_members else ""))
    inst_prefix = os.path.join(a.devkitpro, "libctru", "lib") + os.sep
    bad = []
    for obj in ("romfs_dev.o", "archive_dev.o", "syscalls.o", "thread.o", "synchronization.o"):
        srcs = members.get(obj, set())
        if not srcs or not all(p.startswith(inst_prefix) for p in srcs):
            bad.append("%s<-%s" % (obj, ",".join(sorted(srcs)) or "missing"))
    report("PASS" if not bad else "FAIL", "installed-libctru-devices",
           "romfs/archive/syscalls/thread/synchronization from %s" % inst_prefix if not bad else "; ".join(bad))
    open_srcs = members.get("libsysbase_libsysbase_a-open.o", set())
    report("PASS" if open_srcs and all("libsysbase.a" in p for p in open_srcs) else "FAIL", "libsysbase-open_r",
           ", ".join(sorted(open_srcs)) or "missing")
    pth = sorted(k for k, v in members.items() if k.endswith("pthread.o") and any("libsysbase" in p for p in v))
    report("PASS" if not pth else "FAIL", "no-libsysbase-pthread", "libsysbase pthread members: %d" % len(pth))
    curl_paths = sorted(p for p in archives if p.endswith("libcurl.a"))
    bundled_curl = [p for p in curl_paths if p.endswith(BUNDLED_CURL_SUFFIX)]
    report("PASS" if curl_paths and not bundled_curl else "FAIL", "curl-archive",
           ", ".join(curl_paths) or "no libcurl in map")
    print("     linked archives (%d):" % len(archives))
    for p in archives:
        print("       " + p)

    # --- 3. ELF provenance strings ---------------------------------------------------------------
    with open(a.elf, "rb") as f:
        blob = f.read()
    fork_hits = blob.count(FORK_DEBUG_PATH)
    inst_hits = blob.count(b"libctru-2.7.0") + blob.count(b"/libctru/lib/")
    report("PASS" if fork_hits == 0 else "FAIL", "no-fork-debug-paths",
           "'%s' occurrences: %d (installed libctru markers: %d)" % (FORK_DEBUG_PATH.decode(), fork_hits, inst_hits))

    # --- 4. TLS support for the __thread based pthread shim ---------------------------------------
    sections = run([readelf, "-S", "-W", a.elf])
    has_tbss = ".tbss" in sections
    tp = [s for s in syms if s[2] == "__aeabi_read_tp"]
    report("PASS" if has_tbss and len(tp) == 1 else "FAIL", "tls-support",
           ".tbss=%s __aeabi_read_tp definitions=%d" % (has_tbss, len(tp)))

    fails = [r for r in results if r[0] == "FAIL"]
    print("RESULT: %s (%d PASS, %d FAIL)" % ("FAIL" if fails else "PASS",
                                             sum(1 for r in results if r[0] == "PASS"), len(fails)))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
