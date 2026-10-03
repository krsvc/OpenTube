#!/usr/bin/env python3
"""Read-only identity dump of a 3DS CIA (ticket / TMD / NCCH / exheader / SMDH / RomFS) and of a 3DSX.

Usage: scripts/inspect_cia.py <file.cia|file.3dsx> [more files...]
Prints one `key=value` line per fact so the output can be diffed / grepped; exits 1 if a container is malformed
or the title ids disagree with each other.  Python 3 stdlib only.  Layouts per 3dbrew (CIA, Ticket, Title
metadata, NCCH, NCCH/Extended Header, SMDH, 3DSX).
"""
import struct
import sys

MEDIA_UNIT = 0x200
SIG_SIZES = {0x10000: (0x200, 0x3C), 0x10001: (0x100, 0x3C), 0x10002: (0x3C, 0x40),
             0x10003: (0x200, 0x3C), 0x10004: (0x100, 0x3C), 0x10005: (0x3C, 0x40)}


def align(x, a):
    return (x + a - 1) & ~(a - 1)


def sig_header_len(buf, off):
    st = struct.unpack_from(">I", buf, off)[0]
    if st not in SIG_SIZES:
        raise ValueError("unknown signature type 0x%x at 0x%x" % (st, off))
    s, p = SIG_SIZES[st]
    return 4 + s + p, st


def utf16(b):
    return b.decode("utf-16-le", errors="replace").rstrip("\0")


def smdh_facts(prefix, blob, out):
    if blob[:4] != b"SMDH":
        out.append("%s.smdh.magic=INVALID(%r)" % (prefix, blob[:4]))
        return False
    en = 0x8 + 1 * 0x200  # application title struct 1 = English
    out.append("%s.smdh.short_en=%s" % (prefix, utf16(blob[en:en + 0x80])))
    out.append("%s.smdh.long_en=%s" % (prefix, utf16(blob[en + 0x80:en + 0x180])))
    out.append("%s.smdh.publisher_en=%s" % (prefix, utf16(blob[en + 0x180:en + 0x200])))
    return True


