"""Sorted set behaviour, checked against both kvstore and Redis (KVSTORE_TARGET=redis).

Scores are compared as numbers: Redis 7.0 formats some doubles with 17 significant
digits ("0.10000000000000001") while Redis 7.2 and later, and kvstore, use the shortest
representation ("0.1").
"""

import pytest

from resp_client import Client, ReplyError


def pairs(reply: list[str]) -> list[tuple[str, float]]:
    return [(member, float(score)) for member, score in zip(reply[::2], reply[1::2], strict=True)]


@pytest.fixture
def board(client: Client) -> Client:
    client.call("ZADD", "board", 30, "carol", 10, "alice", 20, "bob", 20, "betty", 50, "eve")
    return client


def test_zadd_counts_new_members_and_orders_ties_by_member(client: Client) -> None:
    assert client.call("ZADD", "z", 1, "a", 2, "b") == 2
    assert client.call("ZADD", "z", 3, "a", 4, "c") == 1
    assert client.call("ZADD", "z", "CH", 5, "a", 4, "c", 6, "d") == 2
    assert client.call("ZCARD", "z") == 4
    client.call("ZADD", "ties", 1, "b", 1, "c", 1, "a")
    assert client.call("ZRANGE", "ties", 0, -1) == ["a", "b", "c"]


def test_zadd_conditional_updates(client: Client) -> None:
    client.call("ZADD", "z", 10, "a")
    assert client.call("ZADD", "z", "NX", 99, "a", 1, "new") == 1
    assert float(client.call("ZSCORE", "z", "a")) == 10
    assert client.call("ZADD", "z", "XX", 5, "a", 1, "absent") == 0
    assert client.call("ZSCORE", "z", "absent") is None
    assert client.call("ZADD", "z", "GT", "CH", 1, "a") == 0
    assert client.call("ZADD", "z", "GT", "CH", 50, "a") == 1
    assert client.call("ZADD", "z", "LT", "CH", 60, "a") == 0
    assert float(client.call("ZSCORE", "z", "a")) == 50


def test_zadd_incr_and_zincrby(client: Client) -> None:
    assert float(client.call("ZADD", "z", "INCR", 5, "a")) == 5
    assert float(client.call("ZINCRBY", "z", 2.5, "a")) == 7.5
    assert float(client.call("ZINCRBY", "z", -10, "a")) == -2.5
    assert client.call("ZADD", "z", "NX", "INCR", 1, "a") is None


def test_zadd_rejects_invalid_input(client: Client) -> None:
    for args, message in [
        ((1, "a", 2), "syntax error"),
        (("NX", "XX", 1, "a"), "not compatible"),
        (("GT", "NX", 1, "a"), "not compatible"),
        (("INCR", 1, "a", 2, "b"), "INCR option supports a single increment-element pair"),
        (("notanumber", "a"), "not a valid float"),
        (("nan", "a"), "not a valid float"),
        (("+-5", "a"), "not a valid float"),
    ]:
        with pytest.raises(ReplyError, match=message):
            client.call("ZADD", "z", *args)
    assert client.call("EXISTS", "z") == 0


def test_infinite_scores(client: Client) -> None:
    client.call("ZADD", "z", "+inf", "top", "-inf", "bottom", 0, "middle")
    assert client.call("ZRANGE", "z", 0, -1) == ["bottom", "middle", "top"]
    assert client.call("ZSCORE", "z", "top") == "inf"
    assert client.call("ZSCORE", "z", "bottom") == "-inf"


def test_ranks(board: Client) -> None:
    assert board.call("ZRANK", "board", "alice") == 0
    assert board.call("ZRANK", "board", "bob") == 2
    assert board.call("ZREVRANK", "board", "eve") == 0
    assert board.call("ZRANK", "board", "nobody") is None
    assert board.call("ZRANK", "missing", "a") is None


def test_ranges_by_index(board: Client) -> None:
    assert board.call("ZRANGE", "board", 0, -1) == ["alice", "betty", "bob", "carol", "eve"]
    assert board.call("ZRANGE", "board", 1, 2) == ["betty", "bob"]
    assert board.call("ZRANGE", "board", -2, 100) == ["carol", "eve"]
    assert board.call("ZRANGE", "board", 3, 1) == []
    assert board.call("ZREVRANGE", "board", 0, 1) == ["eve", "carol"]
    assert board.call("ZRANGE", "board", 0, 1, "REV") == ["eve", "carol"]
    assert pairs(board.call("ZRANGE", "board", 0, 0, "WITHSCORES")) == [("alice", 10.0)]


