#!/usr/bin/env python3
"""OpenTube r15: build the updater's scoped trust bundle (romfs/cert/update_roots.pem).

Takes the named public root certificates from a Mozilla-derived CA bundle (PRIMARY) and requires every one of them to
be present byte-identical in a second, independent bundle (SECONDARY). Prints subject, SHA-256 fingerprint and expiry.
Only public roots; no private key, no system trust store change.

Usage: make_update_roots.py PRIMARY_BUNDLE SECONDARY_BUNDLE OUT_PEM
"""
import subprocess, sys

# GitHub (api.github.com, github.com): Sectigo E46 / R46, cross-signed by USERTrust ECC / RSA.
# GitHub's release asset CDN (objects / release-assets.githubusercontent.com): Let's Encrypt -> ISRG roots.
WANTED = [
    "USERTrust ECC Certification Authority",
    "USERTrust RSA Certification Authority",
    "Sectigo Public Server Authentication Root E46",
    "Sectigo Public Server Authentication Root R46",
    "ISRG Root X1",
    "ISRG Root X2",
    "ISRG Root YE",
    "ISRG Root YR",
]


def split(path):
    certs, cur = [], None
    for line in open(path, encoding="ascii", errors="replace"):
        if line.startswith("-----BEGIN CERTIFICATE-----"):
            cur = [line]
        elif cur is not None:
            cur.append(line)
            if line.startswith("-----END CERTIFICATE-----"):
                certs.append("".join(cur))
                cur = None
    return certs


def info(pem):
    out = subprocess.run(["openssl", "x509", "-noout", "-subject", "-issuer", "-fingerprint", "-sha256", "-enddate",
                          "-nameopt", "multiline,utf8"], input=pem.encode(), capture_output=True, check=True).stdout
    text = out.decode()
    cn = issuer_cn = fp = end = ""
    section = None
    for line in text.splitlines():
        s = line.strip()
        if line.startswith("subject="):
            section = "s"
        elif line.startswith("issuer="):
            section = "i"
        elif s.startswith("commonName"):
            value = s.split("=", 1)[1].strip()
            if section == "s":
                cn = value
            elif section == "i":
                issuer_cn = value
        elif "Fingerprint=" in line:
            fp = line.split("=", 1)[1].strip()
        elif line.startswith("notAfter="):
            end = line.split("=", 1)[1].strip()
    return cn, issuer_cn, fp, end


def main():
    primary, secondary, out = sys.argv[1], sys.argv[2], sys.argv[3]
    second = {info(p)[2]: p for p in split(secondary)}
    chosen = {}
    for pem in split(primary):
        cn, issuer, fp, end = info(pem)
        if cn in WANTED and cn == issuer:  # self-signed root only
            if cn in chosen:
                sys.exit("two roots named %r in %s" % (cn, primary))
            chosen[cn] = (pem, fp, end)
    missing = [w for w in WANTED if w not in chosen]
    body = ["# OpenTube updater trust roots (scripts/make_update_roots.py). Public root certificates only, used for\n",
            "# api.github.com / github.com and GitHub's release asset CDN; nothing else is trusted by the updater.\n"]
    for name in WANTED:
        if name not in chosen:
            continue
        pem, fp, end = chosen[name]
        agree = fp in second and second[fp].strip() == pem.strip()
        print("%-48s %s  notAfter %s  %s" % (name, fp, end, "matches secondary" if agree else "NOT IN SECONDARY"))
        if not agree:
            sys.exit("root %r differs or is missing in %s" % (name, secondary))
        body.append("# %s\n# SHA256 %s  notAfter %s\n" % (name, fp, end))
        body.append(pem)
    if missing:
        print("not in the primary bundle (left out): " + ", ".join(missing))
    open(out, "w", encoding="ascii").write("".join(body))
    print("wrote %s: %d roots" % (out, len(chosen)))


if __name__ == "__main__":
    main()
