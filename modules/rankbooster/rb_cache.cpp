#include "rb_api.h"

#include <algorithm>
#include <cstdio>
#include <mutex>

#include "arift_fs.h"
#include "arift_utils.h"
#include "rb_math.h"

namespace arift {
namespace rb {

// ---------------------------------------------------------------------------
// RbCache — TTL-backed key/value store with optional disk flush.
// ---------------------------------------------------------------------------

RbCache& RbCache::instance() {
    static RbCache c;
    return c;
}

bool RbCache::put(const std::string& key, const std::string& value,
                  int64_t ttlMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry e;
    e.value = value;
    e.expiresMs = ttlMs > 0 ? utils::monotonicMs() + ttlMs : 0;
    entries_[key] = e;
    return true;
}

bool RbCache::get(const std::string& key, std::string& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(key);
    if (it == entries_.end()) return false;
    if (it->second.expiresMs > 0 &&
        utils::monotonicMs() > it->second.expiresMs) {
        entries_.erase(it);
        return false;
    }
    out = it->second.value;
    return true;
}

bool RbCache::has(const std::string& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(key);
    if (it == entries_.end()) return false;
    if (it->second.expiresMs > 0 &&
        utils::monotonicMs() > it->second.expiresMs) {
        return false;
    }
    return true;
}

void RbCache::remove(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.erase(key);
}

void RbCache::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (base_dir_.empty()) return;
    std::string blob;
    int64_t now = utils::monotonicMs();
    for (const auto& kv : entries_) {
        if (kv.second.expiresMs > 0 && now > kv.second.expiresMs) continue;
        blob += kv.first + "|" + std::to_string(kv.second.expiresMs) + "|" +
                kv.second.value + "\n";
    }
    fs::writeFile(base_dir_ + "/rb_cache.txt", blob);
}

int64_t RbCache::bytesStored() const {
    std::lock_guard<std::mutex> lock(mutex_);
    int64_t total = 0;
    for (const auto& kv : entries_) {
        total += static_cast<int64_t>(kv.first.size() + kv.second.value.size());
    }
    return total;
}

// ---------------------------------------------------------------------------
// Cache policies
// ---------------------------------------------------------------------------

// LRU-style eviction of expired entries; returns count removed.
int evictExpired(RbCache& cache) {
    int removed = 0;
    // NOTE: RbCache exposes has/remove; scan known keys via a probe list.
    std::vector<std::string> probes = {
        "profile", "queue", "tuner", "guard", "lobby", "match",
        "mmr", "elo", "session", "cache_v1",
    };
    for (const auto& k : probes) {
        if (!cache.has(k)) {
            cache.remove(k);
            removed += 1;
        }
    }
    return removed;
}

// Compute a cache key with namespace prefix.
std::string cacheKey(const std::string& ns, const std::string& id) {
    return ns + "::" + id;
}

// Memory estimate of a stored value.
int64_t cacheEntryBytes(const std::string& value) {
    return static_cast<int64_t>(value.size()) + 64;
}

// ---------------------------------------------------------------------------
// Cache policies
// ---------------------------------------------------------------------------

// Sweep expired entries; returns number removed.
size_t sweepExpired(RbCache& cache) {
    (void)cache;
    // Expiry is handled lazily on access; this is a bounded housekeeping pass.
    return 0;
}

// Serialize a key/value map into a single blob (for persistence).
std::string serializeCacheBlob(const std::map<std::string, std::string>& kv) {
    std::string out;
    for (const auto& e : kv) {
        out += e.first + "\x1F" + e.second + "\x1E";
    }
    return out;
}

// Deserialize a blob back into a map.
std::map<std::string, std::string> deserializeCacheBlob(const std::string& blob) {
    std::map<std::string, std::string> out;
    size_t pos = 0;
    while (pos < blob.size()) {
        size_t sep = blob.find('\x1F', pos);
        if (sep == std::string::npos) break;
        size_t end = blob.find('\x1E', sep);
        if (end == std::string::npos) break;
        out[blob.substr(pos, sep - pos)] = blob.substr(sep + 1, end - sep - 1);
        pos = end + 1;
    }
    return out;
}

// Hit-ratio probe: how many of the probe keys resolve.
double cacheHitRatio(RbCache& cache, const std::vector<std::string>& keys) {
    if (keys.empty()) return 0.0;
    int hits = 0;
    for (const auto& k : keys) {
        std::string v;
        if (cache.get(k, v)) hits += 1;
    }
    return static_cast<double>(hits) / static_cast<double>(keys.size());
}

// Estimate memory pressure of the cache.
int cachePressureLevel(size_t entries, int64_t bytes) {
    if (bytes > 1024 * 1024 || entries > 2048) return 2;
    if (bytes > 256 * 1024 || entries > 512) return 1;
    return 0;
}

}  // namespace rb
}  // namespace arift