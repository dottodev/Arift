#include "rb_api.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "rb_math.h"

namespace arift {
namespace rb {

// ---------------------------------------------------------------------------
// ProtocolCodec — packet framing, checksums, obfuscation.
// ---------------------------------------------------------------------------

namespace {

constexpr uint16_t kMagic = 0xAF41;
constexpr uint8_t kVersion = 1;

}  // namespace

uint32_t ProtocolCodec::crc32(const uint8_t* data, size_t len) const {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) {
            uint32_t mask = static_cast<uint32_t>(-(static_cast<int>(crc & 1)));
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

uint32_t ProtocolCodec::checksum(const std::vector<uint8_t>& data) const {
    uint32_t sum = crc32(data.data(), data.size());
    sum ^= key_;
    return sum;
}

std::vector<uint8_t> ProtocolCodec::obfuscate(
    const std::vector<uint8_t>& data) const {
    std::vector<uint8_t> out = data;
    uint32_t state = key_ ^ 0x9E3779B9u;
    for (size_t i = 0; i < out.size(); ++i) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        out[i] ^= static_cast<uint8_t>(state >> 24);
    }
    return out;
}

std::vector<uint8_t> ProtocolCodec::deobfuscate(
    const std::vector<uint8_t>& data) const {
    // XOR is symmetric.
    return obfuscate(data);
}

FramedPacket ProtocolCodec::encode(const LobbyPacket& pkt) const {
    FramedPacket fp;
    fp.magic = kMagic;
    fp.version = kVersion;
    fp.type = pkt.opcode;
    fp.payload = pkt.payload;
    fp.payloadLen = static_cast<uint16_t>(pkt.payload.size());
    // Compute checksum over payload + session id + sequence.
    std::vector<uint8_t> tmp;
    tmp.reserve(pkt.payload.size() + 8);
    tmp = pkt.payload;
    for (int i = 0; i < 4; ++i) {
        tmp.push_back(static_cast<uint8_t>((pkt.sessionId >> (i * 8)) & 0xFF));
    }
    for (int i = 0; i < 2; ++i) {
        tmp.push_back(static_cast<uint8_t>((pkt.sequence >> (i * 8)) & 0xFF));
    }
    fp.checksum = checksum(tmp);
    fp.payload = obfuscate(fp.payload);
    ++counter_;
    return fp;
}

LobbyPacket ProtocolCodec::decode(const FramedPacket& framed) const {
    LobbyPacket pkt;
    pkt.opcode = framed.type;
    pkt.payload = deobfuscate(framed.payload);
    // Rebuild the checksum input to validate integrity (payload only here).
    std::vector<uint8_t> tmp = pkt.payload;
    for (int i = 0; i < 4; ++i) {
        tmp.push_back(static_cast<uint8_t>((pkt.sessionId >> (i * 8)) & 0xFF));
    }
    for (int i = 0; i < 2; ++i) {
        tmp.push_back(static_cast<uint8_t>((pkt.sequence >> (i * 8)) & 0xFF));
    }
    // Integrity check is informational for simulation.
    (void)tmp;
    return pkt;
}

std::vector<uint8_t> ProtocolCodec::frameRaw(
    const std::vector<uint8_t>& payload, uint8_t type) const {
    FramedPacket fp;
    fp.magic = kMagic;
    fp.version = kVersion;
    fp.type = type;
    fp.payload = payload;
    fp.payloadLen = static_cast<uint16_t>(payload.size());
    fp.checksum = checksum(payload);
    fp.payload = obfuscate(payload);

    std::vector<uint8_t> out;
    out.reserve(12 + fp.payload.size());
    out.push_back(static_cast<uint8_t>(fp.magic & 0xFF));
    out.push_back(static_cast<uint8_t>(fp.magic >> 8));
    out.push_back(fp.version);
    out.push_back(fp.type);
    out.push_back(static_cast<uint8_t>(fp.payloadLen & 0xFF));
    out.push_back(static_cast<uint8_t>(fp.payloadLen >> 8));
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<uint8_t>((fp.checksum >> (i * 8)) & 0xFF));
    }
    out.insert(out.end(), fp.payload.begin(), fp.payload.end());
    return out;
}

FramedPacket ProtocolCodec::parse(const std::vector<uint8_t>& bytes) const {
    FramedPacket fp;
    if (bytes.size() < 12) return fp;
    fp.magic = static_cast<uint16_t>(bytes[0] | (bytes[1] << 8));
    fp.version = bytes[2];
    fp.type = bytes[3];
    fp.payloadLen = static_cast<uint16_t>(bytes[4] | (bytes[5] << 8));
    for (int i = 0; i < 4; ++i) {
        fp.checksum |= static_cast<uint32_t>(bytes[6 + i]) << (i * 8);
    }
    if (bytes.size() < 12 + fp.payloadLen) return fp;
    fp.payload.assign(bytes.begin() + 12, bytes.begin() + 12 + fp.payloadLen);
    return fp;
}

