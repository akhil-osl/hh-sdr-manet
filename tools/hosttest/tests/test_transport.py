import threading
import unittest

from fake_board import FakeBoard, closed_port
from hosttest.transport import (ConnectFailed, TcpTransport, TransportClosed,
                                TransportTimeout)


def read_exactly(link: TcpTransport, count: int) -> bytes:
    data = b""
    while len(data) < count:
        data += link.receive(timeout_s=1.0)
    return data


class TransportTest(unittest.TestCase):
    def make(self, port: int, sleeps=None) -> TcpTransport:
        record = sleeps if sleeps is not None else []
        link = TcpTransport("127.0.0.1", port, connect_timeout_s=1.0,
                            sleep=record.append)
        self.addCleanup(link.close)
        return link

    def test_echo_roundtrip(self):
        def echo(conn):
            conn.sendall(conn.recv(64))
        with FakeBoard(echo) as board:
            link = self.make(board.port)
            link.connect()
            link.send(b"PING", timeout_s=1.0)
            self.assertEqual(read_exactly(link, 4), b"PING")
            link.close()
            self.assertFalse(link.is_open)

    def test_fragmented_reply_is_reassembled_by_caller(self):
        def two_parts(conn):
            conn.sendall(b"AB")
            conn.sendall(b"CD")
        with FakeBoard(two_parts) as board:
            link = self.make(board.port)
            link.connect()
            self.assertEqual(read_exactly(link, 4), b"ABCD")

    def test_receive_times_out_when_board_is_silent(self):
        release = threading.Event()
        with FakeBoard(lambda conn: release.wait(2)) as board:
            link = self.make(board.port)
            link.connect()
            with self.assertRaises(TransportTimeout):
                link.receive(timeout_s=0.05)
            self.assertTrue(link.is_open)
            release.set()

    def test_send_times_out_when_board_never_reads(self):
        release = threading.Event()
        with FakeBoard(lambda conn: release.wait(2)) as board:
            link = self.make(board.port)
            link.connect()
            with self.assertRaises(TransportTimeout):
                link.send(b"x" * (32 * 1024 * 1024), timeout_s=0.1)
            release.set()

    def test_peer_close_raises_closed_and_closes_link(self):
        with FakeBoard(lambda conn: None) as board:
            link = self.make(board.port)
            link.connect()
            with self.assertRaises(TransportClosed):
                link.receive(timeout_s=1.0)
            self.assertFalse(link.is_open)

    def test_use_before_connect_raises_closed(self):
        link = self.make(closed_port())
        with self.assertRaises(TransportClosed):
            link.send(b"x", timeout_s=0.1)
        with self.assertRaises(TransportClosed):
            link.receive(timeout_s=0.1)

    def test_connect_refused_raises_connect_failed(self):
        with self.assertRaises(ConnectFailed):
            self.make(closed_port()).connect()

    def test_retry_is_bounded_and_backs_off(self):
        sleeps = []
        link = self.make(closed_port(), sleeps)
        with self.assertRaises(ConnectFailed):
            link.connect_with_retry(attempts=4, backoff_s=0.5)
        self.assertEqual(sleeps, [0.5, 1.0, 2.0])

    def test_retry_rejects_zero_attempts(self):
        with self.assertRaises(ValueError):
            self.make(closed_port()).connect_with_retry(attempts=0, backoff_s=0.1)

    def test_reconnect_after_link_drop(self):
        def one_byte_then_close(conn):
            conn.sendall(b"x")
        with FakeBoard(one_byte_then_close) as board:
            link = self.make(board.port)
            link.connect()
            self.assertEqual(read_exactly(link, 1), b"x")
            with self.assertRaises(TransportClosed):
                link.receive(timeout_s=1.0)
            link.connect_with_retry(attempts=3, backoff_s=0.0)
            self.assertEqual(read_exactly(link, 1), b"x")

    def test_connect_succeeds_after_board_comes_up(self):
        board_holder = []
        sleeps = []

        def start_board_on_first_sleep(seconds):
            sleeps.append(seconds)
            board_holder.append(FakeBoard(lambda conn: None))
            link._port = board_holder[0].port

        link = TcpTransport("127.0.0.1", closed_port(), connect_timeout_s=1.0,
                            sleep=start_board_on_first_sleep)
        self.addCleanup(link.close)
        self.addCleanup(lambda: [b.stop() for b in board_holder])
        link.connect_with_retry(attempts=3, backoff_s=0.5)
        self.assertTrue(link.is_open)
        self.assertEqual(sleeps, [0.5])


if __name__ == "__main__":
    unittest.main()
