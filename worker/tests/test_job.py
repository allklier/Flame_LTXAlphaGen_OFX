"""Runs job.py end to end (no UI) on a staged input, as the OFX Process button would."""

import csv
import json
import os
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

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
            # Not started by Flame: no refresh marker (the hook would step a live Flame's frame).
            self.assertFalse(os.path.exists(os.path.join(root, ".cache/ltx-alphagen/run/refresh")))
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


class TrackerTest(unittest.TestCase):
    """Progress line and time-left estimate from stage/step events (fake clock)."""

    def setUp(self):
        import job

        self.job = job
        self.now = 1000.0
        patcher = mock.patch.object(job.time, "time", lambda: self.now)
        patcher.start()
        self.addCleanup(patcher.stop)

    def test_line_and_estimate(self):
        t = self.job.Tracker([[0, 145], [120, 145]], 265)
        self.assertEqual(t.render(), (0.0, "Starting the worker · 0:00 elapsed"))
        self.now += 5
        t.on_stage(0, "converting")
        self.now += 4
        t.on_stage(0, "denoising")
        frac, text = t.render()
        self.assertIn("Chunk 1/2 · frames 1–145 of 265 · starting · 0:09 elapsed · estimating time left", text)
        self.now += 16  # step 1 includes the transformer build: not used for the rate
        t.on_step(0, 1, 8)
        self.assertIn("estimating time left", t.render()[1])
        self.now += 10
        t.on_step(0, 2, 8)
        frac, text = t.render()
        self.assertIn("step 2/8", text)
        # left: 6 steps x 10 s + decode 1.4 x 10 s, then chunk 2: 4 s + 80 s + 14 s = 74 + 98 = 172 s
        self.assertIn("about 2:52 left", text)
        self.assertAlmostEqual(frac, (0.05 + 0.85 * 2 / 8) * 145 / 290)
        for k in range(3, 9):
            self.now += 10
            t.on_step(0, k, 8)
        t.on_stage(0, "decoding")
        self.now += 20  # the real decode took 20 s: chunk 2 uses that
        t.on_chunk_done(0)
        t.on_stage(1, "preparing")
        frac, text = t.render()
        self.assertIn("Chunk 2/2 · frames 121–265 of 265 · preparing", text)
        self.assertIn("about 1:44 left", text)  # 4 + 80 + 20
        self.assertAlmostEqual(frac, 0.5)


class RefreshMarkerTest(unittest.TestCase):
    def test_marker_only_for_flame(self):
        import job

        self.assertFalse(job.started_by_flame(os.getpid()))  # this test runs in python, not flame
        self.assertFalse(job.started_by_flame(0))
        with tempfile.TemporaryDirectory() as home, mock.patch.object(job, "REFRESH_DIR", os.path.join(home, "r")):
            job.signal_refresh("j1", "n1", "done")
            with open(os.path.join(home, "r", "j1.json")) as f:
                self.assertEqual(json.load(f)["node_id"], "n1")


class VramRecordTest(unittest.TestCase):
    def test_csv_row(self):
        import job

        with tempfile.TemporaryDirectory() as home, mock.patch.dict(os.environ, {"HOME": home}):
            m = {"job_id": "j1", "precision": "fp8", "width": 1920, "height": 1080, "chunks": [[0, 145]],
                 "unload": False, "vram": {"estimate_gb": 43.8, "free_gb": 89.7, "resident_gb": 0.0,
                                           "reserve_gb": 2.7, "model_w": 1920, "model_h": 1088,
                                           "model_frames": 145, "warned": True, "total_gb": 95.59,
                                           "gpu": "NVIDIA RTX PRO 6000", "driver": "580.159.04"}}
            job.record_vram(m, "vram 49.80 39.50 40.10 0.00 2.13.0+cu132", "done")
            with open(os.path.join(home, ".cache/ltx-alphagen/logs/vram_stats.csv")) as f:
                rows = list(csv.DictReader(f))
        self.assertEqual(len(rows), 1)
        self.assertEqual((rows[0]["used_gb"], rows[0]["estimate_gb"], rows[0]["warned"], rows[0]["resident"]),
                         ("39.9", "43.8", "1", "1"))
        self.assertEqual((rows[0]["gpu"], rows[0]["total_gb"], rows[0]["driver"], rows[0]["torch"]),
                         ("NVIDIA RTX PRO 6000", "95.59", "580.159.04", "2.13.0+cu132"))


if __name__ == "__main__":
    unittest.main()
