"""MULTI, EXEC, DISCARD, WATCH and UNWATCH, checked against both kvstore and Redis
(KVSTORE_TARGET=redis)."""

import time
from collections.abc import Iterator

import pytest

from resp_client import Client, ReplyError


@pytest.fixture
def other(server) -> Iterator[Client]:  # type: ignore[no-untyped-def]
    """A second connection, for changes made by another client."""
    with server.client() as c:
        yield c


def test_exec_runs_queued_commands_in_order(client: Client) -> None:
    assert client.call("MULTI") == "OK"
    assert client.call("SET", "k", 1) == "QUEUED"
    assert client.call("INCR", "k") == "QUEUED"
    assert client.call("GET", "k") == "QUEUED"
    assert client.call("EXEC") == ["OK", 2, "2"]
    assert client.call("MULTI") == "OK"
    assert client.call("EXEC") == []


def test_runtime_errors_do_not_stop_the_transaction(client: Client) -> None:
    client.call("MULTI")
    client.call("SET", "k", "v")
    client.call("LPUSH", "k", "x")
    client.call("SET", "k2", "v2")
    reply = client.pipeline([("EXEC",)])[0]
    assert reply[0] == "OK" and reply[2] == "OK"
    assert isinstance(reply[1], ReplyError) and "WRONGTYPE" in str(reply[1])
    assert client.call("GET", "k2") == "v2"


def test_queueing_errors_abort_the_transaction(client: Client) -> None:
    client.call("SET", "k", "before")
    client.call("MULTI")
    with pytest.raises(ReplyError, match="wrong number of arguments for 'set'"):
        client.call("SET", "k")
    with pytest.raises(ReplyError, match="unknown command 'NOSUCH'"):
        client.call("NOSUCH", "x")
    assert client.call("SET", "k", "after") == "QUEUED"
    with pytest.raises(ReplyError, match="EXECABORT Transaction discarded because of previous errors"):
        client.call("EXEC")
    assert client.call("GET", "k") == "before"


def test_transaction_command_errors(client: Client) -> None:
    for command, message in (("EXEC", "EXEC without MULTI"), ("DISCARD", "DISCARD without MULTI")):
        with pytest.raises(ReplyError, match=message):
            client.call(command)
    with pytest.raises(ReplyError, match="wrong number of arguments for 'watch'"):
        client.call("WATCH")
    client.call("MULTI")
    # These errors are replied immediately and do not abort the transaction.
    with pytest.raises(ReplyError, match="MULTI calls can not be nested"):
        client.call("MULTI")
    with pytest.raises(ReplyError, match="WATCH inside MULTI is not allowed"):
        client.call("WATCH", "k")
    client.call("SET", "k", "v")
    assert client.call("EXEC") == ["OK"]


def test_discard(client: Client) -> None:
    client.call("MULTI")
    client.call("SET", "k", "v")
    assert client.call("DISCARD") == "OK"
    assert client.call("GET", "k") is None
    with pytest.raises(ReplyError, match="EXEC without MULTI"):
        client.call("EXEC")


def test_unwatch_is_queued_inside_multi(client: Client) -> None:
    assert client.call("UNWATCH") == "OK"
    client.call("MULTI")
    assert client.call("UNWATCH") == "QUEUED"
    assert client.call("PING") == "QUEUED"
    assert client.call("EXEC") == ["OK", "PONG"]


def test_quit_inside_multi_discards_the_transaction(client: Client, other: Client) -> None:
    client.call("MULTI")
    client.call("SET", "k", "v")
    assert client.call("QUIT") == "OK"
    assert other.call("GET", "k") is None


def test_watch_fails_exec_when_another_client_writes(client: Client, other: Client) -> None:
    client.call("SET", "k", "1")
    assert client.call("WATCH", "k") == "OK"
    other.call("SET", "k", "1")  # same value, still a modification
    client.call("MULTI")
    client.call("SET", "k", "mine")
    assert client.call("EXEC") is None
    assert client.call("GET", "k") == "1"
    # EXEC unwatches even when it fails.
    other.call("SET", "k", "2")
    client.call("MULTI")
    assert client.call("EXEC") == []


