#include "multi.hpp"

#include <algorithm>

#include "database.hpp"

namespace kv {

void Watches::filter_add(std::string_view key) {
    if (filter_counts_.empty()) {
        filter_bits_.resize(kFilterBits / 64);
        filter_counts_.resize(kFilterBits);
    }
    const std::size_t index = slot(key);
    if (filter_counts_[index]++ == 0) filter_bits_[index / 64] |= std::uint64_t{1} << (index % 64);
}

void Watches::filter_remove(std::string_view key) {
    const std::size_t index = slot(key);
    if (--filter_counts_[index] == 0) filter_bits_[index / 64] &= ~(std::uint64_t{1} << (index % 64));
}

// The duplicate check searches the key's watchers, which are few, rather than the
// session's watched keys, so watching n keys costs O(n) rather than O(n^2).
void Watches::watch(Session& session, const std::string& key, bool exists) {
    auto& sessions = sessions_by_key_[key];
    if (std::find(sessions.begin(), sessions.end(), &session) != sessions.end()) return;
    if (sessions.empty()) filter_add(key);
    sessions.push_back(&session);
    session.watched.emplace_back(key, exists);
}

void Watches::unwatch_all(Session& session) {
    for (const auto& [key, existed] : session.watched) {
        const auto it = sessions_by_key_.find(key);
        if (it == sessions_by_key_.end()) continue;
        auto& sessions = it->second;
        sessions.erase(std::remove(sessions.begin(), sessions.end(), &session), sessions.end());
        if (sessions.empty()) {
            filter_remove(key);
            sessions_by_key_.erase(it);
        }
    }
    session.watched.clear();
    session.watch_dirty = false;
}

void Watches::touch(std::string_view key) {
    if (sessions_by_key_.empty() || !maybe_watched(slot(key))) return;
    const auto it = sessions_by_key_.find(key);
    if (it == sessions_by_key_.end()) return;
    for (Session* session : it->second) session->watch_dirty = true;
}

void Watches::touch_existing(Database& db, std::int64_t now) {
    for (auto& [key, sessions] : sessions_by_key_) {
        if (db.find(key, now) == nullptr) continue;
        for (Session* session : sessions) session->watch_dirty = true;
    }
}

bool watched_key_vanished(const Session& session, Database& db, std::int64_t now) {
    for (const auto& [key, existed] : session.watched) {
        if (existed && db.find(key, now) == nullptr) return true;
    }
    return false;
}

}  // namespace kv
