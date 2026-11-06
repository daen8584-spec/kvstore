import socket
import threading

import pytest

from resp_client import Client, ReplyError, encode


def test_ping_and_echo(client: Client) -> None:
    assert client.call("PING") == "PONG"
    assert client.call("PING", "hello") == "hello"
    assert client.call("ECHO", "a b") == "a b"


def test_inline_commands(client: Client) -> None:
    client.send_raw(b"SET inline value\r\nGET inline\r\n")
    assert client.read_reply() == "OK"
    assert client.read_reply() == "value"


def test_request_delivered_one_byte_at_a_time(client: Client) -> None:
    for byte in encode("SET", "slow", "x" * 100):
        client.send_raw(bytes([byte]))
    assert client.read_reply() == "OK"
    assert client.call("STRLEN", "slow") == 100


def test_pipelined_requests_are_answered_in_order(client: Client) -> None:
    commands = [("INCR", "counter") for _ in range(10_000)]
    assert client.pipeline(commands) == list(range(1, 10_001))


def test_large_binary_value_round_trip(client: Client) -> None:
    value = bytes(range(256)) * (32 * 1024)  # 8 MiB, every byte value
    assert client.call("SET", "blob", value) == "OK"
    client.send_raw(encode("GET", "blob"))
    assert client.read_reply(binary=True) == value


def test_client_that_does_not_read_replies_gets_all_of_them(client: Client) -> None:
    client.call("SET", "k", "v" * 1024)
    # About 100 MiB of replies requested before reading any: the server must pause
    # reading this client instead of buffering without limit or dropping replies.
    count = 100_000
    sender = threading.Thread(target=lambda: client.send_raw(encode("GET", "k") * count))
    sender.start()
    for _ in range(count):
        assert len(client.read_reply()) == 1024
    sender.join()


def test_unknown_command_and_wrong_arity(client: Client) -> None:
    with pytest.raises(ReplyError, match="unknown command"):
        client.call("NOSUCHCOMMAND", "a")
    with pytest.raises(ReplyError, match="wrong number of arguments"):
        client.call("GET")
    assert client.call("PING") == "PONG"


def test_client_input_in_errors_cannot_inject_replies(client: Client) -> None:
    # CR and LF from the client would end the error line early and turn the rest into
    # replies of their own.
    client.send_raw(encode("x\r\n+OK\r\n:1") + encode("PING"))
    error = client.read_reply(raise_errors=False)
    assert isinstance(error, ReplyError)
    assert "unknown command" in str(error)
    assert client.read_reply() == "PONG"

    client.send_raw(encode("SET", "k", "v") + encode("EXPIRE", "k", 10, "x\r\n+OK") + encode("PING"))
    assert client.read_reply() == "OK"
    error = client.read_reply(raise_errors=False)
    assert isinstance(error, ReplyError)
    assert "Unsupported option" in str(error)
    assert client.read_reply() == "PONG"


def test_long_unknown_command_is_not_echoed_in_full(client: Client) -> None:
    with pytest.raises(ReplyError) as raised:
        client.call("a" * 100_000, "b" * 100_000, "c" * 100_000)
    assert len(str(raised.value)) < 400
    assert client.call("PING") == "PONG"


def test_protocol_error_closes_the_connection(client: Client) -> None:
    client.send_raw(b"*1\r\n$abc\r\n")
    with pytest.raises(ReplyError, match="Protocol error"):
        client.read_reply()
    assert client.closed_by_server()


def test_concurrent_clients_see_atomic_increments(server) -> None:  # type: ignore[no-untyped-def]
    def worker() -> None:
        with server.client() as c:
            c.pipeline([("INCR", "shared") for _ in range(2_000)])

    threads = [threading.Thread(target=worker) for _ in range(20)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    with server.client() as c:
        assert c.call("GET", "shared") == "40000"


def test_disconnect_with_pending_input_does_not_affect_others(server) -> None:  # type: ignore[no-untyped-def]
    raw = socket.create_connection(("127.0.0.1", server.port))
    raw.sendall(b"*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$100\r\npartial")
    raw.close()
    with server.client() as c:
        assert c.call("PING") == "PONG"
        assert c.call("EXISTS", "k") == 0


@pytest.mark.kvstore_only
def test_maxclients_rejects_extra_connections(make_server) -> None:  # type: ignore[no-untyped-def]
    server = make_server("--maxclients", "2")
    with server.client() as a, server.client() as b:
        assert a.call("PING") == b.call("PING") == "PONG"
        with server.client() as c:
            with pytest.raises(ReplyError, match="max number of clients"):
                c.read_reply()
