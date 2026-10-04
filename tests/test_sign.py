import tempfile
import unittest
from pathlib import Path

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey


class SignatureTests(unittest.TestCase):
    def test_exact_bytes_verify_and_tampering_fails(self):
        key = Ed25519PrivateKey.generate()
        message = b'{"schema_version":1}\n'
        signature = key.sign(message)
        key.public_key().verify(signature, message)
        with self.assertRaises(InvalidSignature):
            key.public_key().verify(signature, message.rstrip())

    def test_cli_sign_verify(self):
        import subprocess
        import sys

        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as tmp:
            private, public = Path(tmp) / "offline.key", Path(tmp) / "verify.pub"
            doc, sig = Path(tmp) / "catalog.json", Path(tmp) / "catalog.json.sig"
            doc.write_bytes(b'{"catalog_version":1}\n')
            script = root / "tools" / "phstore_sign.py"
            subprocess.run([sys.executable, str(script), "keygen", str(private), str(public)], check=True)
            subprocess.run([sys.executable, str(script), "sign", str(private), str(doc), str(sig)], check=True)
            subprocess.run([sys.executable, str(script), "verify", str(public), str(doc), str(sig)], check=True)
            doc.write_bytes(b'{"catalog_version":2}\n')
            bad = subprocess.run([sys.executable, str(script), "verify", str(public), str(doc), str(sig)])
            self.assertNotEqual(bad.returncode, 0)


if __name__ == "__main__":
    unittest.main()
