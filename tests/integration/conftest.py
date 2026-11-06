"""Starts a server per test on a free port with its own data directory.

Set KVSTORE_TARGET=redis to run the protocol and command tests against redis-server
instead, which checks that both behave the same. Tests marked kvstore_only cover
behaviour specific to this implementation (file format, logging, limits).
"""

from __future__ import annotations

import os
import signal
import socket
import subprocess
import time
from collections.abc import Callable, Iterator
from pathlib import Path

import pytest

from resp_client import Client

ROOT = Path(__file__).resolve().parents[2]
TARGET = os.environ.get("KVSTORE_TARGET", "kvstore")
BINARY = os.environ.get("KVSTORE_BIN", str(ROOT / "build" / "kvstore"))


def pytest_collection_modifyitems(config: pytest.Config, items: list[pytest.Item]) -> None:
    if TARGET == "kvstore":
        return
    skip = pytest.mark.skip(reason="specific to kvstore")
    for item in items:
        if "kvstore_only" in item.keywords:
            item.add_marker(skip)


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return int(s.getsockname()[1])


class Server:
    def __init__(self, directory: Path, extra: list[str], preexec: Callable[[], None] | None = None) -> None:
        self.directory = directory
        self.preexec = preexec
        self.port = free_port()
        self.extra = extra
        self.log = directory / "server.log"
        self.process: subprocess.Popen[bytes] | None = None

    def command_line(self) -> list[str]:
        if TARGET == "redis":
            return ["redis-server", "--port", str(self.port), "--dir", str(self.directory), "--save", "", *self.extra]
        return [BINARY, "--port", str(self.port), "--dir", str(self.directory), *self.extra]

    def start(self, expect_failure: bool = False) -> int | None:
        with self.log.open("ab") as log:
            self.process = subprocess.Popen(self.command_line(), stdout=log, stderr=log, preexec_fn=self.preexec)
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                if expect_failure:
                    return self.process.returncode
                raise RuntimeError(f"server exited with {self.process.returncode}:\n{self.log.read_text()}")
            try:
                with self.client() as c:
                    if c.call("PING") == "PONG":
                        return None
            except OSError:
                time.sleep(0.02)
        raise RuntimeError("server did not start")

    def stop(self, sig: int = signal.SIGTERM) -> int:
        assert self.process is not None
        if self.process.poll() is None:
            self.process.send_signal(sig)
        code = self.process.wait(timeout=10)
        self.process = None
        return code

    def restart(self) -> None:
        self.stop()
        self.start()

    def client(self, timeout: float = 5.0) -> Client:
        return Client("127.0.0.1", self.port, timeout)


@pytest.fixture
def make_server(tmp_path: Path) -> Iterator[object]:
    servers: list[Server] = []

    def factory(*extra: str, start: bool = True, preexec: Callable[[], None] | None = None) -> Server:
        server = Server(tmp_path, list(extra), preexec)
        servers.append(server)
        if start:
            server.start()
        return server

    yield factory
    for server in servers:
        if server.process is not None:
            server.process.kill()
            server.process.wait()


@pytest.fixture
def server(make_server) -> Server:  # type: ignore[no-untyped-def]
    return make_server()


@pytest.fixture
def client(server: Server) -> Iterator[Client]:
    with server.client() as c:
        yield c
