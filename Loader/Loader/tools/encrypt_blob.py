#!/usr/bin/env python3
"""
encrypt_blob.py - 3-layer encryption for Loader embedded resources.

Layers (inner-to-outer applied during encrypt):
  C. XOR-stream (Xorshift32, seed = base_seed XOR salt_C)
  B. ChaCha20  (key Kb, nonce random 12)
  A. AES-256-GCM (key Ka, iv random 12, tag 16)

Output blob layout:
  magic "RHv3"(4) | iv_A(12) | tag_A(16) | nonce_B(12) | salt_C(4 LE) | origLen(4 LE) | ct_A

Cheat.dll additionally gets wrapped in a driver-format inner shell before
the 3-layer outer encryption so that the kernel driver, which only knows
the session key, can decrypt its payload:
  driver_inner = iv_S(12) | tag_S(16) | AES-GCM(sessionKey, iv_S)(cheat.dll)
  then drier_inner is fed into the 3-layer encryption as input.

Constants MUST stay in sync with Loader/Loader/src/Resources.cpp &
Loader/Loader/src/ApiResolve.cpp (for SeedC()).

Usage:
    python encrypt_blob.py --cheat <path> --driver <path> --kdm <path> --out-dir <dir>
"""

import argparse
import os
import secrets
import struct
import sys

try:
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
    from cryptography.hazmat.backends import default_backend
except ImportError:
    print("[ERR] pip install cryptography", file=sys.stderr)
    sys.exit(2)

SESS_KA = bytes([0x12, 0xA4, 0x77, 0xC9, 0x3E, 0xB1, 0x58, 0x6F])
SESS_KB = bytes([0x83, 0x55, 0xE0, 0x29, 0xDD, 0x47, 0x91, 0x3A])
SESS_KC = bytes([0x6E, 0xCB, 0x14, 0x77, 0xA8, 0x02, 0x59, 0xBC])
SESS_KD = bytes([0xF1, 0x36, 0x8B, 0x4D, 0x07, 0x90, 0xE3, 0x52])
SESS_MA = bytes([0x68, 0x9B, 0xC3, 0x0B, 0xDF, 0xB8, 0x05, 0xE7])
SESS_MB = bytes([0xCC, 0xF7, 0x37, 0x42, 0x0C, 0x76, 0x80, 0x22])
SESS_MC = bytes([0x0E, 0x69, 0xC3, 0x1C, 0x62, 0x71, 0x57, 0xE5])
SESS_MD = bytes([0x5D, 0x33, 0x7A, 0x06, 0xC6, 0xE3, 0x12, 0x65])

OUTER_Aa = bytes([0x3F, 0x9C, 0x14, 0x77, 0xA2, 0x0B, 0xDE, 0x51])
OUTER_Ab = bytes([0xC7, 0x55, 0xE0, 0x29, 0xDD, 0x47, 0x91, 0x3A])
OUTER_Ac = bytes([0x6E, 0xCB, 0x14, 0x77, 0xA8, 0x02, 0x59, 0xBC])
OUTER_Ad = bytes([0xF1, 0x36, 0x8B, 0x4D, 0x07, 0x90, 0xE3, 0x52])
OUTER_MA = bytes([0x11, 0x22, 0x44, 0x66, 0x88, 0xAA, 0xCC, 0xEE])
OUTER_MB = bytes([0xFE, 0xDC, 0xBA, 0x98, 0x76, 0x54, 0x32, 0x10])
OUTER_MC = bytes([0x5A, 0xA5, 0x3C, 0xC3, 0x69, 0x96, 0x71, 0x8E])
OUTER_MD = bytes([0xB3, 0x4D, 0x72, 0x61, 0x55, 0xEE, 0xF0, 0x11])


