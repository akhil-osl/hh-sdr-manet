import unittest

from hosttest.codec import MAX_PENDING_BYTES, LineCodec


class CodecTest(unittest.TestCase):
    def test_encode_builds_one_line(self):
        self.assertEqual(LineCodec().encode("t1", "READ", ["ch1", "x"]),
                         b"t1 READ ch1 x\n")

    def test_encode_rejects_whitespace_in_a_token(self):
        for bad in ("a b", "a\nb", ""):
            with self.assertRaises(ValueError):
                LineCodec().encode("t1", "READ", [bad])

    def test_reply_is_parsed(self):
        [reply] = LineCodec().feed(b"t1 OK id=B7 fw=1.2\n")
        self.assertEqual((reply.tag, reply.status), ("t1", "OK"))
        self.assertEqual(reply.fields, {"id": "B7", "fw": "1.2"})

    def test_reply_split_over_reads_is_joined(self):
        codec = LineCodec()
        self.assertEqual(codec.feed(b"t1 OK va"), [])
        [reply] = codec.feed(b"lue=42\n")
        self.assertEqual(reply.fields, {"value": "42"})

    def test_two_replies_in_one_read_are_both_returned(self):
        replies = LineCodec().feed(b"t1 OK\r\nt2 ERR\n")
        self.assertEqual([r.tag for r in replies], ["t1", "t2"])

    def test_unparseable_lines_have_no_tag(self):
        for text in (b"garbage\n", b"t1 OK nofield\n", b"t1 OK =x\n"):
            [reply] = LineCodec().feed(text)
            self.assertIsNone(reply.tag)

    def test_blank_lines_are_skipped(self):
        self.assertEqual(LineCodec().feed(b"\n  \r\n"), [])

    def test_overlong_line_is_dropped_and_reported(self):
        codec = LineCodec()
        [reply] = codec.feed(b"x" * (MAX_PENDING_BYTES + 1))
        self.assertIsNone(reply.tag)
        [ok] = codec.feed(b"t1 OK\n")
        self.assertEqual(ok.tag, "t1")


if __name__ == "__main__":
    unittest.main()
