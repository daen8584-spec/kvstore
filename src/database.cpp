#include "database.hpp"

namespace kv {

const char* type_name(const Value& value) {
    switch (value.index()) {
        case 0:
            return "string";
        case 1:
            return "list";
        case 2:
            return "hash";
        default:
            return "zset";
    }
}

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

bool Database::expired(std::string_view key, std::int64_t now) const {
    const std::int64_t* when = expires_.find(key);
    return when != nullptr && *when <= now;
}

Value* Database::find(std::string_view key, std::int64_t now) {
    Value* value = keys_.find(key);
    if (value == nullptr) return nullptr;
    if (expired(key, now)) {
        keys_.erase(key);
        expires_.erase(key);
        ++expired_total_;
        return nullptr;
    }
    return value;
}

Value& Database::set(std::string_view key, Value value, bool keep_ttl) {
    // An expired key that has not been removed yet must not pass its old expiry on.
    if (keep_ttl && expired(key, now_ms())) expires_.erase(key);
    auto [slot, inserted] = keys_.try_emplace(key, std::move(value));
    if (!inserted) {
        *slot = std::move(value);
        if (!keep_ttl) expires_.erase(key);
    }
    return *slot;
}

bool Database::erase(std::string_view key) {
    expires_.erase(key);
    return keys_.erase(key);
}

void Database::clear() {
    keys_.clear();
    expires_.clear();
}

bool Database::set_expire(std::string_view key, std::int64_t when_ms) {
    if (keys_.find(key) == nullptr) return false;
    auto [slot, inserted] = expires_.try_emplace(key, when_ms);
    if (!inserted) *slot = when_ms;
    return true;
}

std::optional<std::int64_t> Database::expire_time(std::string_view key) const {
    const std::int64_t* when = expires_.find(key);
    if (when == nullptr) return std::nullopt;
    return *when;
}

bool Database::persist(std::string_view key) {
    return expires_.erase(key);
}

ExpireStats Database::active_expire(std::int64_t now, std::chrono::microseconds budget) {
    constexpr std::size_t kSample = 20;
    ExpireStats stats;
    const auto deadline = std::chrono::steady_clock::now() + budget;
    std::string key;
    for (;;) {
        std::size_t expired_now = 0;
        const std::size_t sample = std::min(kSample, expires_.size());
        for (std::size_t i = 0; i < sample; ++i) {
            auto* entry = expires_.random_entry(rng_);
            if (entry == nullptr) break;
            ++stats.sampled;
            if (entry->value <= now) {
                key = entry->key;
                keys_.erase(key);
                expires_.erase(key);
                ++expired_now;
            }
        }
        stats.expired += expired_now;
        expired_total_ += expired_now;
        if (sample == 0 || expired_now * 4 <= sample) break;
        if (std::chrono::steady_clock::now() >= deadline) break;
    }
    return stats;
}

void Database::rehash_idle(std::chrono::microseconds budget) {
    keys_.rehash_for(budget / 2);
    expires_.rehash_for(budget / 2);
}

}  // namespace kv