def session_key() -> bytes:
    p0 = bytes(a ^ b for a, b in zip(SESS_KA, SESS_MA))
    p1 = bytes(a ^ b for a, b in zip(SESS_KB, SESS_MB))
    p2 = bytes(a ^ b for a, b in zip(SESS_KC, SESS_MC))
    p3 = bytes(a ^ b for a, b in zip(SESS_KD, SESS_MD))
    k = p0 + p1 + p2 + p3
    assert len(k) == 32
    return k


def outer_a_key() -> bytes:
    p0 = bytes(a ^ b for a, b in zip(OUTER_Aa, OUTER_MA))
    p1 = bytes(a ^ b for a, b in zip(OUTER_Ab, OUTER_MB))
    p2 = bytes(a ^ b for a, b in zip(OUTER_Ac, OUTER_MC))
    p3 = bytes(a ^ b for a, b in zip(OUTER_Ad, OUTER_MD))
    return p0 + p1 + p2 + p3


def outer_b_key() -> bytes:
    k = bytearray(32)
    k[ 0] = (0x17 ^ 0x40) & 0xFF
    k[ 1] = (0xB2 + 0x11) & 0xFF
    k[ 2] = (0xC3 ^ 0x2A) & 0xFF
    k[ 3] = (((0x5F << 1) | (0x5F >> 7)) & 0xFF)
    k[ 4] = (0x7E ^ 0x19) & 0xFF
    k[ 5] = (0x88 + 0x07) & 0xFF
    k[ 6] = (0x91 ^ 0xAA) & 0xFF
    k[ 7] = (0x0F ^ 0xF0) & 0xFF
    k[ 8] = (0xA1 ^ 0x1A) & 0xFF
    k[ 9] = (0x22 + 0x33) & 0xFF
    k[10] = (0xBB ^ 0x44) & 0xFF
    k[11] = (0xCC ^ 0xDD) & 0xFF
    k[12] = (0x12 ^ 0x77) & 0xFF
    k[13] = (0x84 + 0x05) & 0xFF
    k[14] = (0x65 ^ 0x89) & 0xFF
    k[15] = (0xEF ^ 0x10) & 0xFF
    k[16] = (0x3C ^ 0x51) & 0xFF
    k[17] = (0xD4 + 0x09) & 0xFF
    k[18] = (0x79 ^ 0xB2) & 0xFF
    k[19] = (0x46 ^ 0x2A) & 0xFF
    k[20] = (0x8D ^ 0x71) & 0xFF
    k[21] = (0x1F + 0x33) & 0xFF
    k[22] = (0xE7 ^ 0xC5) & 0xFF
    k[23] = (0x58 ^ 0x39) & 0xFF
    k[24] = (0x62 ^ 0xB8) & 0xFF
    k[25] = (0xAD + 0x0C) & 0xFF
    k[26] = (0x30 ^ 0x4F) & 0xFF
    k[27] = (0xF2 ^ 0x8E) & 0xFF
    k[28] = (0x09 ^ 0x73) & 0xFF
    k[29] = (0xC4 + 0x1A) & 0xFF
    k[30] = (0x5E ^ 0x27) & 0xFF
    k[31] = (0xB1 ^ 0x0D) & 0xFF
    return bytes(k)


def ror13(v: int) -> int:
    v &= 0xFFFFFFFF
    return ((v >> 13) | (v << (32 - 13))) & 0xFFFFFFFF


def hash_ror13(s: str) -> int:
    h = 0
    for ch in s.encode("ascii"):
        h = ror13(h)
        h = (h + ch) & 0xFFFFFFFF
    return h


def seed_c_base() -> int:
    a = hash_ror13("NtProtectVirtualMemory")
    b = hash_ror13("RtlImageNtHeader")
    c = hash_ror13("NtQueryInformationProcess")
    d = hash_ror13("RtlDecodePointer")
    s = (a ^ ((b + 0x9E3779B9) & 0xFFFFFFFF)) & 0xFFFFFFFF
    s ^= (((c << 7) & 0xFFFFFFFF) | (c >> 25)) & 0xFFFFFFFF
    s ^= (d ^ 0xC0DEC0DE) & 0xFFFFFFFF
    return s & 0xFFFFFFFF


