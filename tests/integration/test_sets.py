"""Set behaviour, checked against both kvstore and Redis (KVSTORE_TARGET=redis).

Set members have no order, so replies are compared as Python sets or sorted lists.
"""

import pytest

from resp_client import Client, ReplyError


@pytest.fixture
def fruit(client: Client) -> Client:
    client.call("SADD", "a", "apple", "banana", "cherry", "date")
    client.call("SADD", "b", "banana", "date", "fig")
    client.call("SADD", "c", "date", "grape")
    return client


def test_sadd_srem_scard_and_membership(client: Client) -> None:
    assert client.call("SADD", "s", "a", "b", "c", "a") == 3
    assert client.call("SADD", "s", "c", "d") == 1
    assert client.call("SCARD", "s") == 4
    assert client.call("SISMEMBER", "s", "a") == 1
    assert client.call("SISMEMBER", "s", "zzz") == 0
    assert client.call("SMISMEMBER", "s", "a", "zzz", "d") == [1, 0, 1]
    assert set(client.call("SMEMBERS", "s")) == {"a", "b", "c", "d"}
    assert client.call("TYPE", "s") == "set"
    assert client.call("SREM", "s", "a", "b", "nope") == 2
    assert client.call("SREM", "s", "c", "d") == 2
    assert client.call("EXISTS", "s") == 0


def test_missing_keys_are_empty_sets(client: Client) -> None:
    assert client.call("SCARD", "missing") == 0
    assert client.call("SISMEMBER", "missing", "a") == 0
    assert client.call("SMISMEMBER", "missing", "a", "b") == [0, 0]
    assert client.call("SMEMBERS", "missing") == []
    assert client.call("SREM", "missing", "a") == 0
    assert client.call("SPOP", "missing") is None
    assert client.call("SPOP", "missing", 2) == []
    assert client.call("SRANDMEMBER", "missing") is None
    assert client.call("SRANDMEMBER", "missing", -3) == []
    assert client.call("SSCAN", "missing", 0) == ["0", []]
    assert client.call("SSCAN", "missing", 0, "NOT-AN-OPTION") == ["0", []]


def test_wrong_type(client: Client) -> None:
    client.call("SET", "str", "x")
    client.call("SADD", "s", "a")
    for command in (
        ("SADD", "str", "a"),
        ("SREM", "str", "a"),
        ("SCARD", "str"),
        ("SISMEMBER", "str", "a"),
        ("SMEMBERS", "str"),
        ("SPOP", "str"),
        ("SRANDMEMBER", "str"),
        ("SINTER", "s", "missing", "str"),
        ("SUNION", "s", "str"),
        ("SDIFF", "missing", "str"),
        ("SINTERSTORE", "dst", "s", "str"),
        ("SINTERCARD", 1, "str"),
        ("SMOVE", "s", "str", "a"),
        ("SMOVE", "str", "s", "a"),
        ("SSCAN", "str", 0),
        ("GET", "s"),
    ):
        with pytest.raises(ReplyError, match="WRONGTYPE"):
            client.call(*command)
    assert client.call("EXISTS", "dst") == 0


def test_intersection_union_and_difference(fruit: Client) -> None:
    assert set(fruit.call("SINTER", "a", "b")) == {"banana", "date"}
    assert fruit.call("SINTER", "a", "b", "c") == ["date"]
    assert fruit.call("SINTER", "a", "missing") == []
    assert set(fruit.call("SUNION", "b", "c", "missing")) == {"banana", "date", "fig", "grape"}
    assert set(fruit.call("SDIFF", "a", "b")) == {"apple", "cherry"}
    assert set(fruit.call("SDIFF", "a", "b", "c", "missing")) == {"apple", "cherry"}
    assert fruit.call("SDIFF", "missing", "a") == []
    assert set(fruit.call("SINTER", "a")) == {"apple", "banana", "cherry", "date"}


def test_store_variants_replace_the_destination(fruit: Client) -> None:
    fruit.call("SET", "dst", "a string")
    fruit.call("EXPIRE", "dst", 100)
    assert fruit.call("SUNIONSTORE", "dst", "b", "c") == 4
    assert fruit.call("TYPE", "dst") == "set"
    assert fruit.call("TTL", "dst") == -1
    assert set(fruit.call("SMEMBERS", "dst")) == {"banana", "date", "fig", "grape"}
    assert fruit.call("SINTERSTORE", "dst", "a", "b") == 2
    assert set(fruit.call("SMEMBERS", "dst")) == {"banana", "date"}
    assert fruit.call("SDIFFSTORE", "dst", "a", "b", "c") == 2
    assert set(fruit.call("SMEMBERS", "dst")) == {"apple", "cherry"}
    # The destination may also be a source.
    assert fruit.call("SUNIONSTORE", "dst", "dst", "c") == 4
    # An empty result deletes the destination.
    assert fruit.call("SINTERSTORE", "dst", "a", "missing") == 0
    assert fruit.call("EXISTS", "dst") == 0
    assert fruit.call("SDIFFSTORE", "dst", "missing", "a") == 0
    assert fruit.call("EXISTS", "dst") == 0


