"""Command-line parsing: --help and invalid invocations exit immediately,
before the server tries to bind. A valid invocation starts a real
long-running server, covered elsewhere via start_server() instead --
run_server_once() would just time out waiting on it.
"""

import unittest

from helpers import run_server_once


class SmokeTest(unittest.TestCase):
    def test_help_exits_zero_and_prints_usage(self):
        result = run_server_once("--help")
        self.assertEqual(result.returncode, 0, msg=result.stderr)
        self.assertIn("--port", result.stdout)

    def test_unknown_flag_exits_two(self):
        result = run_server_once("--this-flag-does-not-exist")
        self.assertEqual(result.returncode, 2)
        self.assertIn("unknown option", result.stderr)

    def test_bad_port_exits_two(self):
        result = run_server_once("--port", "not-a-number")
        self.assertEqual(result.returncode, 2)

    def test_out_of_range_port_exits_two(self):
        result = run_server_once("--port", "70000")
        self.assertEqual(result.returncode, 2)


if __name__ == "__main__":
    unittest.main()
