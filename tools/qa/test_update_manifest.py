import importlib.util
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]


def load(name):
    spec = importlib.util.spec_from_file_location(name.replace("-", "_"), ROOT / "scripts" / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


manifest_script = load("make-update-manifest")
key_script = load("update-key-header")
verify_script = load("verify-update-signature")


class ManifestTests(unittest.TestCase):
    def setUp(self):
        self.folder = pathlib.Path(tempfile.mkdtemp())
        self.windows = self.folder / "OBS-Jukebox-1.2.5-Windows-Setup.exe"
        self.mac = self.folder / "OBS-Jukebox-1.2.5-macOS-arm64.zip"
        self.windows.write_bytes(b"windows")
        self.mac.write_bytes(b"mac!")

    def test_canonical_bytes(self):
        first = manifest_script.manifest(self.windows, self.mac, "v1.2.5", "v1.2.5")
        self.assertEqual(first, manifest_script.manifest(self.windows, self.mac, "v1.2.5", "v1.2.5"))
        self.assertEqual(
            first,
            b'{"files":{"macos-arm64":{"sha256":"'
            + __import__("hashlib").sha256(b"mac!").hexdigest().encode()
            + b'","size":4},"windows":{"sha256":"'
            + __import__("hashlib").sha256(b"windows").hexdigest().encode()
            + b'","size":7}},"schema":1,"version":"1.2.5"}\n',
        )

    def test_refuses_disagreeing_versions(self):
        for mod_version, tag in (("v1.2.4", "v1.2.5"), ("v1.2.5", "v1.2.6")):
            with self.assertRaises(SystemExit):
                manifest_script.manifest(self.windows, self.mac, mod_version, tag)
        other = self.folder / "OBS-Jukebox-1.2.6-macOS-arm64.zip"
        other.write_bytes(b"x")
        with self.assertRaises(SystemExit):
            manifest_script.manifest(self.windows, other, "v1.2.5", "v1.2.5")

    def test_refuses_unexpected_names(self):
        wrong = self.folder / "setup.exe"
        wrong.write_bytes(b"x")
        with self.assertRaises(SystemExit):
            manifest_script.manifest(wrong, self.mac, "v1.2.5", "v1.2.5")


class KeyTests(unittest.TestCase):
    def test_rotation_and_empty_trust(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = pathlib.Path(directory)
            message = folder / "update.json"
            message.write_bytes(b"release manifest\n")
            public = []
            signatures = []
            for name in ("old", "new", "untrusted"):
                private, pem, sig = (folder / (name + suffix) for suffix in (".key", ".pem", ".sig"))
                subprocess.run(["openssl", "genpkey", "-algorithm", "EC", "-pkeyopt", "ec_paramgen_curve:P-256", "-out", str(private)], check=True, capture_output=True)
                subprocess.run(["openssl", "pkey", "-in", str(private), "-pubout", "-out", str(pem)], check=True, capture_output=True)
                subprocess.run(["openssl", "dgst", "-sha256", "-sign", str(private), "-out", str(sig), str(message)], check=True)
                public.append(pem.read_text())
                signatures.append(sig)
            bundle = folder / "bundle.pem"
            bundle.write_text(public[0] + public[1])
            self.assertEqual(len(key_script.public_keys(bundle.read_text())), 2)
            self.assertIn(">, 2> UPDATE_PUBLIC_KEYS", key_script.key_header(bundle.read_text()))
            self.assertTrue(verify_script.verify(bundle, signatures[0], message))
            self.assertTrue(verify_script.verify(bundle, signatures[1], message))
            self.assertFalse(verify_script.verify(bundle, signatures[2], message))
            message.write_bytes(b"tampered manifest\n")
            self.assertFalse(verify_script.verify(bundle, signatures[0], message))
            bundle.write_text("# Not provisioned\n")
            self.assertFalse(verify_script.verify(bundle, signatures[0], message))
            with self.assertRaises(ValueError):
                key_script.public_keys("-----BEGIN PRIVATE KEY-----\nAAAA\n-----END PRIVATE KEY-----")

    def test_header_matches_committed_key(self):
        pem = (ROOT / "scripts" / "update-signing-public.pem").read_text()
        self.assertEqual(key_script.key_header(pem), (ROOT / "src" / "UpdateKey.hpp").read_text())


if __name__ == "__main__":
    unittest.main()
