import unittest

from fake_board import FakeBoard, line_script
from hosttest.codec import LineCodec
from hosttest.command import FAIL, PASS, CommandClient
from hosttest.transport import TcpTransport


class CommandTest(unittest.TestCase):
    def client_for(self, handler):
        board = FakeBoard(line_script(handler))
        self.addCleanup(board.stop)
        link = TcpTransport("127.0.0.1", board.port, 1.0, sleep=lambda s: None)
        link.connect()
        self.addCleanup(link.close)
        return CommandClient(link, LineCodec())

    @staticmethod
    def tag(line):
        return line.split()[0]

    def test_matching_reply_with_expected_fields_passes(self):
        client = self.client_for(lambda l: [f"{self.tag(l)} OK value=42\n"])
        res = client.execute("READ", ["ch1"], {"status": "OK", "value": "42"}, 1.0)
        self.assertEqual(res.verdict, PASS)
        self.assertEqual(res.tx, f"{res.tag} READ ch1")
        self.assertEqual(res.rx, f"{res.tag} OK value=42")
        self.assertEqual(res.fields, {"value": "42"})

    def test_wrong_status_fails_with_reason(self):
        client = self.client_for(lambda l: [f"{self.tag(l)} ERR\n"])
        res = client.execute("READ", [], {"status": "OK"}, 1.0)
        self.assertEqual(res.verdict, FAIL)
        self.assertIn("status=OK", res.reason)

    def test_wrong_field_value_fails(self):
        client = self.client_for(lambda l: [f"{self.tag(l)} OK value=7\n"])
        res = client.execute("READ", [], {"value": "42"}, 1.0)
        self.assertEqual(res.verdict, FAIL)
        self.assertIn("value=42", res.reason)

    def test_missing_expected_field_fails(self):
        client = self.client_for(lambda l: [f"{self.tag(l)} OK\n"])
        self.assertEqual(client.execute("READ", [], {"value": "42"}, 1.0).verdict, FAIL)

    def test_silent_board_times_out(self):
        client = self.client_for(lambda l: None)
        res = client.execute("READ", [], {"status": "OK"}, 0.1)
        self.assertEqual(res.verdict, FAIL)
        self.assertIn("no reply", res.reason)
        self.assertIsNone(res.rx)

    def test_reply_with_other_tag_is_not_accepted(self):
        client = self.client_for(lambda l: ["other OK\n"])
        res = client.execute("READ", [], {"status": "OK"}, 0.1)
        self.assertEqual(res.verdict, FAIL)
        self.assertEqual(res.discarded, ["other OK"])

    def test_stale_and_garbage_lines_are_skipped_before_the_match(self):
        client = self.client_for(
            lambda l: ["t99 OK\n", "garbage\n", f"{self.tag(l)} OK\n"])
        res = client.execute("READ", [], {"status": "OK"}, 1.0)
        self.assertEqual(res.verdict, PASS)
        self.assertEqual(res.discarded, ["t99 OK", "garbage"])

    def test_late_reply_to_a_timed_out_command_cannot_pass_the_next(self):
        seen = []

        def handler(line):
            seen.append(self.tag(line))
            if len(seen) == 1:
                return None
            return [f"{seen[0]} OK\n", f"{seen[1]} OK\n"]

        client = self.client_for(handler)
        first = client.execute("A", [], {"status": "OK"}, 0.05)
        self.assertEqual(first.verdict, FAIL)
        second = client.execute("B", [], {"status": "OK"}, 1.0)
        self.assertEqual(second.verdict, PASS)
        self.assertEqual(second.discarded, [first.tag + " OK"])

    def test_reply_split_across_reads_is_assembled(self):
        client = self.client_for(
            lambda l: [f"{self.tag(l)} OK va", "lue=42\n"])
        res = client.execute("READ", [], {"value": "42"}, 1.0)
        self.assertEqual(res.verdict, PASS)

    def test_closed_link_fails_with_reason(self):
        board = FakeBoard(lambda conn: None)
        self.addCleanup(board.stop)
        link = TcpTransport("127.0.0.1", board.port, 1.0)
        link.connect()
        self.addCleanup(link.close)
        res = CommandClient(link, LineCodec()).execute("READ", [], {"status": "OK"}, 1.0)
        self.assertEqual(res.verdict, FAIL)
        self.assertIn("link closed", res.reason)

    def test_unencodable_command_fails_without_sending(self):
        client = self.client_for(lambda l: self.fail("must not be sent"))
        res = client.execute("BAD COMMAND", [], {"status": "OK"}, 0.1)
        self.assertEqual(res.verdict, FAIL)
        self.assertIn("not encodable", res.reason)


if __name__ == "__main__":
    unittest.main()
