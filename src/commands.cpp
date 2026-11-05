#include "commands.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <limits>
#include <optional>
#include <string>

#include "glob.hpp"
#include "resp.hpp"

namespace kv {
namespace {

constexpr std::string_view kWrongType = "WRONGTYPE Operation against a key holding the wrong kind of value";
constexpr std::string_view kNotInteger = "ERR value is not an integer or out of range";
constexpr std::string_view kSyntax = "ERR syntax error";

bool iequals(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) ==
                      std::tolower(static_cast<unsigned char>(y));
           });
}

bool parse_int(std::string_view text, std::int64_t& value) {
    if (text.empty() || text.size() > 20) return false;
    auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    return ec == std::errc{} && ptr == text.data() + text.size();
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

template <class T>
T* as(Value* value) {
    return value ? std::get_if<T>(value) : nullptr;
}

// ---- connection and server ----

void cmd_ping(ExecContext& ctx, const Args& args) {
    if (args.size() > 2) return resp::error(ctx.out, "ERR wrong number of arguments for 'ping' command");
    if (args.size() == 2) return resp::bulk(ctx.out, args[1]);
    resp::simple_string(ctx.out, "PONG");
}

void cmd_echo(ExecContext& ctx, const Args& args) {
    resp::bulk(ctx.out, args[1]);
}

void cmd_quit(ExecContext& ctx, const Args&) {
    ctx.out += resp::kOk;
    ctx.close_connection = true;
}

void cmd_select(ExecContext& ctx, const Args& args) {
    std::int64_t index = 0;
    if (!parse_int(args[1], index)) return resp::error(ctx.out, kNotInteger);
    if (index != 0) return resp::error(ctx.out, "ERR DB index is out of range");
    ctx.out += resp::kOk;
}

void cmd_command(ExecContext& ctx, const Args& args) {
    // Clients such as redis-cli query command metadata on connect. An empty reply tells
    // them no metadata is available, which they handle.
    if (args.size() >= 2 && iequals(args[1], "count")) {
        return resp::integer(ctx.out, static_cast<std::int64_t>(command_count()));
    }
    resp::array_header(ctx.out, 0);
}

void cmd_config(ExecContext& ctx, const Args& args) {
    if (iequals(args[1], "get") && args.size() == 3) {
        auto pairs = ctx.server ? ctx.server->config(args[2]) : decltype(ctx.server->config(""))();
        resp::array_header(ctx.out, pairs.size() * 2);
        for (const auto& [name, value] : pairs) {
            resp::bulk(ctx.out, name);
            resp::bulk(ctx.out, value);
        }
        return;
    }
    resp::error(ctx.out, "ERR only CONFIG GET <parameter> is supported");
}

void cmd_info(ExecContext& ctx, const Args& args) {
    const std::string section = args.size() > 1 ? lower(args[1]) : "default";
    resp::bulk(ctx.out, ctx.server ? ctx.server->info(section) : std::string());
}

void cmd_time(ExecContext& ctx, const Args&) {
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    resp::array_header(ctx.out, 2);
    resp::bulk(ctx.out, std::to_string(micros / 1'000'000));
    resp::bulk(ctx.out, std::to_string(micros % 1'000'000));
}

void cmd_bgrewriteaof(ExecContext& ctx, const Args&) {
    const std::string failure = ctx.server ? ctx.server->start_aof_rewrite() : "ERR not available";
    if (!failure.empty()) return resp::error(ctx.out, failure);
    resp::simple_string(ctx.out, "Background append only file rewriting started");
}

void cmd_dbsize(ExecContext& ctx, const Args&) {
    resp::integer(ctx.out, static_cast<std::int64_t>(ctx.db.size()));
}

void cmd_flushall(ExecContext& ctx, const Args& args) {
    if (args.size() > 2 || (args.size() == 2 && !iequals(args[1], "sync") && !iequals(args[1], "async"))) {
        return resp::error(ctx.out, kSyntax);
    }
    ctx.db.clear();
    ctx.dirty = true;
    ctx.propagate = {{"FLUSHALL"}};
    ctx.out += resp::kOk;
}

// ---- keys ----

void cmd_del(ExecContext& ctx, const Args& args) {
    std::int64_t removed = 0;
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (ctx.db.find(args[i], ctx.now) && ctx.db.erase(args[i])) ++removed;
    }
    ctx.dirty = removed > 0;
    resp::integer(ctx.out, removed);
}

