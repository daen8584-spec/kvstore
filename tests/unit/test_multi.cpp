#include <string>

#include "commands.hpp"
#include "database.hpp"
#include "multi.hpp"
#include "test.hpp"

using kv::Args;
using kv::Database;
using kv::Session;
using kv::Value;
using kv::Watches;

namespace {

std::string run(Database& db, Session& session, Watches& watches, const Args& args, std::int64_t now = 0) {
    std::string out;
    kv::ExecContext ctx{db, out, now};
    ctx.session = &session;
    ctx.watches = &watches;
    kv::execute(ctx, args);
    return out;
}

}  // namespace

TEST("watches mark every session watching a touched key") {
    Watches watches;
    Session a, b, c;
    watches.watch(a, "k", true);
    watches.watch(b, "k", true);
    watches.watch(c, "other", true);
    watches.touch("k");
    CHECK(a.watch_dirty);
    CHECK(b.watch_dirty);
    CHECK(!c.watch_dirty);
}

TEST("unwatching removes the session and empty keys from the registry") {
    Watches watches;
    Session a, b;
    watches.watch(a, "k", true);
    watches.watch(a, "k", true);  // duplicates are ignored
    watches.watch(a, "j", false);
    watches.watch(b, "k", true);
    CHECK_EQ(a.watched.size(), std::size_t{2});
    CHECK_EQ(watches.watched_keys(), std::size_t{2});
    watches.touch("k");
    watches.unwatch_all(a);
    CHECK(!a.watch_dirty);
    CHECK(a.watched.empty());
    CHECK_EQ(watches.watched_keys(), std::size_t{1});
    watches.unwatch_all(b);
    CHECK_EQ(watches.watched_keys(), std::size_t{0});
    watches.touch("k");  // nobody left to mark
    CHECK(!a.watch_dirty && !b.watch_dirty);
}

TEST("a watched key that existed and then vanished counts as changed") {
    Database db;
    Watches watches;
    Session session;
    db.set("k", Value{std::string("v")});
    db.set_expire("k", 1000);
    watches.watch(session, "k", true);
    watches.watch(session, "never", false);
    CHECK(!kv::watched_key_vanished(session, db, 999));
    CHECK(kv::watched_key_vanished(session, db, 1000));
}

TEST("flushall touches only watched keys that exist") {
    Database db;
    Watches watches;
    Session present, absent;
    db.set("k", Value{std::string("v")});
    watches.watch(present, "k", true);
    watches.watch(absent, "missing", false);
    watches.touch_existing(db, 0);
    CHECK(present.watch_dirty);
    CHECK(!absent.watch_dirty);
}

TEST("exec wraps a transaction's writes in MULTI and EXEC for the append-only file") {
    Database db;
    Watches watches;
    Session session;
    CHECK_EQ(run(db, session, watches, {"MULTI"}), std::string("+OK\r\n"));
    CHECK_EQ(run(db, session, watches, {"SET", "k", "v"}), std::string("+QUEUED\r\n"));
    CHECK_EQ(run(db, session, watches, {"GET", "k"}), std::string("+QUEUED\r\n"));
    std::string out;
    kv::ExecContext ctx{db, out, 0};
    ctx.session = &session;
    ctx.watches = &watches;
    const auto records = kv::execute(ctx, {"EXEC"});
    CHECK_EQ(out, std::string("*2\r\n+OK\r\n$1\r\nv\r\n"));
    REQUIRE(records.size() == 3);
    CHECK(records[0] == Args{"MULTI"});
    CHECK(records[1] == (Args{"SET", "k", "v"}));
    CHECK(records[2] == Args{"EXEC"});
    CHECK(!session.in_multi);
}

TEST("write commands touch the keys named by their key positions") {
    Database db;
    Watches watches;
    Session watcher, writer;
    watches.watch(watcher, "b", false);
    run(db, writer, watches, {"MSET", "a", "1", "b", "2"});
    CHECK(watcher.watch_dirty);

    Session values;
    watches.watch(values, "2", false);
    run(db, writer, watches, {"MSET", "1", "2", "c", "d"});  // "2" is a value, not a key
    CHECK(!values.watch_dirty);

    Session unchanged;
    run(db, writer, watches, {"SADD", "s", "m"});
    watches.watch(unchanged, "s", true);
    run(db, writer, watches, {"SADD", "s", "m"});  // no change
    CHECK(!unchanged.watch_dirty);
}

TEST("the watch filter stays correct when keys share a slot") {
    // 300,000 keys in 2^18 filter slots guarantees many shared slots.
    Watches watches;
    Session even, odd;
    for (int i = 0; i < 300'000; ++i) watches.watch(i % 2 == 0 ? even : odd, "k" + std::to_string(i), false);
    watches.unwatch_all(even);
    CHECK_EQ(watches.watched_keys(), std::size_t{150'000});
    for (int i = 0; i < 300'000; i += 2) watches.touch("k" + std::to_string(i));
    CHECK(!odd.watch_dirty);  // only unwatched keys were touched
    watches.touch("k299999");
    CHECK(odd.watch_dirty);
    watches.unwatch_all(odd);
    CHECK_EQ(watches.watched_keys(), std::size_t{0});
    Session again;
    watches.watch(again, "k1", false);
    watches.touch("k1");
    CHECK(again.watch_dirty);
}
