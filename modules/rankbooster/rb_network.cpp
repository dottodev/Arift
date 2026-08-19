#include "rb_api.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "arift_utils.h"
#include "rb_math.h"

namespace arift {
namespace rb {

// ---------------------------------------------------------------------------
// RbNetwork — lobby traffic generation, shaping and RTT modelling.
// ---------------------------------------------------------------------------

LobbyPacket RbNetwork::craftJoinRequest(const PlayerProfile& p,
                                        const MatchmakingRequest& req) const {
    LobbyPacket pkt;
    pkt.opcode = 0x11;
    pkt.flags = 0x02;
    pkt.sequence = nextSequence();
    pkt.sessionId = nextSessionId();
    pkt.timestamp = static_cast<uint64_t>(utils::monotonicMs());
    pkt.payload.reserve(96);

    // Mode string (null terminated).
    const char* mode = req.mode.c_str();
    pkt.payload.insert(pkt.payload.end(), mode, mode + strlen(mode) + 1);

    // Role + party size.
    pkt.payload.push_back(static_cast<uint8_t>(req.preferredRole & 0xFF));
    pkt.payload.push_back(static_cast<uint8_t>(req.partySize & 0xFF));

    // Region bias.
    pkt.payload.push_back(static_cast<uint8_t>(req.regionBias & 0xFF));

    // MMR-adjacent placeholder (would be negotiated server-side).
    uint32_t mmrBits;
    float mmr = static_cast<float>(p.mmr);
    memcpy(&mmrBits, &mmr, 4);
    for (int i = 0; i < 4; ++i) {
        pkt.payload.push_back(static_cast<uint8_t>((mmrBits >> (i * 8)) & 0xFF));
    }

    // Rank tier placeholder.
    pkt.payload.push_back(static_cast<uint8_t>(p.rank.tier));
    pkt.payload.push_back(static_cast<uint8_t>(p.rank.stars & 0xFF));

    // Client build fingerprint placeholder.
    pkt.payload.push_back(0x2A);
    pkt.payload.push_back(0x00);

    ++packets_sent_;
    return pkt;
}

LobbyPacket RbNetwork::craftReadySignal(const PlayerProfile& p,
                                        double delayMs) const {
    LobbyPacket pkt;
    pkt.opcode = 0x12;
    pkt.flags = 0x00;
    pkt.sequence = nextSequence();
    pkt.sessionId = nextSessionId();
    pkt.timestamp = static_cast<uint64_t>(utils::monotonicMs() +
                                          static_cast<int64_t>(delayMs));
    uint32_t d = static_cast<uint32_t>(delayMs);
    for (int i = 0; i < 4; ++i) {
        pkt.payload.push_back(static_cast<uint8_t>((d >> (i * 8)) & 0xFF));
    }
    ++packets_sent_;
    return pkt;
}

LobbyPacket RbNetwork::craftPing(double rttMs) const {
    LobbyPacket pkt;
    pkt.opcode = 0x20;
    pkt.sequence = nextSequence();
    pkt.timestamp = static_cast<uint64_t>(utils::monotonicMs());
    uint32_t rtt = static_cast<uint32_t>(rttMs);
    for (int i = 0; i < 4; ++i) {
        pkt.payload.push_back(static_cast<uint8_t>((rtt >> (i * 8)) & 0xFF));
    }
    ++packets_sent_;
    return pkt;
}

LobbyPacket RbNetwork::craftHeartbeat(int64_t sessionId) const {
    LobbyPacket pkt;
    pkt.opcode = 0x21;
    pkt.sequence = nextSequence();
    pkt.sessionId = static_cast<uint32_t>(sessionId & 0xFFFFFFFF);
    pkt.timestamp = static_cast<uint64_t>(utils::monotonicMs());
    pkt.payload = {0x01};
    ++packets_sent_;
    return pkt;
}

std::vector<uint8_t> RbNetwork::shapePacket(
    const std::vector<uint8_t>& raw) const {
    // XOR obfuscation with a rotating key byte; prevents naive sniffing.
    std::vector<uint8_t> out = raw;
    uint8_t key = static_cast<uint8_t>((raw.size() * 31 + 7) & 0xFF);
    for (size_t i = 0; i < out.size(); ++i) {
        out[i] ^= static_cast<uint8_t>(key + i);
    }
    ++packets_shaped_;
    return out;
}

std::vector<uint8_t> RbNetwork::unshapePacket(
    const std::vector<uint8_t>& shaped) const {
    std::vector<uint8_t> out = shaped;
    uint8_t key = static_cast<uint8_t>((shaped.size() * 31 + 7) & 0xFF);
    for (size_t i = 0; i < out.size(); ++i) {
        out[i] ^= static_cast<uint8_t>(key + i);
    }
    return out;
}

double RbNetwork::simulateRtt(double baseMs, double jitterMs) const {
    return rb_utils::clamp(baseMs + rb_utils::gaussianRandom(0.0, jitterMs),
                           5.0, 400.0);
}

uint32_t RbNetwork::nextSessionId() const {
    ++session_counter_;
    return session_counter_;
}

uint16_t RbNetwork::nextSequence() const {
    ++seq_counter_;
    if (seq_counter_ == 0) seq_counter_ = 1;
    return seq_counter_;
}

// ---------------------------------------------------------------------------
// Traffic shaping helpers
// ---------------------------------------------------------------------------

namespace {

// Human-like inter-packet gap: gamma-ish distribution with floor.
double humanizedGapMs(double meanGapMs, double variance) {
    double gap = rb_utils::gaussianRandom(meanGapMs, variance);
    return rb_utils::clamp(gap, meanGapMs * 0.3, meanGapMs * 3.0);
}

}  // namespace

// Simulated packet burst schedule for a lobby interaction window.
std::vector<double> buildBurstSchedule(double windowMs, double baseGapMs) {
    std::vector<double> schedule;
    double t = 0.0;
    while (t < windowMs) {
        schedule.push_back(t);
        t += humanizedGapMs(baseGapMs, baseGapMs * 0.35);
    }
    return schedule;
}

// Estimate the entropy of a payload stream (higher = more organic).
double payloadEntropy(const std::vector<uint8_t>& data) {
    if (data.empty()) return 0.0;
    int counts[256] = {0};
    for (uint8_t b : data) counts[b] += 1;
    double h = 0.0;
    double n = static_cast<double>(data.size());
    for (int c : counts) {
        if (c == 0) continue;
        double p = static_cast<double>(c) / n;
        h -= p * std::log2(p);
    }
    return h;
}

// Pad a payload to a target length with entropy-preserving filler.
std::vector<uint8_t> padToLength(const std::vector<uint8_t>& data,
                                 size_t targetLen) {
    std::vector<uint8_t> out = data;
    if (out.size() >= targetLen) return out;
    uint32_t state = 0x9E3779B9u;
    while (out.size() < targetLen) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        out.push_back(static_cast<uint8_t>(state & 0xFF));
    }
    return out;
}

// Validate a received packet's framing sanity.
bool validatePacketShape(const LobbyPacket& pkt) {
    if (pkt.payload.size() > 512) return false;
    if (pkt.sequence == 0) return false;
    return true;
}

// ---------------------------------------------------------------------------
// RTT / latency modelling
// ---------------------------------------------------------------------------

namespace {

// EWM of recent RTT samples.
struct RttEstimator {
    double ewm = 60.0;
    double variance = 20.0;
    int samples = 0;