void cmd_exists(ExecContext& ctx, const Args& args) {
    std::int64_t count = 0;
    for (std::size_t i = 1; i < args.size(); ++i) count += ctx.db.find(args[i], ctx.now) ? 1 : 0;
    resp::integer(ctx.out, count);
}

void cmd_type(ExecContext& ctx, const Args& args) {
    Value* value = ctx.db.find(args[1], ctx.now);
    resp::simple_string(ctx.out, value ? type_name(*value) : "none");
}

void cmd_keys(ExecContext& ctx, const Args& args) {
    std::vector<std::string_view> matched;
    const std::string_view pattern = args[1];
    const bool all = pattern == "*";
    ctx.db.keys().for_each([&](const std::string& key, const Value&) {
        const auto when = ctx.db.expire_time(key);
        if (when && *when <= ctx.now) return;
        if (all || glob_match(pattern, key)) matched.push_back(key);
    });
    resp::array_header(ctx.out, matched.size());
    for (std::string_view key : matched) resp::bulk(ctx.out, key);
}

void cmd_scan(ExecContext& ctx, const Args& args) {
    std::uint64_t cursor = 0;
    {
        const std::string& text = args[1];
        auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), cursor);
        if (ec != std::errc{} || ptr != text.data() + text.size()) {
            return resp::error(ctx.out, "ERR invalid cursor");
        }
    }
    std::string_view pattern;
    std::string_view type;
    std::int64_t count = 10;
    for (std::size_t i = 2; i < args.size(); ++i) {
        if (i + 1 >= args.size()) return resp::error(ctx.out, kSyntax);
        if (iequals(args[i], "match")) {
            pattern = args[++i];
        } else if (iequals(args[i], "count")) {
            if (!parse_int(args[++i], count)) return resp::error(ctx.out, kNotInteger);
            if (count < 1) return resp::error(ctx.out, kSyntax);
        } else if (iequals(args[i], "type")) {
            type = args[++i];
        } else {
            return resp::error(ctx.out, kSyntax);
        }
    }

    std::vector<std::string> found;
    // Like Redis, COUNT bounds the work per call rather than the result size: each call
    // visits buckets until it has seen about COUNT keys, or ten times that many buckets.
    std::int64_t budget = std::min<std::int64_t>(count, 1'000'000) * 10;
    do {
        cursor = ctx.db.keys().scan(cursor, [&](const std::string& key, const Value& value) {
            const auto when = ctx.db.expire_time(key);
            if (when && *when <= ctx.now) return;
            if (!type.empty() && !iequals(type, type_name(value))) return;
            if (!pattern.empty() && !glob_match(pattern, key)) return;
            found.push_back(key);
        });
    } while (cursor != 0 && static_cast<std::int64_t>(found.size()) < count && --budget > 0);

    resp::array_header(ctx.out, 2);
    resp::bulk(ctx.out, std::to_string(cursor));
    resp::array_header(ctx.out, found.size());
    for (const std::string& key : found) resp::bulk(ctx.out, key);
}

