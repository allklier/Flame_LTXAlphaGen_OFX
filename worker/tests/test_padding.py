import os
import sys
import tempfile
import unittest

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from ltxag_worker import padding, transport  # noqa: E402


class PaddingTest(unittest.TestCase):
    def test_padded_sizes(self):
        self.assertEqual(padding.padded_size(1080), 1088)
        self.assertEqual(padding.padded_size(720), 736)
        self.assertEqual(padding.padded_size(1920), 1920)
        self.assertEqual(padding.padded_size(3240), 3264)

    def test_padded_frame_count(self):
        for n, want in [(1, 1), (2, 9), (9, 9), (10, 17), (24, 25), (144, 145), (145, 145)]:
            self.assertEqual(padding.padded_frame_count(n), want, n)
            self.assertEqual((padding.padded_frame_count(n) - 1) % 8, 0)

    def test_spatial_roundtrip_and_edge_replicate(self):
        rng = np.random.default_rng(0)
        frames = rng.random((3, 1080, 64, 3)).astype(np.float16)
        padded, (top, left) = padding.pad_spatial(frames)
        self.assertEqual(padded.shape, (3, 1088, 64, 3))
        self.assertEqual((top, left), (4, 0))  # 8 rows split 4 + 4
        np.testing.assert_array_equal(padded[:, 0], frames[:, 0])  # edge replicated
        np.testing.assert_array_equal(padded[:, -1], frames[:, -1])
        back = padding.crop(padded[..., 0], 3, top, left, 1080, 64)
        np.testing.assert_array_equal(back, frames[..., 0])

    def test_temporal_mirror_tail(self):
        frames = np.arange(10, dtype=np.float16).reshape(10, 1, 1, 1)
        out = padding.pad_temporal(frames)
        self.assertEqual(out.shape[0], 17)
        self.assertEqual(list(out[:, 0, 0, 0]), list(range(10)) + [8, 7, 6, 5, 4, 3, 2])

    def test_temporal_short_clip_holds_last(self):
        frames = np.arange(2, dtype=np.float16).reshape(2, 1, 1, 1)
        out = padding.pad_temporal(frames)
        self.assertEqual(list(out[:, 0, 0, 0]), [0, 1] + [1] * 7)

    def test_temporal_noop(self):
        frames = np.zeros((145, 1, 1, 1), np.float16)
        self.assertIs(padding.pad_temporal(frames), frames)

    def test_hold_indices(self):
        self.assertEqual(padding.hold_indices(4, 1), [0, 1, 2, 3])
        self.assertEqual(padding.hold_indices(3, 2), [0, 1, 1, 2, 2])
        self.assertEqual(len(padding.hold_indices(19, 8)), 145)  # 8x: 19 source frames per 145
        self.assertEqual(padding.hold_indices(1, 8), [0])
        # 8x: every 8-frame latent group (frames 1..8, 9..16, ...) holds one source frame
        idx = padding.hold_indices(19, 8)
        for g in range(18):
            self.assertEqual(set(idx[1 + 8 * g: 9 + 8 * g]), {g + 1})

    def test_held_output_indices(self):
        for hold in (1, 2, 4, 8):
            for n in (1, 2, 10, 19):
                idx, out = padding.hold_indices(n, hold), padding.held_output_indices(n, hold)
                self.assertEqual(len(out), n)
                self.assertEqual([idx[o] for o in out], list(range(n)), (hold, n))
        self.assertEqual(padding.held_output_indices(3, 8), [0, 5, 13])

    def test_transport_roundtrip(self):
        rng = np.random.default_rng(1)
        frames = rng.random((5, 7, 9, 3)).astype(np.float16)
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "in.bin")
            with open(p, "wb") as f:
                f.write(transport.HEADER.pack(transport.IN_MAGIC, transport.VERSION, 9, 7, 5, 3))
                frames.astype("<f2").tofile(f)
            np.testing.assert_array_equal(transport.read_frames(p), frames)
            q = os.path.join(d, "out.bin")
            transport.write_mattes(q, frames[..., 0])
            with open(q, "rb") as f:
                hdr = transport.HEADER.unpack(f.read(transport.HEADER.size))
            self.assertEqual(hdr, (transport.OUT_MAGIC, 1, 9, 7, 5, 1))


if __name__ == "__main__":
    unittest.main()
