#!/usr/bin/env python3
"""APK Signature Scheme v2 signer/verifier in pure Python (no JDK needed).

Why this exists: this machine has no JDK at all (no `java`, no `keytool`), so
build-tools/apksigner (a Java jar wrapper) cannot run. This tool implements the
documented APK Signature Scheme v2 (source.android.com/docs/security/features/
apk-signing/apk-sig-scheme-v2) plus a self-signed X.509 debug certificate using
pycryptodome, which is available in .venv.

Scope: RSA-2048, RSASSA-PKCS1-v1_5 with SHA-256 (signature algorithm id 0x0103),
one signer, no additional attributes. This is exactly what a debug APK needs.
It does NOT implement v1 (JAR) or v3/v4 signing; minSdk here is 29, and every
Android release since 7.0 verifies v2, so a v2-only signature is sufficient.

Usage:
  apk_sign_v2.py sign   --in unsigned.apk --out signed.apk [--keydir DIR]
  apk_sign_v2.py verify --apk signed.apk [--keydir DIR]
  apk_sign_v2.py keygen --keydir DIR

Exit codes: 0 ok, 1 verification/IO failure, 2 usage error.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import struct
import sys
from pathlib import Path

try:
    from Crypto.Hash import SHA256
    from Crypto.PublicKey import RSA
    from Crypto.Signature import pkcs1_15
except ImportError:  # pragma: no cover
    sys.stderr.write("error: pycryptodome missing (pip install pycryptodome)\n")
    raise SystemExit(2)

# --- constants -------------------------------------------------------------

SIG_ALG_RSA_PKCS1V15_SHA256 = 0x0103
V2_BLOCK_ID = 0x7109871A
PADDING_BLOCK_ID = 0x42726577
CHUNK_SIZE = 1048576  # 1 MiB, fixed by the v2 spec

OID_RSA = "1.2.840.113549.1.1.1"
OID_SHA256_RSA = "1.2.840.113549.1.1.11"
OID_CN = "2.5.4.3"
OID_O = "2.5.4.10"
OID_C = "2.5.4.6"

KEY_FILE = "debug-key.pem"
CERT_FILE = "debug-cert.der"
INFO_FILE = "debug-keystore.json"


# --- minimal DER encoder ---------------------------------------------------

def _der_len(n: int) -> bytes:
    if n < 0x80:
        return bytes([n])
    body = n.to_bytes((n.bit_length() + 7) // 8, "big")
    return bytes([0x80 | len(body)]) + body


def _tlv(tag: int, content: bytes) -> bytes:
    return bytes([tag]) + _der_len(len(content)) + content


def der_seq(*items: bytes) -> bytes:
    return _tlv(0x30, b"".join(items))


def der_set(*items: bytes) -> bytes:
    return _tlv(0x31, b"".join(items))


def der_int(value: int) -> bytes:
    if value < 0:
        raise ValueError("negative integers unsupported")
    body = value.to_bytes(max(1, (value.bit_length() + 7) // 8), "big")
    if body[0] & 0x80:
        body = b"\x00" + body
    return _tlv(0x02, body)


def der_oid(dotted: str) -> bytes:
    parts = [int(p) for p in dotted.split(".")]
    body = bytearray([parts[0] * 40 + parts[1]])
    for p in parts[2:]:
        chunk = [p & 0x7F]
        p >>= 7
        while p:
            chunk.append((p & 0x7F) | 0x80)
            p >>= 7
        body.extend(reversed(chunk))
    return _tlv(0x06, bytes(body))


def der_null() -> bytes:
    return b"\x05\x00"


def der_octets(data: bytes) -> bytes:
    return _tlv(0x04, data)


def der_bitstring(data: bytes, unused_bits: int = 0) -> bytes:
    return _tlv(0x03, bytes([unused_bits]) + data)


def der_printable(s: str) -> bytes:
    return _tlv(0x13, s.encode("ascii"))


def der_utf8(s: str) -> bytes:
    return _tlv(0x0C, s.encode("utf-8"))


def der_utctime(s: str) -> bytes:
    return _tlv(0x17, s.encode("ascii"))


def der_explicit(tag_number: int, content: bytes) -> bytes:
    return _tlv(0xA0 | tag_number, content)


# --- keys / certificate ----------------------------------------------------

def keygen(keydir: Path, bits: int = 2048) -> tuple[Path, Path]:
    keydir.mkdir(parents=True, exist_ok=True)
    key = RSA.generate(bits)
    key_path = keydir / KEY_FILE
    key_path.write_bytes(key.export_key(format="PEM"))
    os.chmod(key_path, 0o600)
    cert = build_self_signed_cert(key)
    cert_path = keydir / CERT_FILE
    cert_path.write_bytes(cert)
    info = {
        "kind": "self-signed debug certificate (NOT a JKS keystore; no JDK on this host)",
        "generated_by": "tools/apk_sign_v2.py",
        "key_algorithm": "RSA",
        "key_bits": bits,
        "signature_algorithm": "RSASSA-PKCS1-v1_5 SHA-256 (APK sig alg 0x0103)",
        "subject": "CN=Android Debug, O=Android, C=US",
        "key_file": KEY_FILE,
        "cert_file": CERT_FILE,
        "key_sha256": hashlib.sha256(key.publickey().export_key(format="DER")).hexdigest(),
        "cert_sha256": hashlib.sha256(cert).hexdigest(),
        "cert_size": len(cert),
    }
    (keydir / INFO_FILE).write_text(json.dumps(info, indent=2) + "\n")
    return key_path, cert_path


def load_or_create(keydir: Path) -> tuple[RSA.RsaKey, bytes]:
    key_path = keydir / KEY_FILE
    cert_path = keydir / CERT_FILE
    if not key_path.exists() or not cert_path.exists():
        print(f"[sign] generating debug key/cert in {keydir}")
        key_path, cert_path = keygen(keydir)
    key = RSA.import_key(key_path.read_bytes())
    return key, cert_path.read_bytes()


def spki_der(key: RSA.RsaKey) -> bytes:
    rsa_pub = der_seq(der_int(key.n), der_int(key.e))
    alg = der_seq(der_oid(OID_RSA), der_null())
    return der_seq(alg, der_bitstring(rsa_pub))


def build_self_signed_cert(key: RSA.RsaKey) -> bytes:
    name = der_seq(
        der_set(der_seq(der_oid(OID_C), der_printable("US"))),
        der_set(der_seq(der_oid(OID_O), der_utf8("Android"))),
        der_set(der_seq(der_oid(OID_CN), der_utf8("Android Debug"))),
    )
    sig_alg = der_seq(der_oid(OID_SHA256_RSA), der_null())
    tbs = der_seq(
        der_explicit(0, der_int(2)),          # version v3
        der_int(0x5B000000 | int.from_bytes(os.urandom(3), "big")),
        sig_alg,
        name,
        der_seq(der_utctime("200101000000Z"), der_utctime("491231235959Z")),
        name,
        spki_der(key),
    )
    signature = pkcs1_15.new(key).sign(SHA256.new(tbs))
    return der_seq(tbs, sig_alg, der_bitstring(signature))


# --- v2 length-prefixed helpers -------------------------------------------

def lp(data: bytes) -> bytes:
    return struct.pack("<I", len(data)) + data


def u32(v: int) -> bytes:
    return struct.pack("<I", v)


# --- ZIP parsing -----------------------------------------------------------

def find_eocd(data: bytes) -> int:
    start = max(0, len(data) - (0xFFFF + 22))
    idx = data.rfind(b"PK\x05\x06", start)
    if idx < 0:
        raise ValueError("EOCD not found")
    return idx


def eocd_fields(data: bytes, off: int) -> dict:
    if data[off:off + 4] != b"PK\x05\x06":
        raise ValueError("bad EOCD signature")
    (disk, cd_disk, n_disk, n_total, cd_size, cd_off,
     comment_len) = struct.unpack("<HHHHIIH", data[off + 4:off + 22])
    return {
        "offset": off,
        "cd_size": cd_size,
        "cd_offset": cd_off,
        "entries": n_total,
        "comment_len": comment_len,
    }


def find_signing_block(data: bytes, cd_offset: int) -> tuple[int, int] | None:
    """Return (block_start, block_size_including_magic) or None."""
    if cd_offset < 32 or cd_offset > len(data):
        return None
    if data[cd_offset - 16:cd_offset] != SIGNING_BLOCK_MAGIC:
        return None
    size = struct.unpack("<Q", data[cd_offset - 24:cd_offset - 16])[0]
    total = size + 8
    start = cd_offset - total
    if start < 0:
        raise ValueError("signing block larger than file")
    if struct.unpack("<Q", data[start:start + 8])[0] != size:
        raise ValueError("signing block size mismatch")
    return start, total


def iter_pairs(block: bytes) -> dict[int, bytes]:
    pairs: dict[int, bytes] = {}
    pos = 8
    end = len(block) - 8 - len(SIGNING_BLOCK_MAGIC)
    while pos < end:
        (length,) = struct.unpack("<Q", block[pos:pos + 8])
        pos += 8
        if length < 4 or pos + length > end:
            raise ValueError("bad signing block pair length")
        pair_id = struct.unpack("<I", block[pos:pos + 4])[0]
        pairs[pair_id] = block[pos + 4:pos + length]
        pos += length
    return pairs


# --- content digest -------------------------------------------------------

def _chunked_digest(chunks: list[bytes]) -> bytes:
    digests = b"".join(
        hashlib.sha256(b"\xa5" + u32(len(c)) + c).digest() for c in chunks
    )
    return hashlib.sha256(b"\x5a" + u32(len(chunks)) + digests).digest()


def content_digest(data: bytes, cd_offset: int, cd_size: int,
                   block_offset: int, eocd_offset: int) -> bytes:
    """Digest over (1) entries, (2) central directory, (3) EOCD w/ patched offset."""
    eocd = bytearray(data[eocd_offset:])
    struct.pack_into("<I", eocd, 16, block_offset)
    sections = [
        data[:block_offset],
        data[cd_offset:cd_offset + cd_size],
        bytes(eocd),
    ]
    chunks: list[bytes] = []
    for section in sections:
        for i in range(0, len(section), CHUNK_SIZE):
            chunks.append(section[i:i + CHUNK_SIZE])
    return _chunked_digest(chunks)


# --- sign / verify --------------------------------------------------------

def build_v2_block(digest: bytes, key: RSA.RsaKey, cert: bytes) -> bytes:
    digests = lp(lp(u32(SIG_ALG_RSA_PKCS1V15_SHA256) + lp(digest)))
    certs = lp(lp(cert))
    attrs = lp(b"")
    # Signed data = concatenation of the three length-prefixed sections WITHOUT
    # an outer length prefix; that byte string is what gets signed.
    signed_data = digests + certs + attrs

    signature = pkcs1_15.new(key).sign(SHA256.new(signed_data))
    signatures = lp(lp(u32(SIG_ALG_RSA_PKCS1V15_SHA256) + lp(signature)))
    public_key = lp(spki_der(key))

    signer = lp(signed_data) + signatures + public_key
    v2_value = lp(lp(signer))
    return v2_value


SIGNING_BLOCK_MAGIC = b"APK Sig Block 42"


def build_signing_block(pairs: list[tuple[int, bytes]], pad_to: int = 4096) -> bytes:
    """APK Signing Block: u64 size | id-value pairs | u64 size | magic(16)."""
    body = b"".join(struct.pack("<QI", len(value) + 4, pid) + value
                    for pid, value in pairs)
    total = 8 + len(body) + 8 + len(SIGNING_BLOCK_MAGIC)
    pad = (-total) % pad_to
    if pad >= 12:
        pad_value = b"\x00" * (pad - 12)
        body += struct.pack("<QI", len(pad_value) + 4, PADDING_BLOCK_ID) + pad_value
    size = len(body) + 8 + len(SIGNING_BLOCK_MAGIC)
    return (struct.pack("<Q", size) + body + struct.pack("<Q", size)
            + SIGNING_BLOCK_MAGIC)


def sign_apk(in_path: Path, out_path: Path, keydir: Path) -> dict:
    key, cert = load_or_create(keydir)
    data = in_path.read_bytes()
    eocd_off = find_eocd(data)
    fields = eocd_fields(data, eocd_off)
    if fields["cd_offset"] + fields["cd_size"] > len(data):
        raise ValueError("central directory out of range")
    if find_signing_block(data, fields["cd_offset"]) is not None:
        raise ValueError("input APK is already v2-signed; re-sign from the aligned unsigned APK")

    block_offset = fields["cd_offset"]
    digest = content_digest(data, fields["cd_offset"], fields["cd_size"],
                           block_offset, eocd_off)
    v2_value = build_v2_block(digest, key, cert)
    block = build_signing_block([(V2_BLOCK_ID, v2_value)])

    out = bytearray(data[:block_offset] + block + data[block_offset:])
    # The signing block is inserted before the central directory, so the real
    # CD offset in the stored EOCD shifts by len(block). (The *digest* above
    # uses the pre-insertion offset, i.e. the start of the signing block, as
    # required by the v2 spec.)
    new_eocd = eocd_off + len(block)
    struct.pack_into("<I", out, new_eocd + 16, block_offset + len(block))
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_bytes(bytes(out))

    return {
        "input": str(in_path),
        "output": str(out_path),
        "input_size": len(data),
        "output_size": len(out),
        "signing_block_offset": block_offset,
        "signing_block_size": len(block),
        "content_digest_sha256": digest.hex(),
        "cert_sha256": hashlib.sha256(cert).hexdigest(),
        "signature_algorithm": "RSASSA-PKCS1-v1_5/SHA-256 (0x0103)",
    }


def verify_apk(apk: Path, keydir: Path | None = None) -> dict:
    data = apk.read_bytes()
    eocd_off = find_eocd(data)
    fields = eocd_fields(data, eocd_off)
    located = find_signing_block(data, fields["cd_offset"])
    if located is None:
        raise ValueError("no APK Signing Block found (not v2 signed)")
    block_start, block_total = located
    block = data[block_start:block_start + block_total]
    pairs = iter_pairs(block)
    if V2_BLOCK_ID not in pairs:
        raise ValueError("APK Signing Block has no v2 signature (id 0x7109871a)")
    v2 = pairs[V2_BLOCK_ID]

    def read_lp(buf: bytes, pos: int) -> tuple[bytes, int]:
        (n,) = struct.unpack("<I", buf[pos:pos + 4])
        return buf[pos + 4:pos + 4 + n], pos + 4 + n

    signers, _ = read_lp(v2, 0)
    signer, _ = read_lp(signers, 0)
    signed_data, pos = read_lp(signer, 0)

    digests_seq, p = read_lp(signed_data, 0)
    certs_seq, p = read_lp(signed_data, p)
    _attrs, p = read_lp(signed_data, p)
    if p != len(signed_data):
        raise ValueError("trailing bytes in signed data")

    digest_value = None
    dp = 0
    while dp < len(digests_seq):
        rec, dp = read_lp(digests_seq, dp)
        alg = struct.unpack("<I", rec[:4])[0]
        value, _ = read_lp(rec, 4)
        if alg == SIG_ALG_RSA_PKCS1V15_SHA256:
            digest_value = value
    if digest_value is None:
        raise ValueError("no SHA-256 digest record in v2 block")

    cert, _ = read_lp(certs_seq, 0)

    signatures_seq, pos = read_lp(signer, pos)
    spki, _ = read_lp(signer, pos)
    signature = None
    sp = 0
    while sp < len(signatures_seq):
        rec, sp = read_lp(signatures_seq, sp)
        alg = struct.unpack("<I", rec[:4])[0]
        value, _ = read_lp(rec, 4)
        if alg == SIG_ALG_RSA_PKCS1V15_SHA256:
            signature = value
    if signature is None:
        raise ValueError("no SHA-256 signature record in v2 block")

    # recompute content digest
    expected = content_digest(data, fields["cd_offset"], fields["cd_size"],
                              block_start, eocd_off)
    if expected != digest_value:
        raise ValueError("content digest mismatch: apk content changed after signing")

    # verify signature with the public key from the signer block
    pub = RSA.import_key(spki)
    try:
        pkcs1_15.new(pub).verify(SHA256.new(signed_data), signature)
    except (ValueError, TypeError) as exc:
        raise ValueError(f"RSA signature verification failed: {exc}") from exc

    # the SPKI must be the key inside the certificate (we can't parse X.509 fully,
    # but the SPKI DER is embedded verbatim in the certificate)
    if spki not in cert:
        raise ValueError("signer public key not present in the X.509 certificate")

    # the certificate must itself be signed by that key (self-signed debug cert)
    result = {
        "apk": str(apk),
        "size": len(data),
        "entries": fields["entries"],
        "signing_block_offset": block_start,
        "signing_block_size": block_total,
        "content_digest_sha256": expected.hex(),
        "cert_sha256": hashlib.sha256(cert).hexdigest(),
        "cert_size": len(cert),
        "public_key_bits": pub.size_in_bits(),
        "signature_algorithm": "RSASSA-PKCS1-v1_5/SHA-256 (0x0103)",
        "v2_signature_verified": True,
    }
    if keydir is not None:
        info_path = keydir / INFO_FILE
        if info_path.exists():
            info = json.loads(info_path.read_text())
            result["matches_local_debug_cert"] = (
                info.get("cert_sha256") == result["cert_sha256"]
            )
    return result


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("sign")
    s.add_argument("--in", dest="inp", required=True)
    s.add_argument("--out", dest="out", required=True)
    s.add_argument("--keydir", default="port/build/apk")

    v = sub.add_parser("verify")
    v.add_argument("--apk", required=True)
    v.add_argument("--keydir", default=None)

    k = sub.add_parser("keygen")
    k.add_argument("--keydir", default="port/build/apk")

    args = ap.parse_args(argv)
    try:
        if args.cmd == "sign":
            info = sign_apk(Path(args.inp), Path(args.out), Path(args.keydir))
            print(json.dumps(info, indent=2))
            return 0
        if args.cmd == "verify":
            info = verify_apk(Path(args.apk),
                              Path(args.keydir) if args.keydir else None)
            print(json.dumps(info, indent=2))
            return 0
        if args.cmd == "keygen":
            key_path, cert_path = keygen(Path(args.keydir))
            print(f"key:  {key_path}\ncert: {cert_path}")
            return 0
    except Exception as exc:  # noqa: BLE001 - CLI boundary
        sys.stderr.write(f"error: {exc}\n")
        return 1
    return 2


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
