import time

import pytest

from resp_client import Client, ReplyError


def test_strings(client: Client) -> None:
    assert client.call("GET", "missing") is None
    assert client.call("SET", "k", "v") == "OK"
    assert client.call("SET", "k", "w", "NX") is None
    assert client.call("SET", "k", "w", "XX", "GET") == "v"
    assert client.call("APPEND", "k", "xyz") == 4
    assert client.call("MSET", "a", "1", "b", "2") == "OK"
    assert client.call("MGET", "a", "b", "missing") == ["1", "2", None]
    assert client.call("INCRBY", "a", 41) == 42
    assert client.call("DECR", "a") == 41
    with pytest.raises(ReplyError, match="not an integer"):
        client.call("INCR", "k")


def test_lists(client: Client) -> None:
    assert client.call("RPUSH", "l", "a", "b", "c") == 3
    assert client.call("LPUSH", "l", "z") == 4
    assert client.call("LRANGE", "l", 0, -1) == ["z", "a", "b", "c"]
    assert client.call("LRANGE", "l", 1, 2) == ["a", "b"]
    assert client.call("LRANGE", "l", -100, 100) == ["z", "a", "b", "c"]
    assert client.call("RPOP", "l") == "c"
    assert client.call("LPOP", "l", 2) == ["z", "a"]
    assert client.call("LLEN", "l") == 1
    assert client.call("LPOP", "l") == "b"
    assert client.call("EXISTS", "l") == 0
    assert client.call("LPOP", "l") is None


def test_hashes(client: Client) -> None:
    assert client.call("HSET", "h", "f1", "a", "f2", "b") == 2
    assert client.call("HSET", "h", "f1", "c", "f3", "d") == 1
    assert client.call("HGET", "h", "f1") == "c"
    assert client.call("HLEN", "h") == 3
    assert client.call("HEXISTS", "h", "f2") == 1
    reply = client.call("HGETALL", "h")
    assert dict(zip(reply[::2], reply[1::2])) == {"f1": "c", "f2": "b", "f3": "d"}
    assert client.call("HDEL", "h", "f1", "f2", "f3", "nope") == 3
    assert client.call("TYPE", "h") == "none"


def test_wrong_type(client: Client) -> None:
    client.call("RPUSH", "l", "a")
    for command in (("GET", "l"), ("HGET", "l", "f"), ("INCR", "l"), ("APPEND", "l", "x")):
        with pytest.raises(ReplyError, match="WRONGTYPE"):
            client.call(*command)


def test_keys_type_and_delete(client: Client) -> None:
    client.call("MSET", "user:1", "a", "user:2", "b", "session:1", "c")
    client.call("RPUSH", "queue", "x")
    assert sorted(client.call("KEYS", "user:*")) == ["user:1", "user:2"]
    assert client.call("TYPE", "queue") == "list"
    assert client.call("EXISTS", "user:1", "user:1", "missing") == 2
    assert client.call("DEL", "user:1", "user:2", "missing") == 2
    assert client.call("DBSIZE") == 2


def test_scan_returns_every_key(client: Client) -> None:
    client.pipeline([("SET", f"key:{i}", i) for i in range(5_000)])
    client.call("RPUSH", "list:1", "x")
    seen: set[str] = set()
    cursor = "0"
    while True:
        cursor, keys = client.call("SCAN", cursor, "MATCH", "key:*", "COUNT", 100)
        seen.update(keys)
        if cursor == "0":
            break
    assert seen == {f"key:{i}" for i in range(5_000)}
    _, lists = client.call("SCAN", 0, "TYPE", "list", "COUNT", 10_000)
    assert lists == ["list:1"]


def test_ttl_lifecycle(client: Client) -> None:
    client.call("SET", "k", "v")
    assert client.call("TTL", "k") == -1
    assert client.call("TTL", "missing") == -2
    assert client.call("EXPIRE", "k", 100) == 1
    assert 99 <= client.call("TTL", "k") <= 100
    assert client.call("EXPIRE", "k", 50, "GT") == 0
    assert client.call("EXPIRE", "k", 50, "LT") == 1
    assert client.call("PERSIST", "k") == 1
    assert client.call("TTL", "k") == -1
    assert client.call("SET", "k", "v", "PX", 100_000) == "OK"
    assert client.call("SET", "k", "w", "KEEPTTL") == "OK"
    assert client.call("PTTL", "k") > 90_000
    assert client.call("SET", "k", "x") == "OK"
    assert client.call("TTL", "k") == -1


def test_expired_keys_disappear_on_access(client: Client) -> None:
    client.call("SET", "short", "v", "PX", 50)
    time.sleep(0.1)
    assert client.call("GET", "short") is None
    assert client.call("EXISTS", "short") == 0


def test_active_expiry_removes_keys_nobody_reads(client: Client) -> None:
    client.pipeline([("SET", f"temp:{i}", "v", "PX", 100) for i in range(2_000)])
    client.call("SET", "keep", "v")
    deadline = time.monotonic() + 5
    while client.call("DBSIZE") > 1 and time.monotonic() < deadline:
        time.sleep(0.1)
    assert client.call("DBSIZE") == 1


def test_negative_expire_deletes_the_key(client: Client) -> None:
    client.call("SET", "k", "v")
    assert client.call("EXPIRE", "k", -1) == 1
    assert client.call("EXISTS", "k") == 0


def test_flushall(client: Client) -> None:
    client.call("MSET", "a", "1", "b", "2")
    assert client.call("FLUSHALL") == "OK"
    assert client.call("DBSIZE") == 0
