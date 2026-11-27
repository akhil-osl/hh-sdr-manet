import subprocess
import unittest

from fake_board import FakeSsh, radioctl_text
from hosttest.board_ssh import SshStatusSource, StatusError, parse_reads, remote_command
from hosttest.config import StatusConfig

CFG = StatusConfig("root@192.0.2.10", "/opt/radhawk_each_worker_status", "/tmp/r.sock", 2.0)


class ParseReadsTest(unittest.TestCase):
    def test_values_come_from_the_runbook_format(self):
        self.assertEqual(parse_reads(radioctl_text(5, 2, 7)),
                         {"crc_ok_count": 5, "crc_bad_count": 2, "link_status": 7})

    def test_a_large_unsigned_counter_is_kept(self):
        self.assertEqual(parse_reads(radioctl_text(3235791364, 0, 7))["crc_ok_count"],
                         3235791364)

    def test_a_missing_property_is_an_error(self):
        text = radioctl_text(1, 1, 7).split("worker=rx_fts")[0]
        with self.assertRaisesRegex(StatusError, "rx_fts link_status"):
            parse_reads(text)

    def test_a_non_number_is_an_error(self):
        with self.assertRaisesRegex(StatusError, "not a number"):
            parse_reads(radioctl_text(1, 1, 7).replace("value=1\n", "value=x\n", 1))

    def test_empty_and_noise_are_errors(self):
        for text in ("", "radioctl: cannot connect\n"):
            with self.assertRaises(StatusError):
                parse_reads(text)


class RemoteCommandTest(unittest.TestCase):
    def test_runs_the_three_runbook_reads_in_the_radioctl_folder(self):
        cmd = remote_command(CFG)
        self.assertTrue(cmd.startswith("cd /opt/radhawk_each_worker_status && "))
        for part in ("opencpi get rx_ber crc_ok_count", "opencpi get rx_ber crc_bad_count",
                     "opencpi get rx_fts link_status", "-s /tmp/r.sock"):
            self.assertIn(part, cmd)

    def test_config_values_are_quoted(self):
        cfg = StatusConfig("h", "/a b; rm x", "/s'; id", 1.0)
        cmd = remote_command(cfg)
        self.assertIn("cd '/a b; rm x' && ", cmd)
        self.assertIn("-s '/s'\"'\"'; id' ", cmd)


class SshSourceTest(unittest.TestCase):
    def setUp(self):
        self.fake = FakeSsh()
        self.src = SshStatusSource(CFG, run=self.fake)

    def test_a_good_read(self):
        ok, counters, reason = self.src.read()
        self.assertEqual((ok, reason), (True, ""))
        self.assertEqual(counters["crc_ok_count"], 10)

    def test_ssh_is_called_without_a_prompt_and_with_the_target_after_dashes(self):
        self.src.read()
        argv = self.fake.calls[0]
        self.assertEqual(argv[:3], ["ssh", "-o", "BatchMode=yes"])
        self.assertEqual(argv[argv.index("--") + 1], "root@192.0.2.10")

    def test_an_ssh_error_is_a_failed_read_with_the_first_line(self):
        self.fake.fail = "Permission denied (publickey)."
        ok, _, reason = self.src.read()
        self.assertFalse(ok)
        self.assertIn("Permission denied", reason)

    def test_a_hang_is_a_failed_read(self):
        self.fake.hang = True
        ok, _, reason = self.src.read()
        self.assertFalse(ok)
        self.assertIn("no reply within 2s", reason)

    def test_no_ssh_program_is_a_failed_read(self):
        def missing(argv, **kw):
            raise FileNotFoundError(2, "No such file or directory")
        ok, _, reason = SshStatusSource(CFG, run=missing).read()
        self.assertFalse(ok)
        self.assertIn("cannot run ssh", reason)

    def test_an_unreadable_reply_is_a_failed_read(self):
        def junk(argv, **kw):
            return subprocess.CompletedProcess(argv, 0, "hello\n", "")
        ok, _, reason = SshStatusSource(CFG, run=junk).read()
        self.assertFalse(ok)
        self.assertIn("crc_ok_count", reason)


if __name__ == "__main__":
    unittest.main()
