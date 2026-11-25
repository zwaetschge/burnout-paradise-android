#!/usr/bin/env python3
"""Pre-release IP, binary, and privacy audit for the public repository.

Fails with exit code 1 if any game binary/data file, compiled artifact, private
host path, internal infrastructure name, private LAN IP, key material or
copy-protection tooling is found in the tree.
"""
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

FORBIDDEN_EXTS = {
    ".exe", ".dll", ".so", ".apk", ".idsig", ".o", ".a", ".cab", ".nls",
    ".bin", ".bundle", ".bndl", ".dat", ".dxcq", ".dxcr", ".dds", ".wav", ".vp6",
    ".keystore", ".jks", ".pem", ".key", ".pk8",
    ".jpg", ".jpeg", ".png", ".webp", ".bmp",
}

FORBIDDEN_PATTERNS = [
    (re.compile(r"/mnt/cache|/mnt/user|/srv/plum|/home/node"), "internal mount path"),
    (re.compile(r"zwaetschge(?![/ ]|$)|zwaetschge/(?!burnout-paradise-android)|Zwischenspeicher|plum-hub|geekom", re.I | re.M), "internal host/share name"),
    (re.compile(r"192\.168\.\d+\.\d+|10\.10\.10\."), "private LAN IP"),
    (re.compile(r"-----BEGIN (RSA |EC |OPENSSH )?PRIVATE KEY-----"), "private key material"),
    (re.compile(r"outer-unwrapped|OriginStubDecrypter|origin_wrapper|\.dlf\b|denuvo", re.I), "copy-protection tooling"),
    (re.compile(r"port/private|port/evidence"), "private workspace reference"),
]

SKIP_DIRS = {".git", "work", "out", "mods", "third_party", "__pycache__"}

# Self-exemption: this audit script defines the forbidden regexes themselves.
SELF_REL = Path("scripts/audit-clean-tree.py")

# Explicitly allowlisted curated README screenshots.
ALLOWED_IMAGE_PATTERNS = (
    re.compile(r"^docs/images/[a-z0-9-]+\.jpg$"),
)

# The seed list is addresses only (one 0x%08X per line plus comments).
SEED_FILE = re.compile(r"^seeds/[a-z0-9.-]+\.txt$")


def main() -> int:
    errors = []
    file_count = 0
    for p in sorted(REPO.rglob("*")):
        if not p.is_file():
            continue
        rel = p.relative_to(REPO)
        if any(part in SKIP_DIRS for part in rel.parts):
            continue
        file_count += 1
        s = rel.as_posix()
        if any(pat.match(s) for pat in ALLOWED_IMAGE_PATTERNS):
            continue
        if p.suffix.lower() in FORBIDDEN_EXTS:
            errors.append(f"FORBIDDEN_EXT: {rel}")
            continue
        if SEED_FILE.match(s):
            bad = [ln for ln in p.read_text().splitlines()
                   if ln and not ln.startswith("#") and not re.fullmatch(r"0x[0-9A-F]{8}", ln)]
            if bad:
                errors.append(f"SEED_FORMAT: {rel} ({bad[0]!r})")
            continue
        if rel == SELF_REL:
            continue
        try:
            text = p.read_text(encoding="utf-8", errors="ignore")
        except OSError as exc:
            errors.append(f"READ_ERR: {rel}: {exc}")
            continue
        for pat, label in FORBIDDEN_PATTERNS:
            m = pat.search(text)
            if m:
                errors.append(f"FORBIDDEN_{label.upper().replace(' ', '_').replace('-', '_')}: {rel} ({m.group(0)!r})")

    if errors:
        print(f"AUDIT FAILED ({len(errors)} findings across {file_count} files):", file=sys.stderr)
        for e in errors[:100]:
            print("  -", e, file=sys.stderr)
        return 1

    print(f"AUDIT PASS: {file_count} files verified clean "
          "(0 game files/binaries, 0 private paths/IPs/keys, 0 copy-protection tooling)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
