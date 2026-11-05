"""Minimal RESP2 client with no dependencies, so the tests control exactly what is sent."""

from __future__ import annotations

import socket
from typing import Any


class ReplyError(Exception):
    pass


def encode(*args: str | bytes | int) -> bytes:
    parts = [b"*%d\r\n" % len(args)]
    for arg in args:
        data = arg if isinstance(arg, bytes) else str(arg).encode()
        parts.append(b"$%d\r\n%s\r\n" % (len(data), data))
    return b"".join(parts)


class Client:
    def __init__(self, host: str, port: int, timeout: float = 5.0) -> None:
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.buffer = b""

    def __enter__(self) -> Client:
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()

    def close(self) -> None:
        self.sock.close()

    def send_raw(self, data: bytes) -> None:
        self.sock.sendall(data)

    def call(self, *args: str | bytes | int) -> Any:
        self.send_raw(encode(*args))
        return self.read_reply()

    def pipeline(self, commands: list[tuple[str | bytes | int, ...]]) -> list[Any]:
        self.send_raw(b"".join(encode(*c) for c in commands))
        return [self.read_reply(raise_errors=False) for _ in commands]

    def _fill(self) -> None:
        chunk = self.sock.recv(1 << 16)
        if not chunk:
            raise ConnectionError("connection closed by server")
        self.buffer += chunk

    def _line(self) -> bytes:
        while b"\r\n" not in self.buffer:
            self._fill()
        line, self.buffer = self.buffer.split(b"\r\n", 1)
        return line

    def _exact(self, n: int) -> bytes:
        while len(self.buffer) < n + 2:
            self._fill()
        data, self.buffer = self.buffer[:n], self.buffer[n + 2 :]
        return data

    def read_reply(self, raise_errors: bool = True, binary: bool = False) -> Any:
        line = self._line()
        kind, rest = line[:1], line[1:]
        if kind == b"+":
            return rest.decode()
        if kind == b"-":
            error = ReplyError(rest.decode())
            if raise_errors:
                raise error
            return error
        if kind == b":":
            return int(rest)
        if kind == b"$":
            length = int(rest)
            if length < 0:
                return None
            data = self._exact(length)
            return data if binary else data.decode(errors="surrogateescape")
        if kind == b"*":
            length = int(rest)
            if length < 0:
                return None
            return [self.read_reply(raise_errors, binary) for _ in range(length)]
        raise AssertionError(f"unexpected reply line {line!r}")

    def closed_by_server(self) -> bool:
        try:
            return self.sock.recv(1) == b""
        except (ConnectionResetError, BrokenPipeError):
            return True