def inspect_cia(path, buf, out):
    ok = True
    hdr_size, typ, ver, cert_size, tik_size, tmd_size, meta_size, content_size = struct.unpack_from(
        "<IHHIIIIQ", buf, 0)
    out.append("cia.header_size=0x%x cia.type=%d cia.version=%d" % (hdr_size, typ, ver))
    cert_off = align(hdr_size, 0x40)
    tik_off = align(cert_off + cert_size, 0x40)
    tmd_off = align(tik_off + tik_size, 0x40)
    con_off = align(tmd_off + tmd_size, 0x40)
    meta_off = align(con_off + content_size, 0x40)
    out.append("cia.sizes cert=0x%x ticket=0x%x tmd=0x%x content=0x%x meta=0x%x file=0x%x" %
               (cert_size, tik_size, tmd_size, content_size, meta_size, len(buf)))
    if meta_off + meta_size > len(buf) + 0x40:
        out.append("cia.ERROR=section layout exceeds file size")
        ok = False
    # ticket
    tl, st = sig_header_len(buf, tik_off)
    td = tik_off + tl
    tik_title = struct.unpack_from(">Q", buf, td + 0x9C)[0]
    tik_issuer = buf[td:td + 0x40].split(b"\0")[0].decode(errors="replace")
    out.append("ticket.sig_type=0x%x ticket.issuer=%s" % (st, tik_issuer))
    out.append("ticket.title_id=%016X" % tik_title)
    out.append("ticket.title_version=%d" % struct.unpack_from(">H", buf, td + 0xA6)[0])
    # tmd
    ml, st2 = sig_header_len(buf, tmd_off)
    md = tmd_off + ml
    tmd_title = struct.unpack_from(">Q", buf, md + 0x4C)[0]
    tmd_ver = struct.unpack_from(">H", buf, md + 0x9C)[0]
    ncontent = struct.unpack_from(">H", buf, md + 0x9E)[0]
    out.append("tmd.sig_type=0x%x tmd.issuer=%s" % (st2, buf[md:md + 0x40].split(b"\0")[0].decode(errors="replace")))
    out.append("tmd.title_id=%016X tmd.title_version=%d tmd.content_count=%d" % (tmd_title, tmd_ver, ncontent))
    chunks = md + 0xC4 + 0x900
    for i in range(ncontent):
        cid, cidx, ctype, csize = struct.unpack_from(">IHHQ", buf, chunks + i * 0x30)
        out.append("tmd.content[%d] id=%08X index=%d type=0x%x size=0x%x" % (i, cid, cidx, ctype, csize))
    # first content = NCCH
    n = con_off
    if buf[n + 0x100:n + 0x104] != b"NCCH":
        out.append("ncch.magic=INVALID(%r)" % buf[n + 0x100:n + 0x104])
        return False
    partition_id = struct.unpack_from("<Q", buf, n + 0x108)[0]
    program_id = struct.unpack_from("<Q", buf, n + 0x118)[0]
    product_code = buf[n + 0x150:n + 0x160].split(b"\0")[0].decode(errors="replace")
    flags = buf[n + 0x188:n + 0x190]
    exh_size = struct.unpack_from("<I", buf, n + 0x180)[0]
    exefs_off, exefs_size = struct.unpack_from("<II", buf, n + 0x1A0)
    romfs_off, romfs_size = struct.unpack_from("<II", buf, n + 0x1B0)
    out.append("ncch.partition_id=%016X ncch.program_id=%016X" % (partition_id, program_id))
    out.append("ncch.product_code=%s" % product_code)
    out.append("ncch.flags=%s no_crypto=%s" % (flags.hex(), bool(flags[7] & 0x4)))
    out.append("ncch.exheader_size=0x%x" % exh_size)
    out.append("ncch.exefs offset=0x%x size=0x%x (media units)" % (exefs_off, exefs_size))
    out.append("ncch.romfs offset=0x%x size=0x%x (media units) present=%s" % (romfs_off, romfs_size, romfs_size > 0))
    # exheader (plain when no_crypto)
    ex = n + 0x200
    app_title = buf[ex:ex + 8].split(b"\0")[0].decode(errors="replace")
    sci_flags = buf[ex + 0xD]
    exh_program_id = struct.unpack_from("<Q", buf, ex + 0x200)[0]
    out.append("exheader.app_title=%s exheader.program_id=%016X compress_code=%s sd_app=%s" %
               (app_title, exh_program_id, bool(sci_flags & 1), bool(sci_flags & 2)))
    ids = {tik_title, tmd_title, partition_id, program_id, exh_program_id}
    out.append("identity.all_title_ids_agree=%s (%s)" % (len(ids) == 1, ",".join("%016X" % i for i in sorted(ids))))
    ok &= len(ids) == 1
    # exefs: icon (SMDH)
    if exefs_size:
        e = n + exefs_off * MEDIA_UNIT
        names = []
        for i in range(10):
            name = buf[e + i * 16:e + i * 16 + 8].split(b"\0")[0].decode(errors="replace")
            off, size = struct.unpack_from("<II", buf, e + i * 16 + 8)
            if name:
                names.append("%s:0x%x" % (name, size))
                if name == "icon":
                    ok &= smdh_facts("ncch", buf[e + 0x200 + off:e + 0x200 + off + size], out)
        out.append("ncch.exefs.files=%s" % " ".join(names))
    # romfs IVFC magic
    if romfs_size:
        r = n + romfs_off * MEDIA_UNIT
        l3 = struct.unpack_from("<I", buf, r + 0x1000)[0]
        out.append("ncch.romfs.ivfc_magic=%r level3_header_size=0x%x ok=%s" % (buf[r:r + 4], l3, buf[r:r + 4] == b"IVFC" and l3 == 0x28))
        ok &= buf[r:r + 4] == b"IVFC" and l3 == 0x28
    return ok


def inspect_3dsx(path, buf, out):
    if buf[:4] != b"3DSX":
        out.append("3dsx.magic=INVALID(%r)" % buf[:4])
        return False
    hdr_size, reloc_hdr, version, flags, code, rodata, data, bss = struct.unpack_from("<HHIIIIII", buf, 4)
    out.append("3dsx.header_size=0x%x version=%d flags=0x%x code=0x%x rodata=0x%x data=0x%x bss=0x%x" %
               (hdr_size, version, flags, code, rodata, data, bss))
    ok = True
    if hdr_size >= 0x2C:
        smdh_off, smdh_size, romfs_off = struct.unpack_from("<III", buf, 0x20)
        out.append("3dsx.smdh offset=0x%x size=0x%x" % (smdh_off, smdh_size))
        out.append("3dsx.romfs offset=0x%x present=%s" % (romfs_off, romfs_off != 0))
        if smdh_size:
            ok &= smdh_facts("3dsx", buf[smdh_off:smdh_off + smdh_size], out)
        if romfs_off:
            # 3dsxtool embeds the bare RomFS level-3 image (no IVFC wrapper); its header starts with its own size 0x28
            l3 = struct.unpack_from("<I", buf, romfs_off)[0]
            out.append("3dsx.romfs.level3_header_size=0x%x ok=%s" % (l3, l3 == 0x28))
            ok &= l3 == 0x28
    else:
        out.append("3dsx.extended_header=absent (no embedded SMDH/RomFS)")
    return ok


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    ok = True
    for path in argv[1:]:
        with open(path, "rb") as f:
            buf = f.read()
        out = ["file=%s size=%d" % (path, len(buf))]
        try:
            if path.lower().endswith(".3dsx") or buf[:4] == b"3DSX":
                ok &= inspect_3dsx(path, buf, out)
            else:
                ok &= inspect_cia(path, buf, out)
        except Exception as e:  # malformed container
            out.append("ERROR=%s" % e)
            ok = False
        print("\n".join(out))
        print()
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
