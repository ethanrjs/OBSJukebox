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

    def make(self, windows=None, mac=None, mod=None, tag="v1.2.5"):
        mod = mod or {"version": "v1.2.5", "gd": {"win": "2.2081", "mac": "2.2081"}}
        return manifest_script.manifest(windows or self.windows, mac or self.mac, mod, tag)

    def test_canonical_bytes(self):
        first = self.make()
        self.assertEqual(first, self.make())
        self.assertEqual(
            first,
            b'{"files":{"macos-arm64":{"sha256":'
            b'"e76e8bb942910fb7a4e884777ed922862101615b7bfefc64b90691f11c60bd16","size":4},'
            b'"windows":{"sha256":'
            b'"340d600392818df2413382dc7d8325c360d83ea49a262d31760348484bbc10b5","size":7}},'
            b'"gd":"2.2081","schema":1,"version":"1.2.5"}\n',
        )

    def test_refuses_disagreeing_versions(self):
        gd = {"win": "2.2081", "mac": "2.2081"}
        for mod_version, tag in (("v1.2.4", "v1.2.5"), ("v1.2.5", "v1.2.6")):
            with self.assertRaises(SystemExit):
                self.make(mod={"version": mod_version, "gd": gd}, tag=tag)
        other = self.folder / "OBS-Jukebox-1.2.6-macOS-arm64.zip"
        other.write_bytes(b"x")
        with self.assertRaises(SystemExit):
            self.make(mac=other)

    def test_refuses_platforms_on_different_gd_versions(self):
        with self.assertRaises(SystemExit):
            self.make(mod={"version": "v1.2.5", "gd": {"win": "2.2081", "mac": "2.2091"}})

    def test_refuses_a_gd_version_the_mod_cannot_read(self):
        for gd in ("*", "٢.2081"):
            with self.assertRaises(SystemExit):
                self.make(mod={"version": "v1.2.5", "gd": {"win": gd, "mac": gd}})

    def test_refuses_unexpected_names(self):
        wrong = self.folder / "setup.exe"
        wrong.write_bytes(b"x")
        with self.assertRaises(SystemExit):
            self.make(windows=wrong)

    def test_refuses_a_version_the_mod_cannot_read(self):
        windows = self.folder / "OBS-Jukebox-١.2.5-Windows-Setup.exe"
        mac = self.folder / "OBS-Jukebox-١.2.5-macOS-arm64.zip"
        windows.write_bytes(b"x")
        mac.write_bytes(b"x")
        mod = {"version": "v١.2.5", "gd": {"win": "2.2081", "mac": "2.2081"}}
        with self.assertRaises(SystemExit):
            self.make(windows=windows, mac=mac, mod=mod, tag="v١.2.5")


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