// EXPIRE, PEXPIRE, EXPIREAT and PEXPIREAT. Every form is recorded in the append-only file
// as PEXPIREAT with an absolute time, so replaying it later gives the same expiry.
void expire_generic(ExecContext& ctx, const Args& args, std::int64_t unit_ms, bool absolute) {
    std::int64_t amount = 0;
    if (!parse_int(args[2], amount)) return resp::error(ctx.out, kNotInteger);
    bool nx = false, xx = false, gt = false, lt = false;
    for (std::size_t i = 3; i < args.size(); ++i) {
        if (iequals(args[i], "nx"))
            nx = true;
        else if (iequals(args[i], "xx"))
            xx = true;
        else if (iequals(args[i], "gt"))
            gt = true;
        else if (iequals(args[i], "lt"))
            lt = true;
        else
            return resp::error(ctx.out, "ERR Unsupported option " + args[i]);
    }
    if ((nx && (xx || gt || lt)) || (gt && lt)) {
        return resp::error(ctx.out, "ERR NX and XX, GT or LT options at the same time are not compatible");
    }
    std::int64_t when = 0;
    if (__builtin_mul_overflow(amount, unit_ms, &when) ||
        (!absolute && __builtin_add_overflow(when, ctx.now, &when))) {
        return resp::error(ctx.out, "ERR invalid expire time in '" + lower(args[0]) + "' command");
    }

    if (!ctx.db.find(args[1], ctx.now)) return resp::integer(ctx.out, 0);
    const auto current = ctx.db.expire_time(args[1]);
    // A key without an expiry counts as an infinite TTL for GT and LT.
    if ((nx && current) || (xx && !current) || (gt && (!current || when <= *current)) ||
        (lt && current && when >= *current)) {
        return resp::integer(ctx.out, 0);
    }

    ctx.dirty = true;
    if (when <= ctx.now) {
        ctx.db.erase(args[1]);
        ctx.propagate = {{"DEL", args[1]}};
    } else {
        ctx.db.set_expire(args[1], when);
        ctx.propagate = {{"PEXPIREAT", args[1], std::to_string(when)}};
    }
    resp::integer(ctx.out, 1);
}

void cmd_expire(ExecContext& ctx, const Args& args) {
    expire_generic(ctx, args, 1000, false);
}
void cmd_pexpire(ExecContext& ctx, const Args& args) {
    expire_generic(ctx, args, 1, false);
}
void cmd_expireat(ExecContext& ctx, const Args& args) {
    expire_generic(ctx, args, 1000, true);
}
void cmd_pexpireat(ExecContext& ctx, const Args& args) {
    expire_generic(ctx, args, 1, true);
}

void ttl_generic(ExecContext& ctx, const Args& args, bool millis) {
    if (!ctx.db.find(args[1], ctx.now)) return resp::integer(ctx.out, -2);
    const auto when = ctx.db.expire_time(args[1]);
    if (!when) return resp::integer(ctx.out, -1);
    const std::int64_t remaining = std::max<std::int64_t>(*when - ctx.now, 0);
    resp::integer(ctx.out, millis ? remaining : (remaining + 500) / 1000);
}

void cmd_ttl(ExecContext& ctx, const Args& args) {
    ttl_generic(ctx, args, false);
}
void cmd_pttl(ExecContext& ctx, const Args& args) {
    ttl_generic(ctx, args, true);
}

void cmd_persist(ExecContext& ctx, const Args& args) {
    const bool removed = ctx.db.find(args[1], ctx.now) && ctx.db.persist(args[1]);
    ctx.dirty = removed;
    resp::integer(ctx.out, removed ? 1 : 0);
}

// ---- strings ----

void cmd_get(ExecContext& ctx, const Args& args) {
    Value* value = ctx.db.find(args[1], ctx.now);
    if (!value) return resp::null_bulk(ctx.out);
    auto* text = std::get_if<std::string>(value);
    if (!text) return resp::error(ctx.out, kWrongType);
    resp::bulk(ctx.out, *text);
}

