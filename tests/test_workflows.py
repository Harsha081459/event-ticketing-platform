import re
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

SERVER = Path(__file__).resolve().parents[1] / "bin" / "etp_server"


class Client:
    def __init__(self, port):
        self.socket = socket.create_connection(("127.0.0.1", port), timeout=5)
        self.socket.settimeout(5)
        self.pending = b""
        self.receive()

    def receive(self):
        while b"etp> " not in self.pending:
            part = self.socket.recv(65536)
            if not part:
                raise AssertionError("Server closed connection before replying")
            self.pending += part
        response, self.pending = self.pending.split(b"etp> ", 1)
        return response.decode("utf-8", errors="replace").strip()

    def command(self, command):
        self.socket.sendall((command + "\n").encode())
        return self.receive()

    def close(self):
        self.socket.close()


@unittest.skipUnless(sys.platform.startswith("linux"), "requires the Linux/POSIX server build")
class Workflows(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="etp-test-")
        self.log = open(Path(self.directory.name) / "server-output.log", "w+")
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            self.port = probe.getsockname()[1]
        self.clients = []
        self.process = None
        self.start()

    def start(self):
        self.process = subprocess.Popen(
            [str(SERVER), "-p", str(self.port)], cwd=self.directory.name,
            stdout=self.log, stderr=subprocess.STDOUT,
        )
        for _ in range(100):
            if self.process.poll() is not None:
                raise AssertionError("Server exited during startup")
            try:
                client = self.client()
                client.close()
                return
            except (ConnectionError, OSError):
                time.sleep(0.05)
        raise AssertionError("Server did not start")

    def stop(self):
        if self.process and self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
                raise AssertionError("Graceful shutdown timed out")
            self.assertEqual(self.process.returncode, 0)

    def tearDown(self):
        for client in self.clients:
            client.close()
        try:
            self.stop()
        finally:
            self.log.seek(0)
            output = self.log.read()
            self.log.close()
            if not self._outcome.success:
                print(output[-12000:])
            self.directory.cleanup()

    def client(self):
        client = Client(self.port)
        self.clients.append(client)
        return client

    def ok(self, response):
        self.assertTrue(response.startswith("OK"), response)
        return response

    def admin(self):
        client = self.client()
        self.ok(client.command("LOGIN admin admin123"))
        return client

    def event(self, admin, name="Demo"):
        response = self.ok(admin.command(f"CREATE_EVENT {name} Hall 2027-07-20 14:00 1 20 100"))
        return int(re.search(r"ID: (\d+)", response)[1])

    def customer(self, name):
        client = self.client()
        self.ok(client.command(f"REGISTER {name} pass customer"))
        self.ok(client.command(f"LOGIN {name} pass"))
        return client

    def test_booking_cancel_and_rebook(self):
        admin = self.admin()
        event = self.event(admin)
        customer = self.customer("alice")
        response = self.ok(customer.command(f"BOOK {event} 1 2"))
        booking = int(re.search(r"Booking ID: (\d+)", response)[1])
        self.assertIn("BOOKED", admin.command(f"VIEW_SEATS {event}"))
        stranger = self.customer("bob")
        self.assertTrue(stranger.command(f"CANCEL {booking}").startswith("ERROR"))
        self.ok(customer.command(f"CANCEL {booking}"))
        self.assertTrue(customer.command(f"CANCEL {booking}").startswith("ERROR"))
        self.ok(stranger.command(f"BOOK {event} 1 2"))

    def test_tcp_fragmented_and_coalesced_commands(self):
        client = self.client()
        client.socket.sendall(b"LOG")
        time.sleep(0.05)
        client.socket.sendall(b"IN admin admin123\nLIST_EVENTS\n")
        self.assertIn("Welcome, admin", self.ok(client.receive()))
        self.assertIn("events", self.ok(client.receive()).lower())

    def test_oversized_line_is_drained_without_executing_suffix(self):
        client = self.client()
        client.socket.sendall(b"X" * 1200 + b"\nHELP\n")
        self.assertTrue(client.receive().startswith("ERROR"))
        self.assertIn("commands", client.receive().lower())

    def test_duplicate_seats_are_rejected_without_booking(self):
        admin = self.admin()
        event = self.event(admin)
        customer = self.customer("alice")
        self.assertTrue(customer.command(f"BOOK {event} 1 1").startswith("ERROR"))
        self.assertIn("No active bookings", customer.command("MY_BOOKINGS"))
        self.ok(customer.command(f"BOOK {event} 1"))

    def test_contended_booking_has_exactly_one_winner(self):
        event = self.event(self.admin())
        clients = [self.customer(f"customer{i}") for i in range(8)]
        barrier = threading.Barrier(len(clients))

        def book(client):
            barrier.wait(timeout=5)
            return client.command(f"BOOK {event} 1 2 3")

        with ThreadPoolExecutor(max_workers=8) as pool:
            responses = list(pool.map(book, clients))
        self.assertEqual(sum(response.startswith("OK") for response in responses), 1, responses)

    def test_disjoint_concurrent_bookings_preserve_all_records(self):
        admin = self.admin()
        event = self.event(admin)
        clients = [self.customer(f"customer{i}") for i in range(8)]
        barrier = threading.Barrier(len(clients))

        def book(pair):
            index, client = pair
            barrier.wait(timeout=5)
            return client.command(f"BOOK {event} {index + 1}")

        with ThreadPoolExecutor(max_workers=8) as pool:
            responses = list(pool.map(book, enumerate(clients)))
        self.assertEqual(sum(response.startswith("OK") for response in responses), 8, responses)
        self.assertEqual(admin.command(f"VIEW_SEATS {event}").count("BOOKED"), 8)
        for client in clients:
            self.assertIn("Your bookings (1)", client.command("MY_BOOKINGS"))

    def test_concurrent_cancellation_succeeds_once(self):
        event = self.event(self.admin())
        first = self.customer("alice")
        second = self.client()
        self.ok(second.command("LOGIN alice pass"))
        response = self.ok(first.command(f"BOOK {event} 1"))
        booking = int(re.search(r"Booking ID: (\d+)", response)[1])
        barrier = threading.Barrier(2)

        def cancel(client):
            barrier.wait(timeout=5)
            return client.command(f"CANCEL {booking}")

        with ThreadPoolExecutor(max_workers=2) as pool:
            responses = list(pool.map(cancel, [first, second]))
        self.assertEqual(sum(response.startswith("OK") for response in responses), 1, responses)
        self.ok(first.command(f"BOOK {event} 1"))

    def test_restart_preserves_records_and_allocates_new_ids(self):
        admin = self.admin()
        first_id = self.event(admin, "BeforeRestart")
        booking_response = self.ok(self.customer("alice").command(f"BOOK {first_id} 1 2"))
        booking_id = int(re.search(r"Booking ID: (\d+)", booking_response)[1])
        self.stop()
        for client in self.clients:
            client.close()
        self.clients = []
        self.start()
        admin = self.admin()
        self.assertIn("BeforeRestart", self.ok(admin.command("LIST_EVENTS")))
        self.assertGreater(self.event(admin, "AfterRestart"), first_id)
        alice = self.client()
        self.ok(alice.command("LOGIN alice pass"))
        self.assertIn("Your bookings (1)", alice.command("MY_BOOKINGS"))
        self.ok(alice.command(f"CANCEL {booking_id}"))
        self.ok(alice.command(f"BOOK {first_id} 1 2"))

    def test_guest_cannot_self_assign_organizer(self):
        client = self.client()
        self.assertTrue(client.command("REGISTER intruder pass organizer").startswith("DENIED"))
        self.ok(client.command("REGISTER customer pass customer"))

    def test_invalid_price_and_foreign_event_ownership(self):
        admin = self.admin()
        for price in ["-1", "nan", "inf"]:
            self.assertTrue(admin.command(f"CREATE_EVENT Bad Hall 2027-01-01 14:00 1 5 {price}").startswith("ERROR"))
        self.ok(admin.command("REGISTER organizer pass organizer"))
        event = self.event(admin)
        organizer = self.client()
        self.ok(organizer.command("LOGIN organizer pass"))
        self.assertTrue(organizer.command(f"DELETE_EVENT {event}").startswith("DENIED"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