    void sample(double rtt) {
        if (samples == 0) {
            ewm = rtt;
            variance = 5.0;
        } else {
            double alpha = 0.15;
            double prev = ewm;
            ewm = alpha * rtt + (1.0 - alpha) * prev;
            variance = alpha * (rtt - prev) * (rtt - prev) +
                       (1.0 - alpha) * variance;
        }
        samples += 1;
    }

    double jittered() const {
        return rb_utils::clamp(
            ewm + rb_utils::gaussianRandom(0.0, std::sqrt(variance)),
            5.0, 500.0);
    }
};

}  // namespace

// Model RTT for a session with typical MLBB-like characteristics.
std::vector<double> modelRttSeries(int samples, double baseMs, double jitterMs,
                                   int64_t seed) {
    (void)seed;
    std::vector<double> out;
    out.reserve(static_cast<size_t>(samples));
    RttEstimator est;
    for (int i = 0; i < samples; ++i) {
        double rtt = rb_utils::clamp(
            baseMs + rb_utils::gaussianRandom(0.0, jitterMs), 5.0, 500.0);
        est.sample(rtt);
        out.push_back(est.jittered());
    }
    return out;
}

// Bandwidth estimate from packet sizes and inter-arrival gaps.
double estimateBandwidthKbps(const std::vector<size_t>& packetSizes,
                             const std::vector<double>& gapsMs) {
    if (packetSizes.empty() || gapsMs.empty()) return 0.0;
    double bytes = 0.0;
    double seconds = 0.0;
    size_t n = std::min(packetSizes.size(), gapsMs.size());
    for (size_t i = 0; i < n; ++i) {
        bytes += static_cast<double>(packetSizes[i]);
        seconds += gapsMs[i] / 1000.0;
    }
    if (seconds <= 0.0) return 0.0;
    return bytes * 8.0 / 1000.0 / seconds;
}

// ---------------------------------------------------------------------------
// Traffic classification
// ---------------------------------------------------------------------------

// Classify an opcode into a traffic category.
const char* trafficCategory(uint8_t opcode) {
    switch (opcode) {
        case 0x10: case 0x11: case 0x12:
            return "join";
        case 0x20: case 0x21:
            return "keepalive";
        case 0x30: case 0x31:
            return "lobby";
        case 0x40: case 0x41:
            return "match";
        default:
            return "misc";
    }
}

// Per-category pacing profile (mean gap ms, jitter ms).
void pacingProfile(const char* category, double& meanGapMs,
                   double& jitterMs) {
    if (strcmp(category, "join") == 0) {
        meanGapMs = 800.0;
        jitterMs = 250.0;
    } else if (strcmp(category, "keepalive") == 0) {
        meanGapMs = 30000.0;
        jitterMs = 5000.0;
    } else if (strcmp(category, "lobby") == 0) {
        meanGapMs = 1500.0;
        jitterMs = 400.0;
    } else if (strcmp(category, "match") == 0) {
        meanGapMs = 500.0;
        jitterMs = 150.0;
    } else {
        meanGapMs = 2000.0;
        jitterMs = 600.0;
    }
}

// Build a realistic paced send schedule for a list of packet types.
std::vector<int64_t> buildSendSchedule(const std::vector<uint8_t>& opcodes,
                                       int64_t startMs) {
    std::vector<int64_t> out;
    int64_t t = startMs;
    for (uint8_t op : opcodes) {
        const char* cat = trafficCategory(op);
        double meanGap;
        double jitterMs;
        pacingProfile(cat, meanGap, jitterMs);
        double gap = rb_utils::clamp(
            rb_utils::gaussianRandom(meanGap, jitterMs), 50.0, 60000.0);
        t += static_cast<int64_t>(gap);
        out.push_back(t);
    }
    return out;
}

// Normalize timestamps relative to a session anchor (for telemetry).
std::vector<int64_t> relativeOffsets(const std::vector<int64_t>& absTimes,
                                     int64_t anchorMs) {
    std::vector<int64_t> out;
    for (int64_t t : absTimes) out.push_back(t - anchorMs);
    return out;
}

// ---------------------------------------------------------------------------
// Session traffic simulation
// ---------------------------------------------------------------------------

// Simulate one lobby interaction burst (join -> ready -> heartbeat).
std::vector<LobbyPacket> simulateLobbyBurst(const PlayerProfile& p,
                                            const MatchmakingRequest& req,
                                            const RbNetwork& net) {
    std::vector<LobbyPacket> burst;
    burst.push_back(net.craftJoinRequest(p, req));
    burst.push_back(net.craftReadySignal(p, rb_utils::gaussianRandom(900.0, 200.0)));
    burst.push_back(net.craftPing(rb_utils::gaussianRandom(40.0, 12.0)));
    for (int i = 0; i < 3; ++i) {
        burst.push_back(net.craftHeartbeat(static_cast<int64_t>(burst[0].sessionId)));
    }
    return burst;
}

// Serialize a burst into wire frames.
std::vector<std::vector<uint8_t>> wireBurst(const ProtocolCodec& codec,
                                            const std::vector<LobbyPacket>& burst) {
    std::vector<std::vector<uint8_t>> frames;
    for (const auto& pkt : burst) {
        frames.push_back(wireFrame(codec, pkt));
    }
    return frames;
}

// ---------------------------------------------------------------------------
// Traffic replay and validation
// ---------------------------------------------------------------------------

// Replay a packet burst against expected pacing; returns anomaly count.
int replayAnomalies(const std::vector<int64_t>& sendTimes,
                    const std::vector<double>& expectedGapsMs) {
    if (sendTimes.size() < 2) return 0;
    int anomalies = 0;
    size_t n = std::min(sendTimes.size() - 1, expectedGapsMs.size());
    for (size_t i = 0; i < n; ++i) {
        double gap = static_cast<double>(sendTimes[i + 1] - sendTimes[i]);
        double expected = expectedGapsMs[i];
        if (expected > 0.0 && (gap < expected * 0.4 || gap > expected * 3.0)) {
            anomalies += 1;
        }
    }
    return anomalies;
}

// Validate a session's opcode mix (no suspicious uniformity).
bool sessionMixOrganic(const std::vector<uint8_t>& opcodes) {
    if (opcodes.size() < 8) return true;
    std::map<uint8_t, int> counts;
    for (uint8_t op : opcodes) counts[op] += 1;
    int distinct = static_cast<int>(counts.size());
    // Organic sessions have several distinct packet types.
    if (distinct < 3) return false;
    // No single type dominating >90%.
    for (const auto& kv : counts) {
        if (static_cast<double>(kv.second) / static_cast<double>(opcodes.size()) > 0.9) {
            return false;
        }
    }
    return true;
}

// Inter-packet gap distribution stats for a send log.
struct GapStats {
    double meanMs = 0.0;
    double sdMs = 0.0;
    double minMs = 0.0;
    double maxMs = 0.0;
    int samples = 0;
};

GapStats gapStats(const std::vector<int64_t>& sendTimes) {
    GapStats g;
    if (sendTimes.size() < 2) return g;
    std::vector<double> gaps;
    for (size_t i = 1; i < sendTimes.size(); ++i) {
        gaps.push_back(static_cast<double>(sendTimes[i] - sendTimes[i - 1]));
    }
    g.meanMs = rb_utils::mean(gaps);
    g.sdMs = rb_utils::standardDeviation(gaps);
    g.minMs = *std::min_element(gaps.begin(), gaps.end());
    g.maxMs = *std::max_element(gaps.begin(), gaps.end());
    g.samples = static_cast<int>(gaps.size());
    return g;
}

// Jitter ratio: sd/mean (organic traffic sits ~0.3-0.6).
double jitterRatio(const GapStats& g) {
    if (g.meanMs <= 0.0) return 0.0;
    return g.sdMs / g.meanMs;
}

// Throttle schedule: exponential backoff after N sends.
std::vector<int64_t> throttleSchedule(int64_t startMs, int sends,
                                      double baseGapMs) {
    std::vector<int64_t> out;
    int64_t t = startMs;
    for (int i = 0; i < sends; ++i) {
        out.push_back(t);
        double gap = baseGapMs * std::pow(1.4, static_cast<double>(i));
        t += static_cast<int64_t>(rb_utils::clamp(gap, 100.0, 30000.0));
    }
    return out;
}

// Session signature: hash of opcode+size+timing (for consistency checks).
uint64_t sessionSignature(const std::vector<LobbyPacket>& burst) {
    uint64_t h = 1469598103934665603ULL;
    for (const auto& pkt : burst) {
        h ^= static_cast<uint64_t>(pkt.opcode);
        h *= 1099511628211ULL;
        h ^= static_cast<uint64_t>(pkt.payload.size());
        h *= 1099511628211ULL;
        h ^= pkt.sequence;
        h *= 1099511628211ULL;
    }
    return h;
}

}  // namespace rb
}  // namespace arift