void cmd_set(ExecContext& ctx, const Args& args) {
    bool nx = false, xx = false, keep_ttl = false, get = false;
    std::optional<std::int64_t> when;
    for (std::size_t i = 3; i < args.size(); ++i) {
        const std::string& option = args[i];
        if (iequals(option, "nx"))
            nx = true;
        else if (iequals(option, "xx"))
            xx = true;
        else if (iequals(option, "keepttl"))
            keep_ttl = true;
        else if (iequals(option, "get"))
            get = true;
        else if (iequals(option, "ex") || iequals(option, "px") || iequals(option, "exat") ||
                 iequals(option, "pxat")) {
            if (when || i + 1 >= args.size()) return resp::error(ctx.out, kSyntax);
            std::int64_t amount = 0;
            if (!parse_int(args[++i], amount)) return resp::error(ctx.out, kNotInteger);
            if (amount <= 0) return resp::error(ctx.out, "ERR invalid expire time in 'set' command");
            const bool seconds = iequals(option, "ex") || iequals(option, "exat");
            const bool absolute = iequals(option, "exat") || iequals(option, "pxat");
            std::int64_t ms = 0;
            if (__builtin_mul_overflow(amount, seconds ? 1000 : 1, &ms) ||
                (!absolute && __builtin_add_overflow(ms, ctx.now, &ms))) {
                return resp::error(ctx.out, "ERR invalid expire time in 'set' command");
            }
            when = ms;
        } else {
            return resp::error(ctx.out, kSyntax);
        }
    }
    if ((nx && xx) || (keep_ttl && when)) return resp::error(ctx.out, kSyntax);

    Value* existing = ctx.db.find(args[1], ctx.now);
    if (get && existing && !std::holds_alternative<std::string>(*existing)) {
        return resp::error(ctx.out, kWrongType);
    }
    std::optional<std::string> previous;
    if (get && existing) previous = std::get<std::string>(*existing);

    const bool allowed = !((nx && existing) || (xx && !existing));
    if (allowed) {
        ctx.db.set(args[1], Value{args[2]}, keep_ttl);
        if (when) ctx.db.set_expire(args[1], *when);
        ctx.dirty = true;
        Args record{"SET", args[1], args[2]};
        if (when) {
            record.insert(record.end(), {"PXAT", std::to_string(*when)});
        } else if (keep_ttl) {
            record.emplace_back("KEEPTTL");
        }
        ctx.propagate = {std::move(record)};
    }

    if (get) {
        if (previous) return resp::bulk(ctx.out, *previous);
        return resp::null_bulk(ctx.out);
    }
    if (!allowed) return resp::null_bulk(ctx.out);
    ctx.out += resp::kOk;
}

void cmd_mget(ExecContext& ctx, const Args& args) {
    resp::array_header(ctx.out, args.size() - 1);
    for (std::size_t i = 1; i < args.size(); ++i) {
        auto* text = as<std::string>(ctx.db.find(args[i], ctx.now));
        if (text)
            resp::bulk(ctx.out, *text);
        else
            resp::null_bulk(ctx.out);
    }
}

void cmd_mset(ExecContext& ctx, const Args& args) {
    if (args.size() % 2 == 0) return resp::error(ctx.out, "ERR wrong number of arguments for 'mset' command");
    for (std::size_t i = 1; i < args.size(); i += 2) ctx.db.set(args[i], Value{args[i + 1]});
    ctx.dirty = true;
    ctx.out += resp::kOk;
}

void incr_generic(ExecContext& ctx, const std::string& key, std::int64_t delta) {
    Value* value = ctx.db.find(key, ctx.now);
    std::int64_t current = 0;
    if (value) {
        auto* text = std::get_if<std::string>(value);
        if (!text) return resp::error(ctx.out, kWrongType);
        if (!parse_int(*text, current)) return resp::error(ctx.out, kNotInteger);
    }
    std::int64_t result = 0;
    if (__builtin_add_overflow(current, delta, &result)) {
        return resp::error(ctx.out, "ERR increment or decrement would overflow");
    }
    if (value)
        std::get<std::string>(*value) = std::to_string(result);
    else
        ctx.db.set(key, Value{std::to_string(result)});
    ctx.dirty = true;
    resp::integer(ctx.out, result);
}

void cmd_incr(ExecContext& ctx, const Args& args) {
    incr_generic(ctx, args[1], 1);
}
void cmd_decr(ExecContext& ctx, const Args& args) {
    incr_generic(ctx, args[1], -1);
}

void cmd_incrby(ExecContext& ctx, const Args& args) {
    std::int64_t delta = 0;
    if (!parse_int(args[2], delta)) return resp::error(ctx.out, kNotInteger);
    incr_generic(ctx, args[1], delta);
}

void cmd_decrby(ExecContext& ctx, const Args& args) {
    std::int64_t delta = 0;
    if (!parse_int(args[2], delta) || delta == std::numeric_limits<std::int64_t>::min()) {
        return resp::error(ctx.out, kNotInteger);
    }
    incr_generic(ctx, args[1], -delta);
}