std::string ProtocolCodec::dump(const FramedPacket& fp) const {
    char buf[256];
    snprintf(buf, sizeof(buf),
             "framed: magic=0x%04X ver=%u type=0x%02X len=%u crc=0x%08X",
             fp.magic, fp.version, fp.type, fp.payloadLen, fp.checksum);
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// Packet-level helpers
// ---------------------------------------------------------------------------

// Build a full wire frame for a crafted lobby packet.
std::vector<uint8_t> wireFrame(const ProtocolCodec& codec,
                               const LobbyPacket& pkt) {
    FramedPacket fp = codec.encode(pkt);
    std::vector<uint8_t> out;
    out.reserve(12 + fp.payload.size());
    out.push_back(static_cast<uint8_t>(fp.magic & 0xFF));
    out.push_back(static_cast<uint8_t>(fp.magic >> 8));
    out.push_back(fp.version);
    out.push_back(fp.type);
    out.push_back(static_cast<uint8_t>(fp.payloadLen & 0xFF));
    out.push_back(static_cast<uint8_t>(fp.payloadLen >> 8));
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<uint8_t>((fp.checksum >> (i * 8)) & 0xFF));
    }
    out.insert(out.end(), fp.payload.begin(), fp.payload.end());
    return out;
}

// Extract opcode from a raw wire frame.
uint8_t wireOpcode(const std::vector<uint8_t>& bytes) {
    if (bytes.size() < 12) return 0;
    return bytes[3];
}

// Verify a wire frame's checksum.
bool wireValid(const ProtocolCodec& codec,
               const std::vector<uint8_t>& bytes) {
    FramedPacket fp = codec.parse(bytes);
    if (fp.magic != 0xAF41 || fp.version != 1) return false;
    if (bytes.size() < 12 + fp.payloadLen) return false;
    std::vector<uint8_t> payload = codec.deobfuscate(fp.payload);
    return codec.checksum(payload) == fp.checksum;
}

// ---------------------------------------------------------------------------
// Packet catalog / semantic decoding
// ---------------------------------------------------------------------------

namespace {

// Opcode -> human readable name table.
const char* opcodeName(uint8_t op) {
    switch (op) {
        case 0x10: return "LOBBY_LIST";
        case 0x11: return "JOIN_REQUEST";
        case 0x12: return "READY_SIGNAL";
        case 0x13: return "CANCEL_SEARCH";
        case 0x20: return "PING";
        case 0x21: return "HEARTBEAT";
        case 0x22: return "SESSION_SYNC";
        case 0x30: return "ROOM_UPDATE";
        case 0x31: return "MEMBER_JOIN";
        case 0x32: return "MEMBER_LEAVE";
        case 0x40: return "MATCH_CONFIRM";
        case 0x41: return "MATCH_LOADING";
        case 0x50: return "RANK_SYNC";
        case 0x51: return "MMR_SYNC";
        default: return "UNKNOWN";
    }
}

}  // namespace

// Decode a wire frame into a semantic description string.
std::string describeWire(const ProtocolCodec& codec,
                         const std::vector<uint8_t>& bytes) {
    if (bytes.size() < 12) return "<short>";
    FramedPacket fp = codec.parse(bytes);
    char buf[192];
    snprintf(buf, sizeof(buf), "%s(type=0x%02X len=%u)",
             opcodeName(fp.type), fp.type, fp.payloadLen);
    return std::string(buf);
}

// Extract a u32 field from a payload (little-endian) at an offset.
bool payloadU32(const std::vector<uint8_t>& payload, size_t offset,
                uint32_t& out) {
    if (offset + 4 > payload.size()) return false;
    out = 0;
    for (size_t i = 0; i < 4; ++i) {
        out |= static_cast<uint32_t>(payload[offset + i]) << (i * 8);
    }
    return true;
}

// Extract a float field from a payload at an offset.
bool payloadFloat(const std::vector<uint8_t>& payload, size_t offset,
                  float& out) {
    uint32_t bits;
    if (!payloadU32(payload, offset, bits)) return false;
    memcpy(&out, &bits, sizeof(out));
    return true;
}

// Extract a null-terminated string from a payload at an offset.
std::string payloadString(const std::vector<uint8_t>& payload, size_t offset) {
    if (offset >= payload.size()) return "";
    size_t end = offset;
    while (end < payload.size() && payload[end] != 0) end += 1;
    return std::string(reinterpret_cast<const char*>(payload.data() + offset),
                       end - offset);
}

// Categorize a frame type into a simple state-machine event.
int frameToEvent(const FramedPacket& fp) {
    switch (fp.type) {
        case 0x11: return 1;   // queued
        case 0x40: return 2;   // matched
        case 0x41: return 3;   // loading
        case 0x50: return 4;   // rank sync
        default: return 0;     // keepalive / misc
    }
}

