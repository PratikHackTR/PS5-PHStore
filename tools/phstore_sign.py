#!/usr/bin/env python3
"""Offline Ed25519 signing for PH Store's exact-byte detached signatures."""
import argparse
import os
from pathlib import Path

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey, Ed25519PublicKey


def write_new(path: Path, data: bytes, mode: int) -> None:
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, mode)
    with os.fdopen(fd, "wb") as output:
        output.write(data)


def main() -> int:
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="command", required=True)
    gen = sub.add_parser("keygen")
    gen.add_argument("private_key", type=Path)
    gen.add_argument("public_key", type=Path)
    sign = sub.add_parser("sign")
    sign.add_argument("private_key", type=Path)
    sign.add_argument("document", type=Path)
    sign.add_argument("signature", type=Path)
    verify = sub.add_parser("verify")
    verify.add_argument("public_key", type=Path)
    verify.add_argument("document", type=Path)
    verify.add_argument("signature", type=Path)
    args = parser.parse_args()

    if args.command == "keygen":
        if args.private_key.exists() or args.public_key.exists():
            parser.error("refusing to overwrite existing key files")
        private = Ed25519PrivateKey.generate()
        write_new(args.private_key, private.private_bytes(
            serialization.Encoding.Raw, serialization.PrivateFormat.Raw,
            serialization.NoEncryption()), 0o600)
        write_new(args.public_key, private.public_key().public_bytes(
            serialization.Encoding.Raw, serialization.PublicFormat.Raw), 0o644)
        return 0
    if args.command == "sign":
        key = Ed25519PrivateKey.from_private_bytes(args.private_key.read_bytes())
        args.signature.write_bytes(key.sign(args.document.read_bytes()))
        return 0
    key = Ed25519PublicKey.from_public_bytes(args.public_key.read_bytes())
    try:
        key.verify(args.signature.read_bytes(), args.document.read_bytes())
    except InvalidSignature:
        print("signature invalid", file=__import__("sys").stderr)
        return 1
    print("signature valid")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