void cmd_append(ExecContext& ctx, const Args& args) {
    Value& value = ctx.db.find_or_create(args[1], ctx.now, [] { return Value{std::string()}; });
    auto* text = std::get_if<std::string>(&value);
    if (!text) return resp::error(ctx.out, kWrongType);
    *text += args[2];
    ctx.dirty = true;
    resp::integer(ctx.out, static_cast<std::int64_t>(text->size()));
}

void cmd_strlen(ExecContext& ctx, const Args& args) {
    Value* value = ctx.db.find(args[1], ctx.now);
    if (!value) return resp::integer(ctx.out, 0);
    auto* text = std::get_if<std::string>(value);
    if (!text) return resp::error(ctx.out, kWrongType);
    resp::integer(ctx.out, static_cast<std::int64_t>(text->size()));
}

// ---- lists ----

void push_generic(ExecContext& ctx, const Args& args, bool left) {
    Value* value = ctx.db.find(args[1], ctx.now);
    if (value && !std::holds_alternative<List>(*value)) return resp::error(ctx.out, kWrongType);
    if (!value) value = &ctx.db.set(args[1], Value{List{}});
    auto& list = std::get<List>(*value);
    for (std::size_t i = 2; i < args.size(); ++i) {
        if (left)
            list.push_front(args[i]);
        else
            list.push_back(args[i]);
    }
    ctx.dirty = true;
    resp::integer(ctx.out, static_cast<std::int64_t>(list.size()));
}

void cmd_lpush(ExecContext& ctx, const Args& args) {
    push_generic(ctx, args, true);
}
void cmd_rpush(ExecContext& ctx, const Args& args) {
    push_generic(ctx, args, false);
}

void pop_generic(ExecContext& ctx, const Args& args, bool left) {
    if (args.size() > 3) return resp::error(ctx.out, kSyntax);
    std::int64_t count = 1;
    const bool with_count = args.size() == 3;
    if (with_count && (!parse_int(args[2], count) || count < 0)) {
        return resp::error(ctx.out, "ERR value is out of range, must be positive");
    }
    Value* value = ctx.db.find(args[1], ctx.now);
    if (!value) return with_count ? resp::null_array(ctx.out) : resp::null_bulk(ctx.out);
    auto* list = std::get_if<List>(value);
    if (!list) return resp::error(ctx.out, kWrongType);

    const auto n = std::min<std::size_t>(static_cast<std::size_t>(count), list->size());
    if (with_count) resp::array_header(ctx.out, n);
    for (std::size_t i = 0; i < n; ++i) {
        if (left) {
            resp::bulk(ctx.out, list->front());
            list->pop_front();
        } else {
            resp::bulk(ctx.out, list->back());
            list->pop_back();
        }
    }
    if (n > 0) ctx.dirty = true;
    if (list->empty()) ctx.db.erase(args[1]);
}

void cmd_lpop(ExecContext& ctx, const Args& args) {
    pop_generic(ctx, args, true);
}
void cmd_rpop(ExecContext& ctx, const Args& args) {
    pop_generic(ctx, args, false);
}

void cmd_llen(ExecContext& ctx, const Args& args) {
    Value* value = ctx.db.find(args[1], ctx.now);
    if (!value) return resp::integer(ctx.out, 0);
    auto* list = std::get_if<List>(value);
    if (!list) return resp::error(ctx.out, kWrongType);
    resp::integer(ctx.out, static_cast<std::int64_t>(list->size()));
}

