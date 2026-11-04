#!/usr/bin/env python3
import importlib.util
import pathlib
import subprocess
import sys
import tempfile

spec = importlib.util.spec_from_file_location("update_keys", pathlib.Path(__file__).with_name("update-key-header.py"))
keys = importlib.util.module_from_spec(spec)
spec.loader.exec_module(keys)


def verify(bundle, signature, manifest):
    with tempfile.TemporaryDirectory() as directory:
        public = pathlib.Path(directory) / "public.pem"
        for pem, _ in keys.public_keys(pathlib.Path(bundle).read_text()):
            public.write_text(pem)
            result = subprocess.run(["openssl", "dgst", "-sha256", "-verify", str(public),
                                     "-signature", str(signature), str(manifest)], capture_output=True)
            if result.returncode == 0:
                return True
    return False


if __name__ == "__main__":
    if len(sys.argv) != 4:
        raise SystemExit("usage: verify-update-signature.py public-keys.pem update.json.sig update.json")
    if not verify(*sys.argv[1:]):
        raise SystemExit("Signature not trusted: provision the maintainer's public key or check the signing secret")