def test_ranges_by_score(board: Client) -> None:
    assert board.call("ZRANGEBYSCORE", "board", 20, 30) == ["betty", "bob", "carol"]
    assert board.call("ZRANGEBYSCORE", "board", "(20", 30) == ["carol"]
    assert board.call("ZRANGEBYSCORE", "board", "-inf", "(20") == ["alice"]
    assert board.call("ZRANGEBYSCORE", "board", 20, "+inf", "LIMIT", 1, 2) == ["bob", "carol"]
    assert board.call("ZREVRANGEBYSCORE", "board", "+inf", 20, "LIMIT", 0, 2) == ["eve", "carol"]
    assert board.call("ZRANGE", "board", "(10", 30, "BYSCORE") == ["betty", "bob", "carol"]
    assert board.call("ZRANGE", "board", 30, 20, "BYSCORE", "REV") == ["carol", "bob", "betty"]
    assert pairs(board.call("ZRANGEBYSCORE", "board", 50, 50, "WITHSCORES")) == [("eve", 50.0)]
    assert board.call("ZRANGEBYSCORE", "board", 40, 30) == []
    with pytest.raises(ReplyError, match="min or max is not a float"):
        board.call("ZRANGEBYSCORE", "board", "low", 30)
    with pytest.raises(ReplyError, match="LIMIT is only supported"):
        board.call("ZRANGE", "board", 0, 1, "LIMIT", 0, 1)
    # BYSCORE and REV are options of ZRANGE only.
    for command in [
        ("ZREVRANGE", "board", 0, 1, "REV"),
        ("ZREVRANGE", "board", 0, 1, "BYSCORE"),
        ("ZRANGEBYSCORE", "board", 20, 30, "REV"),
        ("ZRANGEBYSCORE", "board", 20, 30, "BYSCORE"),
        ("ZREVRANGEBYSCORE", "board", 30, 20, "REV"),
    ]:
        with pytest.raises(ReplyError, match="syntax error"):
            board.call(*command)


def test_zcount(board: Client) -> None:
    assert board.call("ZCOUNT", "board", "-inf", "+inf") == 5
    assert board.call("ZCOUNT", "board", 20, 20) == 2
    assert board.call("ZCOUNT", "board", "(20", "(50") == 1
    assert board.call("ZCOUNT", "board", 100, 200) == 0


def test_zrem_and_pop(board: Client) -> None:
    assert board.call("ZREM", "board", "bob", "nobody") == 1
    assert pairs(board.call("ZPOPMIN", "board")) == [("alice", 10.0)]
    assert pairs(board.call("ZPOPMAX", "board", 2)) == [("eve", 50.0), ("carol", 30.0)]
    assert pairs(board.call("ZPOPMIN", "board", 10)) == [("betty", 20.0)]
    assert board.call("EXISTS", "board") == 0
    assert board.call("ZPOPMIN", "board") == []
    with pytest.raises(ReplyError, match="must be positive"):
        board.call("ZPOPMIN", "board", -1)


def test_type_and_wrong_type(client: Client) -> None:
    client.call("ZADD", "z", 1, "a")
    client.call("SET", "s", "v")
    assert client.call("TYPE", "z") == "zset"
    for command in (("GET", "z"), ("ZADD", "s", 1, "a"), ("ZRANGE", "s", 0, -1), ("ZSCORE", "s", "a")):
        with pytest.raises(ReplyError, match="WRONGTYPE"):
            client.call(*command)


def test_large_sorted_set_ranks_and_ranges(client: Client) -> None:
    # Enough members for many skip list levels, with repeated scores.
    for start in range(0, 20_000, 1_000):
        args: list[str | int] = []
        for i in range(start, start + 1_000):
            args += [i % 997, f"m{i:05d}"]
        client.call("ZADD", "big", *args)
    assert client.call("ZCARD", "big") == 20_000
    expected = sorted(((i % 997, f"m{i:05d}") for i in range(20_000)))
    for position in (0, 1, 999, 12_345, 19_999):
        _, member = expected[position]
        assert client.call("ZRANK", "big", member) == position
        assert client.call("ZRANGE", "big", position, position) == [member]
    assert client.call("ZCOUNT", "big", 100, 199) == sum(1 for s, _ in expected if 100 <= s <= 199)
    assert client.call("ZRANGEBYSCORE", "big", 500, 500) == [m for s, m in expected if s == 500]