void cmd_lrange(ExecContext& ctx, const Args& args) {
    std::int64_t start = 0, stop = 0;
    if (!parse_int(args[2], start) || !parse_int(args[3], stop)) return resp::error(ctx.out, kNotInteger);
    Value* value = ctx.db.find(args[1], ctx.now);
    if (!value) return resp::array_header(ctx.out, 0);
    auto* list = std::get_if<List>(value);
    if (!list) return resp::error(ctx.out, kWrongType);

    const auto size = static_cast<std::int64_t>(list->size());
    if (start < 0) start = std::max<std::int64_t>(size + start, 0);
    if (stop < 0) stop = size + stop;
    stop = std::min(stop, size - 1);
    if (start > stop || start >= size) return resp::array_header(ctx.out, 0);
    resp::array_header(ctx.out, static_cast<std::size_t>(stop - start + 1));
    for (auto i = start; i <= stop; ++i) resp::bulk(ctx.out, (*list)[static_cast<std::size_t>(i)]);
}

// ---- hashes ----

void cmd_hset(ExecContext& ctx, const Args& args) {
    if (args.size() % 2 != 0) return resp::error(ctx.out, "ERR wrong number of arguments for 'hset' command");
    Value* value = ctx.db.find(args[1], ctx.now);
    if (value && !std::holds_alternative<Hash>(*value)) return resp::error(ctx.out, kWrongType);
    if (!value) value = &ctx.db.set(args[1], Value{Hash{}});
    auto& hash = std::get<Hash>(*value);
    std::int64_t added = 0;
    for (std::size_t i = 2; i < args.size(); i += 2) {
        auto [slot, inserted] = hash.try_emplace(args[i], args[i + 1]);
        if (inserted)
            ++added;
        else
            *slot = args[i + 1];
    }
    ctx.dirty = true;
    resp::integer(ctx.out, added);
}

void cmd_hget(ExecContext& ctx, const Args& args) {
    Value* value = ctx.db.find(args[1], ctx.now);
    if (!value) return resp::null_bulk(ctx.out);
    auto* hash = std::get_if<Hash>(value);
    if (!hash) return resp::error(ctx.out, kWrongType);
    const std::string* field = hash->find(args[2]);
    if (field)
        resp::bulk(ctx.out, *field);
    else
        resp::null_bulk(ctx.out);
}

void cmd_hdel(ExecContext& ctx, const Args& args) {
    Value* value = ctx.db.find(args[1], ctx.now);
    if (!value) return resp::integer(ctx.out, 0);
    auto* hash = std::get_if<Hash>(value);
    if (!hash) return resp::error(ctx.out, kWrongType);
    std::int64_t removed = 0;
    for (std::size_t i = 2; i < args.size(); ++i) removed += hash->erase(args[i]) ? 1 : 0;
    if (hash->empty()) ctx.db.erase(args[1]);
    ctx.dirty = removed > 0;
    resp::integer(ctx.out, removed);
}

void cmd_hgetall(ExecContext& ctx, const Args& args) {
    Value* value = ctx.db.find(args[1], ctx.now);
    if (!value) return resp::array_header(ctx.out, 0);
    auto* hash = std::get_if<Hash>(value);
    if (!hash) return resp::error(ctx.out, kWrongType);
    resp::array_header(ctx.out, hash->size() * 2);
    hash->for_each([&](const std::string& field, const std::string& v) {
        resp::bulk(ctx.out, field);
        resp::bulk(ctx.out, v);
    });
}

void cmd_hlen(ExecContext& ctx, const Args& args) {
    Value* value = ctx.db.find(args[1], ctx.now);
    if (!value) return resp::integer(ctx.out, 0);
    auto* hash = std::get_if<Hash>(value);
    if (!hash) return resp::error(ctx.out, kWrongType);
    resp::integer(ctx.out, static_cast<std::int64_t>(hash->size()));
}

void cmd_hexists(ExecContext& ctx, const Args& args) {
    Value* value = ctx.db.find(args[1], ctx.now);
    if (!value) return resp::integer(ctx.out, 0);
    auto* hash = std::get_if<Hash>(value);
    if (!hash) return resp::error(ctx.out, kWrongType);
    resp::integer(ctx.out, hash->find(args[2]) ? 1 : 0);
}