def test_watch_succeeds_when_nothing_changed(client: Client, other: Client) -> None:
    client.call("SET", "k", "1")
    client.call("SADD", "s", "m")
    client.call("WATCH", "k", "s", "missing")
    other.call("SET", "unrelated", "x")
    other.call("SADD", "s", "m")  # no change: the member exists
    other.call("DEL", "missing")  # no change: the key does not exist
    client.call("MULTI")
    client.call("INCR", "k")
    assert client.call("EXEC") == [2]


def test_watched_key_created_by_another_client(client: Client, other: Client) -> None:
    client.call("WATCH", "new")
    other.call("SET", "new", "1")
    client.call("MULTI")
    assert client.call("EXEC") is None


def test_own_writes_before_multi_count(client: Client) -> None:
    client.call("WATCH", "k")
    client.call("SET", "k", "1")
    client.call("MULTI")
    assert client.call("EXEC") is None


@pytest.mark.parametrize(
    "write",
    [
        ("EXPIRE", "k", 100),
        ("DEL", "k"),
        ("APPEND", "k", "x"),
        ("MSET", "a", "1", "k", "2"),
        ("SMOVE", "src", "k2", "m"),
        ("SUNIONSTORE", "k", "src"),
        ("SET", "unrelated", "1"),
    ],
)
def test_writes_that_touch_a_watched_key(client: Client, other: Client, write: tuple) -> None:  # type: ignore[type-arg]
    client.call("SET", "k", "1")
    client.call("SADD", "src", "m")
    watched = "k2" if write[0] == "SMOVE" else "k"
    client.call("WATCH", watched)
    other.call(*write)
    client.call("MULTI")
    expected_abort = write[1] != "unrelated"
    assert (client.call("EXEC") is None) == expected_abort


def test_store_commands_touch_only_the_destination(client: Client, other: Client) -> None:
    client.call("SADD", "src", "a")
    client.call("WATCH", "src")
    other.call("SINTERSTORE", "dst", "src")
    client.call("MULTI")
    assert client.call("EXEC") == []


def test_flushall_touches_only_existing_watched_keys(client: Client, other: Client) -> None:
    client.call("WATCH", "missing")
    other.call("FLUSHALL")
    client.call("MULTI")
    assert client.call("EXEC") == []
    client.call("SET", "e", "1")
    client.call("WATCH", "e")
    other.call("FLUSHALL")
    client.call("MULTI")
    assert client.call("EXEC") is None


def test_watched_key_that_expires_fails_exec(client: Client) -> None:
    client.call("SET", "x", "1", "PX", 100)
    client.call("SET", "y", "1", "EX", 100)
    client.call("WATCH", "y")
    client.call("MULTI")
    assert client.call("EXEC") == []
    client.call("WATCH", "x")
    time.sleep(0.2)
    client.call("MULTI")
    assert client.call("EXEC") is None


def test_discard_unwatches(client: Client, other: Client) -> None:
    client.call("WATCH", "k")
    client.call("MULTI")
    client.call("DISCARD")
    other.call("SET", "k", "1")
    client.call("MULTI")
    assert client.call("EXEC") == []


def test_closing_a_watching_client_is_safe(server, other: Client) -> None:  # type: ignore[no-untyped-def]
    for _ in range(20):
        with server.client() as c:
            c.call("WATCH", "k", "j")
            c.call("MULTI")
    other.call("SET", "k", "1")
    assert other.call("GET", "k") == "1"


def test_transactions_are_isolated_from_other_clients(client: Client, other: Client) -> None:
    client.call("MULTI")
    for i in range(1_000):
        client.call("RPUSH", "l", i)
    # Nothing has run yet.
    assert other.call("LLEN", "l") == 0
    reply = client.call("EXEC")
    assert reply[-1] == 1_000
    assert other.call("LRANGE", "l", 0, 2) == ["0", "1", "2"]
