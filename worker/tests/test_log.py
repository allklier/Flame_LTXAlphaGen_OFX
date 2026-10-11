import os
import re
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

from ltxag_worker import log as ltxlog  # noqa: E402

# Must match ofx/src/log.cpp exactly (one shared timeline).
LINE = re.compile(r"^\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{3} (DEBUG|INFO |WARN |ERROR) "
                  r"job\[\d+/\d+\] node=n1 job=j1 \| (.*)$")


class LogTest(unittest.TestCase):
    def test_format_and_level(self):
        with tempfile.TemporaryDirectory() as home, \
                mock.patch.dict(os.environ, {"HOME": home, "LTXAG_LOG_LEVEL": "info"}):
            lg = ltxlog.Logger("job", node="n1", job="j1")
            lg.info("hello")
            lg.debug("hidden")
            lg.error("bad thing")
            with open(ltxlog.log_path()) as f:
                lines = f.read().splitlines()
        self.assertEqual(len(lines), 2, lines)
        m0, m1 = LINE.match(lines[0]), LINE.match(lines[1])
        self.assertTrue(m0 and m1, lines)
        self.assertEqual((m0.group(1), m0.group(2)), ("INFO ", "hello"))
        self.assertEqual((m1.group(1), m1.group(2)), ("ERROR", "bad thing"))

    def test_never_raises(self):
        with mock.patch.dict(os.environ, {"HOME": "/nonexistent/forbidden"}):
            ltxlog.Logger("job").error("no crash")


if __name__ == "__main__":
    unittest.main()
