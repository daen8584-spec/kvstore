import resource
import signal
import socket
import time

import pytest

from resp_client import ReplyError

pytestmark = pytest.mark.kvstore_only

AOF = ("--appendonly", "yes")


def populate(client, n: int = 1_000) -> None:  # type: ignore[no-untyped-def]
    client.pipeline([("SET", f"s:{i}", i) for i in range(n)])
    client.call("RPUSH", "list", "a", "b", "c")
    client.call("HSET", "hash", "f", "v")
    client.call("SET", "expiring", "v", "EX", 1000)
    client.call("INCRBY", "counter", 5)


def verify(client, n: int = 1_000) -> None:  # type: ignore[no-untyped-def]
    assert client.call("DBSIZE") == n + 4
    assert client.call("GET", f"s:{n - 1}") == str(n - 1)
    assert client.call("LRANGE", "list", 0, -1) == ["a", "b", "c"]
    assert client.call("HGET", "hash", "f") == "v"
    assert 990 <= client.call("TTL", "expiring") <= 1000
    assert client.call("GET", "counter") == "5"


def test_data_survives_a_clean_restart(make_server) -> None:  # type: ignore[no-untyped-def]
    server = make_server(*AOF)
    with server.client() as c:
        populate(c)
    server.restart()
    with server.client() as c:
        verify(c)


def test_acknowledged_writes_survive_a_crash(make_server) -> None:  # type: ignore[no-untyped-def]
    server = make_server(*AOF, "--appendfsync", "always")
    with server.client() as c:
        populate(c)
    server.stop(signal.SIGKILL)
    server.start()
    with server.client() as c:
        verify(c)


def test_expiry_is_stored_as_absolute_time(make_server) -> None:  # type: ignore[no-untyped-def]
    # Time that passes while the server is down counts towards expiry. The checks hold
    # however long the restart takes, so they do not depend on the machine's speed.
    server = make_server(*AOF)
    with server.client() as c:
        c.call("SET", "short", "v", "PX", 1500)
        c.call("SET", "long", "v", "PX", 60_000)
    server.stop()
    time.sleep(2.0)
    server.start()
    with server.client() as c:
        assert c.call("EXISTS", "short") == 0
        assert 0 < c.call("PTTL", "long") <= 58_000


def test_truncated_tail_is_discarded(make_server) -> None:  # type: ignore[no-untyped-def]
    server = make_server(*AOF)
    with server.client() as c:
        c.call("SET", "complete", "yes")
    server.stop()
    aof = server.directory / "appendonly.aof"
    intact = aof.stat().st_size
    with aof.open("ab") as f:
        f.write(b"*3\r\n$3\r\nSET\r\n$7\r\npartial\r\n$5\r\nva")  # crash in the middle of a write
    server.start()
    with server.client() as c:
        assert c.call("GET", "complete") == "yes"
        assert c.call("EXISTS", "partial") == 0
    assert aof.stat().st_size == intact
    assert "truncating" in server.log.read_text()


def test_corruption_inside_the_file_prevents_startup(make_server) -> None:  # type: ignore[no-untyped-def]
    server = make_server(*AOF)
    with server.client() as c:
        c.call("SET", "a", "1")
        c.call("SET", "b", "2")
    server.stop()
    aof = server.directory / "appendonly.aof"
    data = bytearray(aof.read_bytes())
    data[data.index(b"$1\r\na")] = ord("!")
    aof.write_bytes(bytes(data))
    assert server.start(expect_failure=True) == 1
    assert "corrupt append-only file" in server.log.read_text()


def test_rewrite_compacts_the_file_and_keeps_concurrent_writes(make_server) -> None:  # type: ignore[no-untyped-def]
    server = make_server(*AOF, "--auto-aof-rewrite-percentage", "0")
    aof = server.directory / "appendonly.aof"
    with server.client() as c:
        for _ in range(20):
            c.pipeline([("SET", f"k:{i}", "x" * 100) for i in range(5_000)])
        before = aof.stat().st_size
        assert c.call("BGREWRITEAOF").startswith("Background")
        # Writes while the child is writing the snapshot must end up in the new file.
        c.pipeline([("SET", f"during:{i}", i) for i in range(2_000)])
        deadline = time.monotonic() + 10
        while "aof_rewrite_in_progress:1" in c.call("INFO", "persistence") and time.monotonic() < deadline:
            time.sleep(0.05)
        info = c.call("INFO", "persistence")
        assert "aof_rewrites:1" in info and "aof_last_bgrewrite_status:ok" in info
        c.call("SET", "after", "1")
    assert aof.stat().st_size < before / 10
    server.restart()
    with server.client() as c:
        assert c.call("DBSIZE") == 5_000 + 2_000 + 1
        assert c.call("GET", "during:1999") == "1999"
        assert c.call("GET", "after") == "1"