// ---------------------------------------------------------------------------
// Framing statistics
// ---------------------------------------------------------------------------

struct FrameStats {
    uint64_t total = 0;
    uint64_t keepalive = 0;
    uint64_t joins = 0;
    uint64_t room = 0;
    uint64_t match = 0;
    double avgPayload = 0.0;
    double maxPayload = 0.0;
};

FrameStats accumulateStats(const std::vector<FramedPacket>& frames) {
    FrameStats s;
    double sumPayload = 0.0;
    for (const auto& fp : frames) {
        s.total += 1;
        double pl = static_cast<double>(fp.payloadLen);
        sumPayload += pl;
        if (pl > s.maxPayload) s.maxPayload = pl;
        const char* cat = trafficCategory(fp.type);
        if (strcmp(cat, "keepalive") == 0) s.keepalive += 1;
        else if (strcmp(cat, "join") == 0) s.joins += 1;
        else if (strcmp(cat, "lobby") == 0) s.room += 1;
        else if (strcmp(cat, "match") == 0) s.match += 1;
    }
    if (s.total > 0) s.avgPayload = sumPayload / static_cast<double>(s.total);
    return s;
}

// Opcode histogram for a session (for organic-traffic verification).
std::vector<int> opcodeHistogram(const std::vector<FramedPacket>& frames) {
    std::vector<int> hist(256, 0);
    for (const auto& fp : frames) {
        hist[static_cast<size_t>(fp.type)] += 1;
    }
    return hist;
}

// ---------------------------------------------------------------------------
// Encryption variants
// ---------------------------------------------------------------------------

// Rot13-style XOR cipher with per-session offset.
std::vector<uint8_t> xorRotate(const std::vector<uint8_t>& data,
                               uint8_t offset) {
    std::vector<uint8_t> out = data;
    for (size_t i = 0; i < out.size(); ++i) {
        out[i] ^= static_cast<uint8_t>(offset + i * 7);
    }
    return out;
}

// Byte-shuffle obfuscation with a fixed permutation (reversible).
std::vector<uint8_t> shuffleBytes(const std::vector<uint8_t>& data) {
    std::vector<uint8_t> out = data;
    size_t n = out.size();
    if (n < 2) return out;
    // Even positions first, then odd (simple reversible shuffle).
    std::vector<uint8_t> evens;
    std::vector<uint8_t> odds;
    for (size_t i = 0; i < n; ++i) {
        if (i % 2 == 0) evens.push_back(out[i]);
        else odds.push_back(out[i]);
    }
    evens.insert(evens.end(), odds.begin(), odds.end());
    return evens;
}

// Un-shuffle (inverse of shuffleBytes).
std::vector<uint8_t> unshuffleBytes(const std::vector<uint8_t>& data) {
    std::vector<uint8_t> out(data.size(), 0);
    size_t n = out.size();
    if (n < 2) return data;
    size_t evens = (n + 1) / 2;
    for (size_t i = 0; i < evens; ++i) out[i * 2] = data[i];
    for (size_t i = 0; i + evens < n; ++i) out[i * 2 + 1] = data[i + evens];
    return out;
}

// Length-prefixed encoding for payload segments.
std::vector<uint8_t> prefixLength(const std::vector<uint8_t>& segment) {
    std::vector<uint8_t> out;
    out.push_back(static_cast<uint8_t>(segment.size() & 0xFF));
    out.push_back(static_cast<uint8_t>((segment.size() >> 8) & 0xFF));
    out.insert(out.end(), segment.begin(), segment.end());
    return out;
}

// Session-key rotation schedule (time -> key offset).
uint32_t keyForTime(int64_t ms) {
    uint64_t bucket = static_cast<uint64_t>(ms / 60000);
    uint32_t k = static_cast<uint32_t>(bucket * 2654435761u);
    return k | 1u;
}

// Header validation helper.
bool headerValid(const FramedPacket& fp) {
    if (fp.magic != 0xAF41) return false;
    if (fp.version != 1) return false;
    if (fp.payloadLen > 1024) return false;
    return true;
}

// Payload capacity planning: worst-case frame size for a payload.
size_t worstCaseFrameSize(size_t payloadBytes) {
    return 12 + payloadBytes + 16;   // header + payload + slack
}

// Packet type summary for a frame batch.
std::string typeSummary(const std::vector<FramedPacket>& frames) {
    std::vector<int> hist = opcodeHistogram(frames);
    std::string out;
    int shown = 0;
    for (size_t i = 0; i < hist.size() && shown < 8; ++i) {
        if (hist[i] > 0) {
            char buf[64];
            snprintf(buf, sizeof(buf), "%s:%d ",
                     opcodeName(static_cast<uint8_t>(i)), hist[i]);
            out += buf;
            shown += 1;
        }
    }
    if (out.empty()) out = "none";
    return out;
}

}  // namespace rb
}  // namespace arift