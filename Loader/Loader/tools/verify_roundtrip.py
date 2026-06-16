#!/usr/bin/env python3
"""
verify_roundtrip.py - sanity check that the runtime C++ decrypt path
would produce identical plaintext to the original input files.

This script re-implements the C++ decrypt pipeline in Python and compares
the output against the original cheat.dll / driver.sys / kdmapper.exe.
"""

import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import encrypt_blob as eb

from cryptography.hazmat.primitives.ciphers.aead import AESGCM
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms
from cryptography.hazmat.backends import default_backend


def decrypt_3layer(blob: bytes) -> bytes:
    assert blob[:4] == b"RHv3", "bad magic"
    iv_a    = blob[4:16]
    tag_a   = blob[16:32]
    nonce_b = blob[32:44]
    salt_c  = blob[44:48]
    orig_ln = struct.unpack("<I", blob[48:52])[0]
    ct_a    = blob[52:]

    aes = AESGCM(eb.outer_a_key())
    mid_b = aes.decrypt(iv_a, ct_a + tag_a, associated_data=None)

    algo = algorithms.ChaCha20(eb.outer_b_key(), b"\x00\x00\x00\x00" + nonce_b)
    cipher = Cipher(algo, mode=None, backend=default_backend())
    dec = cipher.decryptor()
    mid_c = dec.update(mid_b) + dec.finalize()

    salt_int = int.from_bytes(salt_c, "little")
    seed = (eb.seed_c_base() ^ salt_int) & 0xFFFFFFFF
    plain = eb.xor_stream(mid_c, seed)

    return plain[:orig_ln]


def unwrap_cheat(inner: bytes) -> bytes:
    iv  = inner[:12]
    tag = inner[12:28]
    ct  = inner[28:]
    key = eb.session_key()
    return AESGCM(key).decrypt(iv, ct + tag, associated_data=None)


def check(label: str, blob_path: str, orig_path: str, *, session_unwrap: bool):
    with open(blob_path, "rb") as f:
        blob = f.read()
    with open(orig_path, "rb") as f:
        orig = f.read()
    plain = decrypt_3layer(blob)
    if session_unwrap:
        plain = unwrap_cheat(plain)
    if plain == orig:
        print(f"[ok] {label}: roundtrip OK ({len(orig)} B)")
    else:
        print(f"[FAIL] {label}: mismatch (orig={len(orig)} plain={len(plain)})")
        if len(plain) != len(orig):
            sys.exit(4)
        diff = sum(1 for a, b in zip(plain, orig) if a != b)
        print(f"       {diff} bytes differ")
        sys.exit(5)


if __name__ == "__main__":
    root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
    rc   = os.path.join(root, "Loader", "rc")
    res  = os.path.join(root, "Loader", "Loader", "resources")

    check("cheat",  os.path.join(res, "alpha.bin"), os.path.join(rc, "cheat.dll"),
          session_unwrap=True)
    check("driver", os.path.join(res, "beta.bin"),  os.path.join(rc, "RainHack.sys"),
          session_unwrap=False)
    check("kdm",    os.path.join(res, "gamma.bin"), os.path.join(rc, "kdmapper.exe"),
          session_unwrap=False)