def test_automatic_rewrite_after_growth(make_server) -> None:  # type: ignore[no-untyped-def]
    server = make_server(*AOF, "--auto-aof-rewrite-percentage", "100", "--auto-aof-rewrite-min-size", "100000")
    with server.client() as c:
        deadline = time.monotonic() + 10
        while "aof_rewrites:0" in c.call("INFO", "persistence") and time.monotonic() < deadline:
            c.pipeline([("SET", "same", "x" * 1000) for _ in range(200)])
            time.sleep(0.05)
        assert "aof_rewrites:0" not in c.call("INFO", "persistence")


def test_sorted_sets_survive_restart_and_rewrite(make_server) -> None:  # type: ignore[no-untyped-def]
    server = make_server(*AOF, "--auto-aof-rewrite-percentage", "0")
    with server.client() as c:
        c.call("ZADD", "z", 1.5, "a", "-inf", "low", 0.1, "tenth", 3, "c")
        c.call("ZINCRBY", "z", 2, "a")
        c.call("ZREM", "z", "c")
    server.restart()
    with server.client() as c:
        assert c.call("ZRANGE", "z", 0, -1, "WITHSCORES") == ["low", "-inf", "tenth", "0.1", "a", "3.5"]
        c.call("BGREWRITEAOF")
        deadline = time.monotonic() + 10
        while "aof_rewrite_in_progress:1" in c.call("INFO", "persistence") and time.monotonic() < deadline:
            time.sleep(0.05)
    server.restart()
    with server.client() as c:
        assert c.call("ZRANGE", "z", 0, -1, "WITHSCORES") == ["low", "-inf", "tenth", "0.1", "a", "3.5"]


def limit_file_size(limit: int):  # type: ignore[no-untyped-def]
    """Makes writes beyond `limit` bytes fail in the server, like a full disk.

    A write that crosses the limit is cut short and the next one fails with EFBIG, so this
    also covers partial writes. SIGXFSZ is ignored so the failure is reported to the
    server instead of terminating it.
    """

    def apply() -> None:
        signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
        resource.setrlimit(resource.RLIMIT_FSIZE, (limit, resource.RLIM_INFINITY))

    return apply


def test_failed_append_holds_the_reply_and_refuses_writes(make_server) -> None:  # type: ignore[no-untyped-def]
    server = make_server(*AOF, preexec=limit_file_size(4096))
    aof = server.directory / "appendonly.aof"
    with server.client() as writer, server.client() as other:
        writer.call("SET", "small", "1")
        size = aof.stat().st_size
        # Crosses the limit: part of the command is written, then the write fails.
        writer.send_raw(b"*3\r\n$3\r\nSET\r\n$3\r\nbig\r\n$8000\r\n" + b"x" * 8000 + b"\r\n")
        writer.sock.settimeout(0.5)
        with pytest.raises(socket.timeout):
            writer.read_reply()  # not acknowledged while it is not in the file
        writer.sock.settimeout(5)

        # Other clients are served, but writes are refused until the file can be written.
        with pytest.raises(ReplyError, match="MISCONF"):
            other.call("SET", "refused", "1")
        assert other.call("GET", "small") == "1"
        assert "aof_last_write_status:err" in other.call("INFO", "persistence")
        assert aof.stat().st_size == size  # the short write was cut back

        # Space becomes available: the held write goes through and is acknowledged.
        # Raising another process's limit is not permitted everywhere (Android denies
        # it); there the test checks the restart below instead.
        assert server.process is not None
        unlimited = (resource.RLIM_INFINITY, resource.RLIM_INFINITY)
        try:
            resource.prlimit(server.process.pid, resource.RLIMIT_FSIZE, unlimited)
            recovered = True
        except PermissionError:
            recovered = False
        if recovered:
            assert writer.read_reply() == "OK"
            assert "aof_last_write_status:ok" in other.call("INFO", "persistence")
            assert other.call("SET", "after", "1") == "OK"

    # Restarting without the limit loads a file with no partial command in it.
    server.preexec = None
    server.restart()
    with server.client() as c:
        assert c.call("GET", "small") == "1"
        assert c.call("EXISTS", "refused") == 0
        if recovered:
            assert c.call("STRLEN", "big") == 8000
            assert c.call("GET", "after") == "1"
        else:
            assert c.call("EXISTS", "big") == 0  # never acknowledged, never written