def test_sintercard(fruit: Client) -> None:
    assert fruit.call("SINTERCARD", 2, "a", "b") == 2
    assert fruit.call("SINTERCARD", 3, "a", "b", "c") == 1
    assert fruit.call("SINTERCARD", 1, "a", "LIMIT", 3) == 3
    assert fruit.call("SINTERCARD", 1, "a", "LIMIT", 0) == 4
    assert fruit.call("SINTERCARD", 2, "a", "missing") == 0
    for args, message in (
        ((0, "a"), "numkeys should be greater than 0"),
        (("x", "a"), "numkeys should be greater than 0"),
        ((3, "a", "b"), "Number of keys can't be greater than number of args"),
        ((1, "a", "LIMIT", -1), "LIMIT can't be negative"),
        ((1, "a", "LIMIT", "x"), "LIMIT can't be negative"),
        ((1, "a", "other"), "syntax error"),
    ):
        with pytest.raises(ReplyError, match=message):
            fruit.call("SINTERCARD", *args)


def test_smove(fruit: Client) -> None:
    assert fruit.call("SMOVE", "a", "b", "apple") == 1
    assert fruit.call("SISMEMBER", "a", "apple") == 0
    assert fruit.call("SISMEMBER", "b", "apple") == 1
    assert fruit.call("SMOVE", "a", "b", "apple") == 0
    assert fruit.call("SMOVE", "missing", "b", "x") == 0
    # Moving within one set is a membership test.
    assert fruit.call("SMOVE", "a", "a", "banana") == 1
    assert fruit.call("SMOVE", "a", "a", "nope") == 0
    assert fruit.call("SMOVE", "c", "new", "grape") == 1
    assert fruit.call("SMOVE", "c", "new", "date") == 1
    assert fruit.call("EXISTS", "c") == 0
    assert set(fruit.call("SMEMBERS", "new")) == {"grape", "date"}


def test_spop(client: Client) -> None:
    members = {f"m{i}" for i in range(20)}
    client.call("SADD", "s", *sorted(members))
    one = client.call("SPOP", "s")
    assert one in members
    assert client.call("SISMEMBER", "s", one) == 0
    some = client.call("SPOP", "s", 5)
    assert len(set(some)) == 5 and set(some) <= members - {one}
    assert client.call("SCARD", "s") == 14
    assert client.call("SPOP", "s", 0) == []
    rest = client.call("SPOP", "s", 100)
    assert set(rest) | set(some) | {one} == members
    assert client.call("EXISTS", "s") == 0
    client.call("SADD", "s", "a")
    for args, message in (((-1,), "must be positive"), (("x",), "must be positive"), ((1, 2), "syntax error")):
        with pytest.raises(ReplyError, match=message):
            client.call("SPOP", "s", *args)


def test_srandmember(client: Client) -> None:
    members = {f"m{i}" for i in range(50)}
    client.call("SADD", "s", *sorted(members))
    assert client.call("SRANDMEMBER", "s") in members
    assert client.call("SRANDMEMBER", "s", 0) == []
    # A positive count returns distinct members, for few and for most of the set.
    for count in (3, 40):
        reply = client.call("SRANDMEMBER", "s", count)
        assert len(reply) == count and len(set(reply)) == count and set(reply) <= members
    assert set(client.call("SRANDMEMBER", "s", 500)) == members
    # A negative count may repeat members.
    client.call("SADD", "small", "x", "y")
    reply = client.call("SRANDMEMBER", "small", -20)
    assert len(reply) == 20 and set(reply) <= {"x", "y"}
    assert client.call("SCARD", "s") == 50
    with pytest.raises(ReplyError, match="not an integer"):
        client.call("SRANDMEMBER", "s", "x")
    with pytest.raises(ReplyError, match="syntax error"):
        client.call("SRANDMEMBER", "s", 1, 2)


@pytest.mark.kvstore_only
def test_srandmember_limits_repeated_members(client: Client) -> None:
    # Redis accepts any negative count and stalls building the reply; kvstore refuses
    # counts that would return more than 2^20 members.
    client.call("SADD", "s", "a")
    assert len(client.call("SRANDMEMBER", "s", -(2**20))) == 2**20
    with pytest.raises(ReplyError, match="out of range"):
        client.call("SRANDMEMBER", "s", -(2**20) - 1)


def test_sscan_returns_every_member(client: Client) -> None:
    members = {f"m:{i}" for i in range(3_000)} | {"other"}
    client.pipeline([("SADD", "s", *sorted(members)[i : i + 500]) for i in range(0, len(members), 500)])
    seen: set[str] = set()
    cursor = "0"
    while True:
        cursor, found = client.call("SSCAN", "s", cursor, "MATCH", "m:*", "COUNT", 100)
        seen.update(found)
        if cursor == "0":
            break
    assert seen == members - {"other"}
    for args, message in (
        (("x",), "invalid cursor"),
        ((0, "COUNT", 0), "syntax error"),
        ((0, "TYPE", "set"), "syntax error"),
        ((0, "MATCH"), "syntax error"),
    ):
        with pytest.raises(ReplyError, match=message):
            client.call("SSCAN", "s", *args)


def test_scan_type_set(client: Client) -> None:
    client.call("SADD", "s", "a")
    client.call("SET", "str", "x")
    assert client.call("SCAN", 0, "TYPE", "set", "COUNT", 1000) == ["0", ["s"]]


def test_large_set_grows_and_shrinks(client: Client) -> None:
    client.pipeline([("SADD", "s", *[f"{j}" for j in range(i, i + 1_000)]) for i in range(0, 20_000, 1_000)])
    assert client.call("SCARD", "s") == 20_000
    assert client.call("SISMEMBER", "s", "19999") == 1
    client.pipeline([("SREM", "s", *[f"{j}" for j in range(i, min(i + 1_000, 19_990))]) for i in range(0, 19_990, 1_000)])
    assert client.call("SCARD", "s") == 10
    assert sorted(client.call("SMEMBERS", "s"), key=int) == [f"{j}" for j in range(19_990, 20_000)]