// clang-format off
constexpr std::array kCommands = {
    Command{"append", 3, true, cmd_append},
    Command{"bgrewriteaof", 1, false, cmd_bgrewriteaof},
    Command{"command", -1, false, cmd_command},
    Command{"config", -2, false, cmd_config},
    Command{"dbsize", 1, false, cmd_dbsize},
    Command{"decr", 2, true, cmd_decr},
    Command{"decrby", 3, true, cmd_decrby},
    Command{"del", -2, true, cmd_del},
    Command{"echo", 2, false, cmd_echo},
    Command{"exists", -2, false, cmd_exists},
    Command{"expire", -3, true, cmd_expire},
    Command{"expireat", -3, true, cmd_expireat},
    Command{"flushall", -1, true, cmd_flushall},
    Command{"flushdb", -1, true, cmd_flushall},
    Command{"get", 2, false, cmd_get},
    Command{"hdel", -3, true, cmd_hdel},
    Command{"hexists", 3, false, cmd_hexists},
    Command{"hget", 3, false, cmd_hget},
    Command{"hgetall", 2, false, cmd_hgetall},
    Command{"hlen", 2, false, cmd_hlen},
    Command{"hset", -4, true, cmd_hset},
    Command{"incr", 2, true, cmd_incr},
    Command{"incrby", 3, true, cmd_incrby},
    Command{"info", -1, false, cmd_info},
    Command{"keys", 2, false, cmd_keys},
    Command{"llen", 2, false, cmd_llen},
    Command{"lpop", -2, true, cmd_lpop},
    Command{"lpush", -3, true, cmd_lpush},
    Command{"lrange", 4, false, cmd_lrange},
    Command{"mget", -2, false, cmd_mget},
    Command{"mset", -3, true, cmd_mset},
    Command{"persist", 2, true, cmd_persist},
    Command{"pexpire", -3, true, cmd_pexpire},
    Command{"pexpireat", -3, true, cmd_pexpireat},
    Command{"ping", -1, false, cmd_ping},
    Command{"pttl", 2, false, cmd_pttl},
    Command{"quit", -1, false, cmd_quit},
    Command{"rpop", -2, true, cmd_rpop},
    Command{"rpush", -3, true, cmd_rpush},
    Command{"scan", -2, false, cmd_scan},
    Command{"select", 2, false, cmd_select},
    Command{"set", -3, true, cmd_set},
    Command{"strlen", 2, false, cmd_strlen},
    Command{"time", 1, false, cmd_time},
    Command{"ttl", 2, false, cmd_ttl},
    Command{"type", 2, false, cmd_type},
    Command{"unlink", -2, true, cmd_del},
};
// clang-format on

static_assert(std::is_sorted(kCommands.begin(), kCommands.end(),
                             [](const Command& a, const Command& b) { return a.name < b.name; }),
              "kCommands must be sorted by name for binary search");

}  // namespace

const Command* find_command(std::string_view name) {
    if (name.size() > 16) return nullptr;
    char buffer[16];
    for (std::size_t i = 0; i < name.size(); ++i) {
        buffer[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(name[i])));
    }
    const std::string_view key(buffer, name.size());
    const auto it = std::lower_bound(kCommands.begin(), kCommands.end(), key,
                                     [](const Command& c, std::string_view n) { return c.name < n; });
    return it != kCommands.end() && it->name == key ? &*it : nullptr;
}

std::size_t command_count() {
    return kCommands.size();
}

std::vector<Args> execute(ExecContext& ctx, const Args& args) {
    const Command* command = find_command(args[0]);
    if (command == nullptr) {
        std::string message = "ERR unknown command '" + args[0] + "', with args beginning with: ";
        for (std::size_t i = 1; i < args.size() && i <= 3; ++i) message += "'" + args[i] + "' ";
        resp::error(ctx.out, message);
        return {};
    }
    const auto count = static_cast<int>(args.size());
    if ((command->arity > 0 && count != command->arity) || (command->arity < 0 && count < -command->arity)) {
        resp::error(ctx.out,
                    "ERR wrong number of arguments for '" + std::string(command->name) + "' command");
        return {};
    }
    ctx.dirty = false;
    ctx.propagate.clear();
    command->handler(ctx, args);
    if (!command->write || !ctx.dirty) return {};
    if (ctx.propagate.empty()) return {args};
    return std::move(ctx.propagate);
}

}  // namespace kv
