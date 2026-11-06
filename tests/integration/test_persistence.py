import signal
import time

import pytest

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
    server = make_server(*AOF)
    with server.client() as c:
        c.call("SET", "k", "v", "PX", 1500)
    time.sleep(1.0)
    server.restart()
    with server.client() as c:
        assert 0 < c.call("PTTL", "k") <= 500
    time.sleep(0.6)
    with server.client() as c:
        assert c.call("GET", "k") is None


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
