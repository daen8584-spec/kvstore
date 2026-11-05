#include <chrono>
#include <string>

#include "commands.hpp"
#include "database.hpp"
#include "test.hpp"

using kv::Database;
using kv::Value;

TEST("database expires keys lazily on access") {
    Database db;
    db.set("a", Value{std::string("1")});
    db.set_expire("a", 1000);
    CHECK(db.find("a", 999) != nullptr);
    CHECK(db.find("a", 1000) == nullptr);
    CHECK_EQ(db.size(), std::size_t{0});
    CHECK_EQ(db.expired_total(), std::uint64_t{1});
}

TEST("database active expiry removes most expired keys without access") {
    Database db;
    for (int i = 0; i < 10000; ++i) {
        const std::string key = "k" + std::to_string(i);
        db.set(key, Value{std::string("v")});
        if (i % 2 == 0) db.set_expire(key, 100);
    }
    for (int round = 0; round < 50 && db.size() > 5000; ++round) {
        db.active_expire(200, std::chrono::milliseconds(5));
    }
    CHECK_EQ(db.size(), std::size_t{5000});
    CHECK_EQ(db.expires_size(), std::size_t{0});
}

TEST("database overwrite clears the expiry unless asked to keep it") {
    Database db;
    db.set("a", Value{std::string("1")});
    db.set_expire("a", kv::now_ms() + 60'000);
    db.set("a", Value{std::string("2")}, true);
    CHECK(db.expire_time("a").has_value());
    db.set("a", Value{std::string("3")});
    CHECK(!db.expire_time("a").has_value());
}

namespace {

struct Harness {
    Database db;
    std::string out;
    std::int64_t now = 1'000'000;

    std::string run(kv::Args args, std::vector<kv::Args>* records = nullptr) {
        out.clear();
        kv::ExecContext ctx{db, out, now};
        auto result = kv::execute(ctx, args);
        if (records) *records = std::move(result);
        return out;
    }
};

}  // namespace

TEST("commands report wrong types, arity and unknown commands") {
    Harness h;
    h.run({"RPUSH", "list", "a"});
    CHECK(h.run({"GET", "list"}).rfind("-WRONGTYPE", 0) == 0);
    CHECK(h.run({"GET"}).rfind("-ERR wrong number of arguments", 0) == 0);
    CHECK(h.run({"NOPE", "x"}).rfind("-ERR unknown command 'NOPE'", 0) == 0);
}

TEST("commands record relative expiry as absolute time") {
    Harness h;
    std::vector<kv::Args> records;
    CHECK_EQ(h.run({"SET", "k", "v", "EX", "10"}, &records), std::string("+OK\r\n"));
    REQUIRE(records.size() == 1);
    CHECK(records[0] == (kv::Args{"SET", "k", "v", "PXAT", "1010000"}));

    CHECK_EQ(h.run({"EXPIRE", "k", "5"}, &records), std::string(":1\r\n"));
    CHECK(records[0] == (kv::Args{"PEXPIREAT", "k", "1005000"}));

    CHECK_EQ(h.run({"EXPIRE", "k", "-1"}, &records), std::string(":1\r\n"));
    CHECK(records[0] == (kv::Args{"DEL", "k"}));
    CHECK_EQ(h.run({"EXISTS", "k"}), std::string(":0\r\n"));
}

TEST("commands do not record writes that changed nothing") {
    Harness h;
    std::vector<kv::Args> records;
    h.run({"DEL", "missing"}, &records);
    CHECK(records.empty());
    h.run({"SET", "k", "v", "XX"}, &records);
    CHECK(records.empty());
    h.run({"INCR", "n"}, &records);
    CHECK(records == (std::vector<kv::Args>{{"INCR", "n"}}));
}

TEST("commands implement SET options and INCR overflow") {
    Harness h;
    CHECK_EQ(h.run({"SET", "k", "1", "NX"}), std::string("+OK\r\n"));
    CHECK_EQ(h.run({"SET", "k", "2", "NX"}), std::string("$-1\r\n"));
    CHECK_EQ(h.run({"SET", "k", "3", "GET"}), std::string("$1\r\n1\r\n"));
    CHECK_EQ(h.run({"SET", "k", "4", "EX", "0"}),
             std::string("-ERR invalid expire time in 'set' command\r\n"));
    CHECK_EQ(h.run({"SET", "big", "9223372036854775807"}), std::string("+OK\r\n"));
    CHECK(h.run({"INCR", "big"}).rfind("-ERR increment or decrement would overflow", 0) == 0);
}

TEST("commands implement list and hash semantics") {
    Harness h;
    CHECK_EQ(h.run({"RPUSH", "l", "a", "b", "c"}), std::string(":3\r\n"));
    CHECK_EQ(h.run({"LPUSH", "l", "z"}), std::string(":4\r\n"));
    CHECK_EQ(h.run({"LRANGE", "l", "0", "-1"}),
             std::string("*4\r\n$1\r\nz\r\n$1\r\na\r\n$1\r\nb\r\n$1\r\nc\r\n"));
    CHECK_EQ(h.run({"LRANGE", "l", "-2", "100"}), std::string("*2\r\n$1\r\nb\r\n$1\r\nc\r\n"));
    CHECK_EQ(h.run({"LPOP", "l", "10"}), std::string("*4\r\n$1\r\nz\r\n$1\r\na\r\n$1\r\nb\r\n$1\r\nc\r\n"));
    CHECK_EQ(h.run({"EXISTS", "l"}), std::string(":0\r\n"));

    CHECK_EQ(h.run({"HSET", "h", "f1", "a", "f2", "b"}), std::string(":2\r\n"));
    CHECK_EQ(h.run({"HSET", "h", "f1", "c"}), std::string(":0\r\n"));
    CHECK_EQ(h.run({"HGET", "h", "f1"}), std::string("$1\r\nc\r\n"));
    CHECK_EQ(h.run({"HDEL", "h", "f1", "f2"}), std::string(":2\r\n"));
    CHECK_EQ(h.run({"TYPE", "h"}), std::string("+none\r\n"));
}
