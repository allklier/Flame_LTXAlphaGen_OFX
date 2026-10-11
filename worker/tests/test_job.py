"""Runs job.py end to end (no UI) on a staged input, as the OFX Process button would."""

import json
import os
import subprocess
import sys
import tempfile
import unittest

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
WORKER_DIR = os.path.join(HERE, "..")
sys.path.insert(0, WORKER_DIR)

from ltxag_worker import cache, transport  # noqa: E402


def stage(job_id, frames):
    n, h, w, _ = frames.shape
    path = f"/dev/shm/ltxag_job_{job_id}_in.bin"
    with open(path, "wb") as f:
        f.write(transport.HEADER.pack(transport.IN_MAGIC, transport.VERSION, w, h, n, 3))
        frames.astype("<f2").tofile(f)
    return path


def luma(frames):
    f = frames.astype(np.float32)
    return np.clip(f[..., 0] * 0.2126 + f[..., 1] * 0.7152 + f[..., 2] * 0.0722, 0, 1)


class JobTest(unittest.TestCase):
    def run_job(self, root, frames, chunks, contrib, simulate="none"):
        job_id = f"test-{os.getpid()}-{simulate}"
        n = frames.shape[0]
        keys = [f"{simulate[:2]}{i:014x}" for i in range(n)]
        manifest = {
            "job_id": job_id, "input": stage(job_id, frames), "width": frames.shape[2],
            "height": frames.shape[1], "frames": n, "chunks": chunks, "frame_keys": keys,
            "contrib": contrib, "cache_root": root,
            "worker": os.path.join(WORKER_DIR, "dummy_worker.py"), "seconds": 0.1,
            "simulate": simulate, "precision": "bf16", "title": "test",
        }
        mpath = f"/dev/shm/ltxag_job_{job_id}.json"
        with open(mpath, "w") as f:
            json.dump(manifest, f)
        env = dict(os.environ, HOME=root)  # keep test logs out of the user's real log
        rc = subprocess.run([sys.executable, "-I", "-B", os.path.join(WORKER_DIR, "job.py"), mpath,
                             "--no-ui"], timeout=60, env=env).returncode
        self.assertTrue(os.path.exists(os.path.join(root, ".cache/ltx-alphagen/logs/ltx-alphagen.log")))
        leftovers = [p for p in os.listdir("/dev/shm") if p.startswith(f"ltxag_job_{job_id}")]
        self.assertEqual(leftovers, [], "job must clean up its /dev/shm files")
        self.assertEqual(os.listdir(os.path.join(root, "pending")), [], "pending markers left")
        with open(os.path.join(root, "jobs", job_id + ".json")) as f:
            return rc, keys, json.load(f)

    def test_two_chunks_blended(self):
        rng = np.random.default_rng(0)
        frames = rng.random((12, 5, 7, 3)).astype(np.float16)
        # Two chunks overlapping on frames 4..7; frames 5 and 6 crossfade.
        chunks = [[0, 8], [4, 8]]
        contrib = [[[0, 1.0]]] * 5 + [[[0, 0.75], [1, 0.25]], [[0, 0.25], [1, 0.75]]] + [[[1, 1.0]]] * 5
        with tempfile.TemporaryDirectory() as root:
            rc, keys, status = self.run_job(root, frames, chunks, contrib)
            self.assertEqual((rc, status["state"]), (0, "done"), status)
            self.assertEqual((status["chunks_done"], status["chunks"], status["frames_done"]), (2, 2, 12))
            want = luma(frames)
            for i, k in enumerate(keys):
                with open(cache.matte_path(root, k), "rb") as f:
                    hdr = transport.HEADER.unpack(f.read(transport.HEADER.size))
                    got = np.fromfile(f, dtype="<f2").reshape(5, 7)
                self.assertEqual(hdr, (transport.OUT_MAGIC, 1, 7, 5, 1, 1))
                np.testing.assert_allclose(got, want[i], atol=2e-3)

    def test_oom_reports_failure_and_cleans_up(self):
        frames = np.zeros((3, 4, 4, 3), np.float16)
        with tempfile.TemporaryDirectory() as root:
            rc, keys, status = self.run_job(root, frames, [[0, 3]], [[[0, 1.0]]] * 3, "oom")
            self.assertEqual(rc, 1)
            self.assertEqual(status["state"], "failed")
            self.assertIn("oom", status["message"])
            self.assertFalse(os.path.exists(cache.matte_path(root, keys[0])))

    def test_crash_reports_signal(self):
        frames = np.zeros((3, 4, 4, 3), np.float16)
        with tempfile.TemporaryDirectory() as root:
            rc, _, status = self.run_job(root, frames, [[0, 3]], [[[0, 1.0]]] * 3, "crash")
            self.assertEqual((rc, status["state"]), (1, "failed"))
            self.assertIn("signal 11", status["message"])


if __name__ == "__main__":
    unittest.main()
