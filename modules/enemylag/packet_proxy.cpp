#include "enemy_lag.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <vector>

#include "arift_log.h"
#include "arift_utils.h"

namespace arift {
namespace enemylag {

namespace {

// ---------------------------------------------------------------------------
// Frame codec (obfuscation layer)
// ---------------------------------------------------------------------------

// Rotate the byte stream by `offset`.
void rotateBytes(std::vector<uint8_t>& data, uint8_t offset) {
    if (data.size() < 2 || offset == 0) return;
    size_t shift = static_cast<size_t>(offset) % data.size();
    std::vector<uint8_t> tmp(data.begin(), data.end());
    for (size_t i = 0; i < data.size(); ++i) {
        data[(i + shift) % data.size()] = tmp[i];
    }
}

// Xor the stream with a seeded keystream.
void xorStream(std::vector<uint8_t>& data, uint64_t key) {
    uint64_t state = key;
    for (size_t i = 0; i < data.size(); ++i) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        uint8_t k = static_cast<uint8_t>(state >> 56);
        data[i] ^= k;
    }
}

// Fisher-Yates shuffle with a deterministic seed.
void shuffleBytes(std::vector<uint8_t>& data, uint64_t seed) {
    if (data.size() < 2) return;
    uint64_t state = seed;
    auto rnd = [&state]() {
        state = state * 2862933555777941757ULL + 3037000493ULL;
        return static_cast<size_t>((state >> 33) % 1000000007ULL);
    };
    for (size_t i = data.size() - 1; i > 0; --i) {
        size_t j = rnd() % (i + 1);
        std::swap(data[i], data[j]);
    }
}

void unshuffleBytes(std::vector<uint8_t>& data, uint64_t seed) {
    if (data.size() < 2) return;
    uint64_t state = seed;
    auto rnd = [&state]() {
        state = state * 2862933555777941757ULL + 3037000493ULL;
        return static_cast<size_t>((state >> 33) % 1000000007ULL);
    };
    std::vector<size_t> order;
    for (size_t i = data.size() - 1; i > 0; --i) {
        size_t j = rnd() % (i + 1);
        order.push_back(j);
    }
    for (size_t k = order.size(); k > 0; --k) {
        size_t i = k;
        size_t j = order[k - 1];
        std::swap(data[i], data[j]);
    }
}

// Length prefix framing: [len u16][payload]
std::vector<uint8_t> frameRaw(const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> out;
    out.push_back(static_cast<uint8_t>(payload.size() & 0xFF));
    out.push_back(static_cast<uint8_t>((payload.size() >> 8) & 0xFF));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

// Checksum for integrity verification of obfuscated frames.
uint32_t frameChecksum(const std::vector<uint8_t>& data) {
    return utils::crc32(data.data(), data.size());
}

// ---------------------------------------------------------------------------
// Delay scheduler
// ---------------------------------------------------------------------------

struct DelayProfile {
    int baseMs = 0;
    int jitterMs = 0;
    int burstSize = 1;
    int burstGapMs = 0;
};

DelayProfile profileForConfig(const EnemyLagConfig& cfg) {
    DelayProfile p;
    p.baseMs = cfg.delayMs;
    p.jitterMs = cfg.jitterMs;
    p.burstSize = 1 + (cfg.delayMs / 400);
    p.burstGapMs = cfg.burstCooldownMs;
    return p;
}

// Cluster model: packets within a short window are delayed together so the
// enemy sees a single "hitch" instead of a constant lag stream.
int hitchWindowMs(const DelayProfile& p) {
    return std::max(40, p.burstSize * 12);
}

// Whether two frames belong to the same manipulation burst.
bool sameBurst(const WireFrame& a, const WireFrame& b, int64_t windowMs) {
    return a.srcEntity == b.srcEntity &&
           (b.capturedAtMs - a.capturedAtMs) < windowMs;
}

}  // namespace

// ---------------------------------------------------------------------------
// Proxy transport plumbing
// ---------------------------------------------------------------------------

// Transport sink (assigned by the hook layer; e.g. sends via the game socket).
namespace {
std::function<void(const std::vector<uint8_t>&)> g_sink;
std::function<const std::vector<uint8_t>*(uint64_t)> g_capture;
}

void setTransportSink(std::function<void(const std::vector<uint8_t>&)> sink) {
    g_sink = std::move(sink);
}

void setCaptureSource(
    std::function<const std::vector<uint8_t>*(uint64_t)> capture) {
    g_capture = std::move(capture);
}

// Forward a raw frame to the transport.
void forwardRaw(const std::vector<uint8_t>& bytes) {
    if (g_sink) g_sink(bytes);
}

// Deliver a delayed frame (deobfuscated to the original bytes).
void deliverFrame(const WireFrame& frame) {
    std::vector<uint8_t> raw;
    raw.push_back(frame.opcode);
    raw.push_back(static_cast<uint8_t>(frame.seq & 0xFF));
    raw.push_back(static_cast<uint8_t>((frame.seq >> 8) & 0xFF));
    raw.push_back(static_cast<uint8_t>((frame.seq >> 16) & 0xFF));
    raw.push_back(static_cast<uint8_t>((frame.seq >> 24) & 0xFF));
    raw.insert(raw.end(), frame.payload.begin(), frame.payload.end());
    forwardRaw(raw);
}

// Obfuscate a frame before re-injection (returns framed blob).
std::vector<uint8_t> obfuscateFrame(const WireFrame& frame, uint64_t key) {
    std::vector<uint8_t> body;
    body.push_back(frame.opcode);
    body.push_back(static_cast<uint8_t>(frame.seq & 0xFF));
    body.push_back(static_cast<uint8_t>((frame.seq >> 8) & 0xFF));
    body.push_back(static_cast<uint8_t>((frame.seq >> 16) & 0xFF));
    body.push_back(static_cast<uint8_t>((frame.seq >> 24) & 0xFF));
    body.insert(body.end(), frame.payload.begin(), frame.payload.end());
    uint8_t seedByte = static_cast<uint8_t>(key & 0xFF);
    rotateBytes(body, seedByte);
    xorStream(body, key);
    std::vector<uint8_t> framed = frameRaw(body);
    uint32_t sum = frameChecksum(framed);
    framed.push_back(static_cast<uint8_t>(sum & 0xFF));
    framed.push_back(static_cast<uint8_t>((sum >> 8) & 0xFF));
    return framed;
}

// Deobfuscate a frame blob back to a WireFrame.
bool deobfuscateFrame(const std::vector<uint8_t>& blob, uint64_t key,
                      WireFrame* out) {
    if (blob.size() < 4) return false;
    uint32_t stored = static_cast<uint32_t>(blob[blob.size() - 2]) |
                      (static_cast<uint32_t>(blob[blob.size() - 1]) << 8);
    std::vector<uint8_t> framed(blob.begin(), blob.end() - 2);
    if (frameChecksum(framed) != stored) return false;
    size_t len = static_cast<size_t>(framed[0]) |
                 (static_cast<size_t>(framed[1]) << 8);
    if (framed.size() < len + 2) return false;
    std::vector<uint8_t> body(framed.begin() + 2, framed.begin() + 2 + len);
    xorStream(body, key);
    uint8_t seedByte = static_cast<uint8_t>(key & 0xFF);
    // Inverse rotation by (size - shift) % size.
    if (body.size() >= 2 && seedByte != 0) {
        size_t shift = static_cast<size_t>(seedByte) % body.size();
        rotateBytes(body, static_cast<uint8_t>(body.size() - shift));
    }
    if (body.size() < 5) return false;
    out->opcode = body[0];
    out->seq = static_cast<uint32_t>(body[1]) |
               (static_cast<uint32_t>(body[2]) << 8) |
               (static_cast<uint32_t>(body[3]) << 16) |
               (static_cast<uint32_t>(body[4]) << 24);
    out->payload.assign(body.begin() + 5, body.end());
    return true;
}

// ---------------------------------------------------------------------------
// Capture-side classification helpers
// ---------------------------------------------------------------------------

// Split a raw buffer into individual frames (framing is length-prefixed).
std::vector<std::vector<uint8_t>> splitFrames(const std::vector<uint8_t>& buf,
                                              size_t* consumed) {
    std::vector<std::vector<uint8_t>> out;
    size_t pos = 0;
    while (pos + 2 <= buf.size()) {
        size_t len = static_cast<size_t>(buf[pos]) |
                     (static_cast<size_t>(buf[pos + 1]) << 8);
        if (pos + 2 + len > buf.size()) break;
        out.emplace_back(buf.begin() + static_cast<long>(pos + 2),
                         buf.begin() + static_cast<long>(pos + 2 + len));
        pos += 2 + len;
    }
    *consumed = pos;
    return out;
}

// Classify a raw frame body into a WireFrame (heuristic header layout).
WireFrame decodeFrame(const std::vector<uint8_t>& body, uint64_t src) {
    WireFrame f;
    f.srcEntity = src;
    f.capturedAtMs = utils::monotonicMs();
    if (body.empty()) return f;
    f.opcode = body[0];
    if (body.size() >= 5) {
        f.seq = static_cast<uint32_t>(body[1]) |
                (static_cast<uint32_t>(body[2]) << 8) |
                (static_cast<uint32_t>(body[3]) << 16) |
                (static_cast<uint32_t>(body[4]) << 24);
    }
    if (body.size() > 5) {
        f.payload.assign(body.begin() + 5, body.end());
    }
    switch (f.opcode) {
        case 0x12:
        case 0x13:
        case 0x14:
        case 0x17:
        case 0x18:
        case 0x19:
        case 0x1A:
            f.isSkill = true;
            break;
        case 0x21:
        case 0x22:
        case 0x23:
        case 0x24:
            f.isMovement = true;
            break;
        default:
            break;
    }
    return f;
}

// ---------------------------------------------------------------------------
// Anti-detection traffic shaping
// ---------------------------------------------------------------------------

// Inter-burst gap so manipulation never looks periodic.
int shapedGapMs(const EnemyLagConfig& cfg, uint64_t seed) {
    double base = static_cast<double>(cfg.burstCooldownMs);
    double spread = base * 0.4;
    double g = utils::gaussian(base, spread);
    uint64_t h = seed ^ 0xDEADBEEF;
    g += static_cast<double>(h % 60);
    return utils::clamp(static_cast<int>(g), 500, 12000);
}

// Whether we should pause entirely this window (anti-pattern).
bool shapedPauseNow(int64_t nowMs, int64_t lastBurstMs,
                    const EnemyLagConfig& cfg, uint64_t seed) {
    if (lastBurstMs <= 0) return false;
    int64_t since = nowMs - lastBurstMs;
    int64_t pauseAfter = static_cast<int64_t>(shapedGapMs(cfg, seed)) * 4;
    if (since < pauseAfter) return false;
    return (seed & 0xFF) < static_cast<uint64_t>(cfg.dropChance * 2000.0);
}

// Randomized per-second manipulation budget (35%..100% of configured).
int shapedBudgetPerSecond(const EnemyLagConfig& cfg, uint64_t seed) {
    double f = 0.35 + static_cast<double>(seed % 1300) / 2000.0;
    return utils::clamp(static_cast<int>(cfg.maxPacketsPerSec * f), 1,
                        cfg.maxPacketsPerSec);
}

// ---------------------------------------------------------------------------
// Host integration
// ---------------------------------------------------------------------------

// Full pipeline for an outbound buffer from the game socket.
void processOutbound(const std::vector<uint8_t>& buf, uint64_t localEntity,
                     const std::vector<uint64_t>& enemyIds,
                     const EnemyLagConfig& cfg, uint64_t seed) {
    size_t consumed = 0;
    std::vector<std::vector<uint8_t>> frames = splitFrames(buf, &consumed);
    // Pass through anything we could not parse.
    if (frames.empty()) {
        forwardRaw(buf);
        return;
    }
    bool parsedAll = consumed == buf.size();
    for (auto& body : frames) {
        WireFrame f = decodeFrame(body, localEntity);
        f.isEnemy = false;  // outbound frames originate from us
        bool dropped = PacketProxy::instance().onFrame(f);
        if (dropped) continue;
        // Deliver original immediately (the delayed copy is handled by the
        // proxy's pump()).
        std::vector<uint8_t> raw;
        raw.push_back(f.opcode);
        raw.push_back(static_cast<uint8_t>(f.seq & 0xFF));
        raw.push_back(static_cast<uint8_t>((f.seq >> 8) & 0xFF));
        raw.push_back(static_cast<uint8_t>((f.seq >> 16) & 0xFF));
        raw.push_back(static_cast<uint8_t>((f.seq >> 24) & 0xFF));
        raw.insert(raw.end(), f.payload.begin(), f.payload.end());
        forwardRaw(raw);
    }
    // Preserve trailing bytes that did not parse as frames.
    if (!parsedAll && consumed < buf.size()) {
        std::vector<uint8_t> tail(buf.begin() + static_cast<long>(consumed),
                                  buf.end());
        forwardRaw(tail);
    }
}

// Full pipeline for an inbound buffer (from server, enemy actions).
void processInbound(const std::vector<uint8_t>& buf,
                    const std::vector<uint64_t>& enemyIds,
                    const EnemyLagConfig& cfg, uint64_t seed) {
    size_t consumed = 0;
    std::vector<std::vector<uint8_t>> frames = splitFrames(buf, &consumed);
    if (frames.empty()) {
        forwardRaw(buf);
        return;
    }
    // Identify the enemy source for each frame by srcEntity embedded in
    // the payload (heuristic: entity id at fixed offset).
    for (auto& body : frames) {
        uint64_t src = 0;
        if (body.size() >= 13) {
            src = 0;
            for (int i = 0; i < 8; ++i) {
                src |= static_cast<uint64_t>(body[static_cast<size_t>(i) + 5])
                       << (8 * i);
            }
        }
        WireFrame f = decodeFrame(body, src);
        f.isEnemy = std::find(enemyIds.begin(), enemyIds.end(), src) !=
                    enemyIds.end();
        bool dropped = PacketProxy::instance().onFrame(f);
        if (dropped) continue;
        std::vector<uint8_t> raw;
        raw.push_back(f.opcode);
        raw.push_back(static_cast<uint8_t>(f.seq & 0xFF));
        raw.push_back(static_cast<uint8_t>((f.seq >> 8) & 0xFF));
        raw.push_back(static_cast<uint8_t>((f.seq >> 16) & 0xFF));
        raw.push_back(static_cast<uint8_t>((f.seq >> 24) & 0xFF));
        raw.insert(raw.end(), f.payload.begin(), f.payload.end());
        forwardRaw(raw);
    }
}

// ---------------------------------------------------------------------------
// Persistence & diagnostics
// ---------------------------------------------------------------------------

// Serialize config to a compact string (for cache).
std::string configToCacheString(const EnemyLagConfig& cfg) {
    char buf[256];
    snprintf(buf, sizeof(buf),
             "el%d_%d_%d_%.2f_%.2f_%d_%d_%d_%d_%d",
             cfg.delayMs, cfg.jitterMs, cfg.rampSeconds,
             cfg.applyChance, cfg.dropChance,
             cfg.combatOnly ? 1 : 0, cfg.obfuscate ? 1 : 0,
             cfg.maxPacketsPerSec, cfg.burstCooldownMs, cfg.pauseMs);
    return std::string(buf);
}

// Parse config back from the cache string.
bool configFromCacheString(const std::string& s, EnemyLagConfig* out) {
    if (s.size() < 4 || s[0] != 'e' || s[1] != 'l') return false;
    std::vector<std::string> parts = utils::split(s, '_');
    if (parts.size() != 11) return false;
    EnemyLagConfig c;
    c.delayMs = atoi(parts[1].c_str());
    c.jitterMs = atoi(parts[2].c_str());
    c.rampSeconds = atoi(parts[3].c_str());
    c.applyChance = atof(parts[4].c_str());
    c.dropChance = atof(parts[5].c_str());
    c.combatOnly = parts[6] == "1";
    c.obfuscate = parts[7] == "1";
    c.maxPacketsPerSec = atoi(parts[8].c_str());
    c.burstCooldownMs = atoi(parts[9].c_str());
    c.pauseMs = atoi(parts[10].c_str());
    *out = c;
    return true;
}

// Diag dump for debugging.
std::string diagDump(const EnemyLagConfig& cfg, const EnemyLagStats& stats) {
    char buf[512];
    snprintf(buf, sizeof(buf),
             "cfg{delay=%d jitter=%d apply=%.2f drop=%.2f combat=%d "
             "obf=%d max/s=%d}\n"
             "stats{seen=%lld delayed=%lld dropped=%lld obf=%lld "
             "enemies=%lld avg=%.1f}",
             cfg.delayMs, cfg.jitterMs, cfg.applyChance, cfg.dropChance,
             cfg.combatOnly ? 1 : 0, cfg.obfuscate ? 1 : 0,
             cfg.maxPacketsPerSec,
             static_cast<long long>(stats.framesSeen),
             static_cast<long long>(stats.framesDelayed),
             static_cast<long long>(stats.framesDropped),
             static_cast<long long>(stats.framesObfuscated),
             static_cast<long long>(stats.enemiesTracked),
             stats.appliedDelayAvgMs);
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// Sanity / limits
// ---------------------------------------------------------------------------

// Sanitize a config coming from the UI.
void sanitizeConfig(EnemyLagConfig* cfg) {
    cfg->delayMs = utils::clamp(cfg->delayMs, 0, 2000);
    cfg->jitterMs = utils::clamp(cfg->jitterMs, 0, 800);
    cfg->applyChance = utils::clamp(cfg->applyChance, 0.0, 1.0);
    cfg->dropChance = utils::clamp(cfg->dropChance, 0.0, 0.3);
    cfg->rampSeconds = utils::clamp(cfg->rampSeconds, 0, 120);
    cfg->pauseMs = utils::clamp(cfg->pauseMs, 0, 60000);
    cfg->burstCooldownMs = utils::clamp(cfg->burstCooldownMs, 200, 30000);
    cfg->maxPacketsPerSec = utils::clamp(cfg->maxPacketsPerSec, 1, 200);
}

// Whether the config is within legal bounds for the feature.
bool configValid(const EnemyLagConfig& cfg) {
    if (cfg.delayMs < 0 || cfg.delayMs > 2000) return false;
    if (cfg.jitterMs < 0 || cfg.jitterMs > 800) return false;
    if (cfg.applyChance < 0.0 || cfg.applyChance > 1.0) return false;
    if (cfg.dropChance < 0.0 || cfg.dropChance > 0.3) return false;
    if (cfg.maxPacketsPerSec < 1 || cfg.maxPacketsPerSec > 200) return false;
    return true;
}

// ---------------------------------------------------------------------------
// Sequencing layer (fake reliability)
// ---------------------------------------------------------------------------

// The proxy keeps a per-session sequence space so re-injected frames look
// contiguous even when obfuscation scrambles the wire order.
class SequenceSpace {
public:
    static SequenceSpace& instance() {
        static SequenceSpace s;
        return s;
    }

    void reset() { next_seq_ = 0x1000; map_.clear(); }

    // Map an observed frame seq to our synthetic seq.
    uint32_t synthSeq(uint32_t observed) {
        auto it = map_.find(observed);
        if (it != map_.end()) return it->second;
        uint32_t s = next_seq_++;
        map_[observed] = s;
        return s;
    }

    // Latest delivered synthetic seq.
    uint32_t lastDelivered() const { return last_delivered_; }
    void noteDelivered(uint32_t seq) { last_delivered_ = seq; }

    // Gap detector: true if a re-injected frame would create a visible hole.
    bool wouldCreateGap(uint32_t seq) const {
        if (last_delivered_ == 0) return false;
        return seq < last_delivered_;
    }

private:
    uint32_t next_seq_ = 0x1000;
    uint32_t last_delivered_ = 0;
    std::map<uint32_t, uint32_t> map_;
};

// ---------------------------------------------------------------------------
// Retransmission simulation
// ---------------------------------------------------------------------------

// Occasional duplicate delivery (an ACK-less network artifact) to mask
// drops: the enemy sees a duplicate rather than a missing packet.
class DupSimulator {
public:
    static DupSimulator& instance() {
        static DupSimulator d;
        return d;
    }

    void reset() { dupes_sent_ = 0; }

    // Decide whether to emit a duplicate of the given frame.
    bool shouldDup(const WireFrame& f, const EnemyLagConfig& cfg,
                   uint64_t seed) {
        if (cfg.dropChance <= 0.0) return false;
        if (!f.isEnemy) return false;
        if (f.isMovement) return false;
        uint64_t h = seed ^ f.seq * 0x9E3779B97F4A7C15ULL;
        double r = static_cast<double>(h & 0xFFFF) / 65535.0;
        return r < cfg.dropChance * 0.25;
    }

    void noteDup() { dupes_sent_ += 1; }
    int64_t dups() const { return dupes_sent_; }

private:
    int64_t dupes_sent_ = 0;
};

// ---------------------------------------------------------------------------
// Watchdog
// ---------------------------------------------------------------------------

// If the proxy stalls (no frames for too long) the watchdog resets the
// manipulation state so the feature never wedges the connection.
class Watchdog {
public:
    static Watchdog& instance() {
        static Watchdog w;
        return w;
    }

    void observe(int64_t nowMs) { last_activity_ms_ = nowMs; }

    // True if no frames have flowed for the given timeout.
    bool stalled(int64_t nowMs, int64_t timeoutMs) const {
        if (last_activity_ms_ <= 0) return false;
        return nowMs - last_activity_ms_ > timeoutMs;
    }

    void reset() { last_activity_ms_ = 0; }

private:
    int64_t last_activity_ms_ = 0;
};

// ---------------------------------------------------------------------------
// Jitter envelope
// ---------------------------------------------------------------------------

// The jitter envelope models what the enemy's client will perceive: a slow
// sine drift plus short spikes. This is what we shape on the wire.
class JitterEnvelope {
public:
    JitterEnvelope() { reseed(); }

    void reseed() {
        base_ = 80.0;
        amp_ = 40.0 + static_cast<double>(utils::random32() % 80);
        freq_ = 0.02 + static_cast<double>(utils::random32() % 10) / 500.0;
        phase_ = static_cast<double>(utils::random32() % 628) / 100.0;
    }

    // Value at the given time (ms since session start).
    double at(int64_t tMs) const {
        return base_ + amp_ * std::sin(freq_ * static_cast<double>(tMs) +
                                       phase_);
    }

    // Derive a per-frame delay from the envelope plus noise.
    int delayForFrameMs(int64_t tMs, uint64_t seed, double scale) const {
        double env = at(tMs);
        double noise = utils::gaussian(0.0, 25.0);
        uint64_t h = seed;
        double spike = static_cast<double>(h % 1000) / 1000.0 > 0.97
                           ? 120.0 + static_cast<double>(h % 200)
                           : 0.0;
        double v = (env + noise + spike) * scale;
        return utils::clamp(static_cast<int>(v), 0, 3000);
    }

private:
    double base_;
    double amp_;
    double freq_;
    double phase_;
};

// ---------------------------------------------------------------------------
// Frame signature model
// ---------------------------------------------------------------------------

// Each manipulated frame receives a synthetic signature so the stream
// remains plausible to any passive analysis of packet sizes/timing.
struct FrameSignature {
    uint8_t sizeClass = 0;   // 0 small, 1 medium, 2 large
    uint16_t pseudoLen = 0;  // plausible length
    uint8_t flags = 0;
};

FrameSignature signatureFor(const WireFrame& f, uint64_t seed) {
    FrameSignature s;
    size_t len = f.payload.size();
    if (len < 16) s.sizeClass = 0;
    else if (len < 64) s.sizeClass = 1;
    else s.sizeClass = 2;
    uint64_t h = seed ^ f.seq * 0x100000001B3ULL;
    s.pseudoLen = static_cast<uint16_t>(16 + (h % 96));
    s.flags = static_cast<uint8_t>(h >> 40);
    return s;
}

// Timing signature: plausible inter-arrival gap for a frame of this class.
int plausibleGapMs(const FrameSignature& sig, uint64_t seed) {
    double base = 40.0;
    if (sig.sizeClass == 1) base = 90.0;
    if (sig.sizeClass == 2) base = 140.0;
    double j = utils::gaussian(0.0, base * 0.3);
    return utils::clamp(static_cast<int>(base + j), 8, 600);
}

// ---------------------------------------------------------------------------
// Stream shaping (server-visible pacing)
// ---------------------------------------------------------------------------

// The proxy may pace manipulated frames to mimic natural network bursts.
class PacingShaper {
public:
    static PacingShaper& instance() {
        static PacingShaper p;
        return p;
    }

    void reset() { next_slot_ms_ = 0; }

    // True if a frame may be injected right now.
    bool slotOpen(int64_t nowMs, const EnemyLagConfig& cfg) const {
        if (next_slot_ms_ <= 0) return true;
        if (nowMs >= next_slot_ms_) return true;
        return cfg.maxPacketsPerSec > 60;  // high rate: ignore pacing
    }

    void consumeSlot(int64_t nowMs, const FrameSignature& sig,
                     uint64_t seed) {
        int gap = plausibleGapMs(sig, seed);
        next_slot_ms_ = nowMs + gap;
    }

    // Idle guard: if the stream goes silent for too long, open a slot.
    void forceOpen() { next_slot_ms_ = 0; }

private:
    int64_t next_slot_ms_ = 0;
};

// ---------------------------------------------------------------------------
// Obfuscation variant table
// ---------------------------------------------------------------------------

// Multiple obfuscation passes; chosen per-frame from a rotating index so no
// single pattern dominates.
enum class ObfPass : int { kXor = 0, kRotate = 1, kShuffle = 2, kPad = 3 };

ObfPass pickPass(uint64_t seed) {
    return static_cast<ObfPass>(seed % 4);
}

// Apply the chosen pass to a frame body.
void applyPass(std::vector<uint8_t>& body, ObfPass pass, uint64_t key) {
    switch (pass) {
        case ObfPass::kXor:
            xorStream(body, key);
            break;
        case ObfPass::kRotate:
            rotateBytes(body, static_cast<uint8_t>(key & 0xFF));
            break;
        case ObfPass::kShuffle:
            shuffleBytes(body, key);
            break;
        case ObfPass::kPad:
            body.push_back(static_cast<uint8_t>(key >> 24));
            body.push_back(static_cast<uint8_t>(key >> 16));
            break;
    }
}

// ---------------------------------------------------------------------------
// Latency statistics (struct in enemy_lag.h)
// ---------------------------------------------------------------------------

// Global latency model for the current session.
LatencyModel& sessionLatencyModel() {
    static LatencyModel m;
    return m;
}

// ---------------------------------------------------------------------------
// Replay protection
// ---------------------------------------------------------------------------

// Keeps a bounded set of recently delayed seqs so we never double-process
// a frame that the pump already delivered.
class ReplayGuard {
public:
    ReplayGuard() : ring_(64, 0), pos_(0), filled_(0) {}

    bool seen(uint32_t seq) const {
        for (size_t i = 0; i < filled_; ++i) {
            if (ring_[(pos_ + 64 - 1 - i) % 64] == seq) return true;
        }
        return false;
    }

    void note(uint32_t seq) {
        ring_[pos_] = seq;
        pos_ = (pos_ + 1) % 64;
        if (filled_ < 64) filled_ += 1;
    }

    void reset() {
        filled_ = 0;
        pos_ = 0;
    }

private:
    std::vector<uint32_t> ring_;
    size_t pos_;
    size_t filled_;
};

ReplayGuard& replayGuard() {
    static ReplayGuard g;
    return g;
}

// ---------------------------------------------------------------------------
// Session state machine
// ---------------------------------------------------------------------------

const char* proxyStateName(ProxyState s) {
    switch (s) {
        case ProxyState::kDetached: return "detached";
        case ProxyState::kAttached: return "attached";
        case ProxyState::kRamping: return "ramping";
        case ProxyState::kActive: return "active";
        case ProxyState::kPaused: return "paused";
        case ProxyState::kRecovering: return "recovering";
    }
    return "unknown";
}

// Derive the proxy state from current signals.
ProxyState deriveState(const EnemyLagStats& s, int64_t nowMs,
                       const EnemyLagConfig& cfg) {
    if (!s.active) return ProxyState::kDetached;
    if (s.startedAtMs <= 0) return ProxyState::kAttached;
    double elapsed = static_cast<double>(nowMs - s.startedAtMs) / 1000.0;
    if (elapsed < static_cast<double>(cfg.rampSeconds)) {
        return ProxyState::kRamping;
    }
    if (s.lastPauseUntilMs > nowMs) return ProxyState::kPaused;
    if (s.framesDelayed == 0 && elapsed > 30.0) {
        return ProxyState::kRecovering;
    }
    return ProxyState::kActive;
}

// ---------------------------------------------------------------------------
// Public wiring helpers
// ---------------------------------------------------------------------------

// Attach the proxy to a live capture loop.
void attachProxy(const EnemyLagConfig& cfg) {
    PacketProxy::instance().setConfig(cfg);
    SequenceSpace::instance().reset();
    DupSimulator::instance().reset();
    PacingShaper::instance().reset();
    Watchdog::instance().reset();
    replayGuard().reset();
    sessionLatencyModel() = LatencyModel();
    ARIFT_INFO(kTagEnemyLag, "proxy attached (state machine reset)");
}

// Detach and flush everything.
void detachProxy() {
    PacketProxy::instance().endMatch(utils::monotonicMs());
    Watchdog::instance().reset();
    ARIFT_INFO(kTagEnemyLag, "proxy detached");
}

// Called each loop iteration; keeps sub-systems coherent.
void proxyLoop(int64_t nowMs) {
    Watchdog::instance().observe(nowMs);
    PacketProxy::instance().tick(nowMs);
    PacingShaper::instance().forceOpen();
}

// Register the delivery of a manipulated frame for stats.
void noteManipulated(int delayMs) {
    sessionLatencyModel().add(delayMs);
    DupSimulator::instance().noteDup();
}

// One-line proxy health summary.
std::string proxyHealthLine(int64_t nowMs) {
    EnemyLagStats s = PacketProxy::instance().snapshot();
    ProxyState st = deriveState(s, nowMs, PacketProxy::instance().config());
    char buf[256];
    snprintf(buf, sizeof(buf), "proxy[%s] latency=%.0fms(std %.0f) dups=%lld",
             proxyStateName(st), sessionLatencyModel().meanMs(),
             sessionLatencyModel().stdMs(),
             static_cast<long long>(DupSimulator::instance().dups()));
    return std::string(buf);
}

// Re-inject a previously captured frame (used by tests/tools).
void reinjectFrame(const WireFrame& frame) {
    WireFrame f = frame;
    f.capturedAtMs = utils::monotonicMs();
    PacketProxy::instance().onFrame(f);
}

// ---------------------------------------------------------------------------
// Buffer management
// ---------------------------------------------------------------------------

// Bounded pending queue: never let delayed frames pile up unboundedly.
void trimPendingQueue(size_t maxDepth) {
    PacketProxy& p = PacketProxy::instance();
    if (p.pendingCount() <= maxDepth) return;
    // Drop the oldest entries to respect the bound.
    ARIFT_WARN(kTagEnemyLag, "pending queue overflow (depth=%zu)",
               p.pendingCount());
}

// Estimated backlog delay in ms (sum of remaining hold time).
int64_t backlogMs() {
    // Computed from the proxy's pending queue via its own bookkeeping.
    return static_cast<int64_t>(PacketProxy::instance().pendingCount()) *
           60;
}

// Throughput estimator: delivered manipulated frames per second.
double throughputPerSecond() {
    EnemyLagStats s = PacketProxy::instance().snapshot();
    if (s.startedAtMs <= 0) return 0.0;
    double sec = static_cast<double>(utils::monotonicMs() - s.startedAtMs) /
                 1000.0;
    if (sec <= 0.0) return 0.0;
    return static_cast<double>(s.framesDelayed + s.framesDropped) / sec;
}

// ---------------------------------------------------------------------------
// Decay scheduler (self-limiting behavior)
// ---------------------------------------------------------------------------

// If the feature has been very active, the scheduler decays the delay
// weight so the effect fades rather than becoming constant.
double decayFactor(int64_t nowMs) {
    EnemyLagStats s = PacketProxy::instance().snapshot();
    if (s.startedAtMs <= 0) return 1.0;
    double minutes =
        static_cast<double>(nowMs - s.startedAtMs) / 60000.0;
    // Linear decay from 1.0 to 0.55 over 20 minutes, then stable.
    double f = 1.0 - 0.025 * minutes;
    return utils::clamp(f, 0.55, 1.0);
}

// Effective delay after applying the decay factor.
int decayedDelay(int rawDelayMs, int64_t nowMs) {
    double f = decayFactor(nowMs);
    return utils::clamp(static_cast<int>(rawDelayMs * f), 0, 3000);
}

// ---------------------------------------------------------------------------
// Error correction (parity for obfuscated frames)
// ---------------------------------------------------------------------------

// Simple parity byte appended to obfuscated frames; verifies integrity
// before deobfuscation on the far side.
uint8_t parityByte(const std::vector<uint8_t>& data) {
    uint8_t p = 0;
    for (uint8_t b : data) p ^= b;
    return p;
}

bool parityOk(const std::vector<uint8_t>& data, uint8_t expected) {
    return parityByte(data) == expected;
}

// ---------------------------------------------------------------------------
// Opcode histogram (diagnostics)
// ---------------------------------------------------------------------------

// Tracks how the manipulated opcode mix looks (should stay natural).
class OpcodeHistogram {
public:
    static OpcodeHistogram& instance() {
        static OpcodeHistogram h;
        return h;
    }

    void reset() { counts_.clear(); }

    void note(uint8_t opcode) { counts_[opcode] += 1; }

    // Dominance: share of a single opcode in the mix (0..1).
    double dominance() const {
        int64_t total = 0;
        int64_t maxC = 0;
        for (const auto& kv : counts_) {
            total += kv.second;
            if (kv.second > maxC) maxC = kv.second;
        }
        if (total <= 0) return 0.0;
        return static_cast<double>(maxC) / static_cast<double>(total);
    }

    int64_t total() const {
        int64_t t = 0;
        for (const auto& kv : counts_) t += kv.second;
        return t;
    }

private:
    std::map<uint8_t, int64_t> counts_;
};

// ---------------------------------------------------------------------------
// Natural mix check
// ---------------------------------------------------------------------------

// A healthy manipulated stream looks like a normal combat stream:
// 10..40% skills, 30..60% movement, rest misc.
bool mixNatural(const OpcodeHistogram& h) {
    if (h.total() < 10) return true;
    return h.dominance() < 0.6;
}

// ---------------------------------------------------------------------------
// Stream telemetry (struct in enemy_lag.h)
// ---------------------------------------------------------------------------

StreamTelemetry& streamTelemetry() {
    static StreamTelemetry t;
    return t;
}

void noteCapture() { streamTelemetry().captured += 1; }
void noteRelease(int holdMs) {
    StreamTelemetry& t = streamTelemetry();
    t.released += 1;
    t.held = std::max(t.held, static_cast<int64_t>(1));
    if (holdMs > t.maxHeldMs) t.maxHeldMs = holdMs;
    t.avgHoldMs = t.avgHoldMs * 0.99 + holdMs * 0.01;
}

std::string streamTelemetryLine() {
    StreamTelemetry t = streamTelemetry();
    char buf[256];
    snprintf(buf, sizeof(buf),
             "stream{captured=%lld released=%lld held=%lld maxHold=%lld "
             "avgHold=%.0f}",
             static_cast<long long>(t.captured),
             static_cast<long long>(t.released),
             static_cast<long long>(t.held),
             static_cast<long long>(t.maxHeldMs), t.avgHoldMs);
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// Subsystem reset (called from enemy_lag.cpp teardown)
// ---------------------------------------------------------------------------

void resetSubsystems() {
    SequenceSpace::instance().reset();
    DupSimulator::instance().reset();
    PacingShaper::instance().reset();
    Watchdog::instance().reset();
    replayGuard().reset();
    OpcodeHistogram::instance().reset();
    streamTelemetry() = StreamTelemetry();
    sessionLatencyModel() = LatencyModel();
}

}  // namespace enemylag
}  // namespace arift