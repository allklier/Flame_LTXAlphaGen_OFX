"""Setup's model step with an Existing Models folder: copy (decision 51) or link (decision 66)."""

from __future__ import annotations

import hashlib
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "helper"))

import setup_helper  # noqa: E402

DATA = b"model weights" * 1000


def manifest(sha: str) -> dict:
    return {"files": [{"path": "loras/m.safetensors", "size": len(DATA), "sha256": sha, "repo": "r",
                       "revision": "v", "hf_path": "m.safetensors"}]}


class ExistingModelsTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        root = self.tmp.name
        self.home = os.environ.get("HOME")
        os.environ["HOME"] = root  # the helper logs to ~/.cache: keep it out of the real log
        self.existing = os.path.join(root, "existing", "sub")
        os.makedirs(self.existing)
        self.src = os.path.join(self.existing, "m.safetensors")
        with open(self.src, "wb") as f:
            f.write(DATA)
        self.models = os.path.join(root, "models")
        self.dst = os.path.join(self.models, "loras", "m.safetensors")
        self.man = manifest(hashlib.sha256(DATA).hexdigest())
        self.prog = setup_helper.Progress(False)

    def tearDown(self) -> None:
        if self.home is not None:
            os.environ["HOME"] = self.home
        self.tmp.cleanup()

    def gather(self, link: bool, man: dict | None = None) -> dict:
        return setup_helper.gather_models(man or self.man, self.models, os.path.dirname(self.existing), "",
                                          self.prog, link=link)

    def test_link(self):
        s = self.gather(link=True)
        self.assertEqual(len(s["linked"]), 1)
        self.assertTrue(os.path.islink(self.dst))
        self.assertEqual(os.path.realpath(self.dst), os.path.realpath(self.src))
        self.assertTrue(os.path.exists(self.dst + ".verified"))
        again = self.gather(link=True)  # a re-run keeps the verified link
        self.assertEqual(len(again["kept"]), 1)
        self.assertTrue(os.path.islink(self.dst))

    def test_copy(self):
        s = self.gather(link=False)
        self.assertEqual(len(s["copied"]), 1)
        self.assertFalse(os.path.islink(self.dst))
        with open(self.dst, "rb") as f:
            self.assertEqual(f.read(), DATA)

    def test_link_rejects_other_version(self):
        with self.assertRaises(setup_helper.SetupError):  # no token to download instead
            self.gather(link=True, man=manifest("0" * 64))
        self.assertFalse(os.path.lexists(self.dst))

    def test_link_replaces_dangling_link(self):
        os.makedirs(os.path.dirname(self.dst))
        os.symlink(os.path.join(self.tmp.name, "gone.safetensors"), self.dst)
        self.gather(link=True)
        self.assertEqual(os.path.realpath(self.dst), os.path.realpath(self.src))


if __name__ == "__main__":
    unittest.main()