def xor_stream(buf: bytes, seed: int) -> bytes:
    s = seed & 0xFFFFFFFF
    if s == 0:
        s = 0xDEADBEEF
    out = bytearray(len(buf))
    for i in range(len(buf)):
        s ^= (s << 13) & 0xFFFFFFFF
        s &= 0xFFFFFFFF
        s ^= (s >> 17) & 0xFFFFFFFF
        s ^= (s << 5)  & 0xFFFFFFFF
        s &= 0xFFFFFFFF
        out[i] = buf[i] ^ (s & 0xFF)
    return bytes(out)


def chacha20_xcrypt(key: bytes, nonce: bytes, data: bytes) -> bytes:
    algo = algorithms.ChaCha20(key, b"\x00\x00\x00\x00" + nonce)
    cipher = Cipher(algo, mode=None, backend=default_backend())
    enc = cipher.encryptor()
    return enc.update(data) + enc.finalize()


def encrypt_3layer(plain: bytes) -> bytes:
    salt_c   = secrets.token_bytes(4)
    salt_int = int.from_bytes(salt_c, "little")
    seed     = (seed_c_base() ^ salt_int) & 0xFFFFFFFF

    mid_c = xor_stream(plain, seed)

    nonce_b = secrets.token_bytes(12)
    mid_b   = chacha20_xcrypt(outer_b_key(), nonce_b, mid_c)

    iv_a    = secrets.token_bytes(12)
    aes     = AESGCM(outer_a_key())
    ct_tag  = aes.encrypt(iv_a, mid_b, associated_data=None)
    ct_a    = ct_tag[:-16]
    tag_a   = ct_tag[-16:]

    orig_len = struct.pack("<I", len(plain))
    header   = b"RHv3" + iv_a + tag_a + nonce_b + salt_c + orig_len
    return header + ct_a


def wrap_cheat_session(cheat_bytes: bytes) -> bytes:
    key = session_key()
    iv  = secrets.token_bytes(12)
    ct_with_tag = AESGCM(key).encrypt(iv, cheat_bytes, associated_data=None)
    ct  = ct_with_tag[:-16]
    tag = ct_with_tag[-16:]
    return iv + tag + ct


def enc_file(src_path: str, dst_path: str, *, session_wrap: bool):
    with open(src_path, "rb") as f:
        raw = f.read()
    inner = wrap_cheat_session(raw) if session_wrap else raw
    blob = encrypt_3layer(inner)
    os.makedirs(os.path.dirname(os.path.abspath(dst_path)) or ".", exist_ok=True)
    with open(dst_path, "wb") as f:
        f.write(blob)
    kind = "cheat(+session)" if session_wrap else "raw"
    print(f"[ok] {src_path} ({len(raw)} B, {kind}) -> {dst_path} ({len(blob)} B)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cheat",   required=True, help="path to cheat.dll")
    ap.add_argument("--driver",  required=True, help="path to driver .sys")
    ap.add_argument("--kdm",     required=True, help="path to kdmapper.exe")
    ap.add_argument("--out-dir", required=True, help="output dir (resources/)")
    args = ap.parse_args()

    for label, p in (("cheat", args.cheat), ("driver", args.driver), ("kdm", args.kdm)):
        if not os.path.isfile(p):
            print(f"[ERR] {label} not found: {p}", file=sys.stderr)
            sys.exit(3)

    os.makedirs(args.out_dir, exist_ok=True)

    enc_file(args.cheat,  os.path.join(args.out_dir, "alpha.bin"), session_wrap=True)
    enc_file(args.driver, os.path.join(args.out_dir, "beta.bin"),  session_wrap=False)
    enc_file(args.kdm,    os.path.join(args.out_dir, "gamma.bin"), session_wrap=False)


if __name__ == "__main__":
    main()
