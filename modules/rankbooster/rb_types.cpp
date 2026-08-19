#include "rb_types.h"

#include <cstdio>
#include <cstring>

#include "arift_utils.h"

namespace arift {
namespace rb {

std::string RankPoint::toString() const {
    static const char* names[] = {
        "Warrior", "Elite", "Master", "Grandmaster", "Epic",
        "Legend", "Mythic", "Mythical Glory", "Mythical Immortal",
    };
    int idx = static_cast<int>(tier);
    if (idx < 0 || idx >= static_cast<int>(RankTier::kCount)) {
        idx = 0;
    }
    char buf[128];
    if (tier >= RankTier::kMythic) {
        snprintf(buf, sizeof(buf), "%s (%d)", names[idx], stars);
    } else {
        snprintf(buf, sizeof(buf), "%s %d", names[idx], stars);
    }
    return std::string(buf);
}

RankPoint RankPoint::fromAbsolute(int abs) {
    RankPoint rp;
    int tier = abs / 100;
    rp.tier = static_cast<RankTier>(tier < 0 ? 0 : (tier >= static_cast<int>(RankTier::kCount)
                                                        ? static_cast<int>(RankTier::kCount) - 1
                                                        : tier));
    rp.stars = abs - tier * 100;
    if (rp.stars < 0) rp.stars = 0;
    return rp;
}

std::string serializeRank(const RankPoint& rp) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%d:%d:%d", static_cast<int>(rp.tier), rp.stars,
             rp.protection);
    return std::string(buf);
}

RankPoint deserializeRank(const std::string& s) {
    RankPoint rp;
    auto parts = utils::split(s, ':');
    if (parts.size() >= 1) {
        rp.tier = static_cast<RankTier>(std::atoi(parts[0].c_str()));
    }
    if (parts.size() >= 2) {
        rp.stars = std::atoi(parts[1].c_str());
    }
    if (parts.size() >= 3) {
        rp.protection = std::atoi(parts[2].c_str());
    }
    return rp;
}

std::string serializeMatch(const MatchRecord& rec) {
    char buf[512];
    snprintf(buf, sizeof(buf),
             "id=%lld;start=%lld;end=%lld;won=%d;ranked=%d;k=%d;d=%d;a=%d;mvp=%d;"
             "team=%d;enemy=%d;dur=%.1f;perc=%d;rankBefore=%s;rankAfter=%s;"
             "mmrDelta=%.2f;skill=%.2f;conf=%.3f;mode=%s",
             static_cast<long long>(rec.matchId),
             static_cast<long long>(rec.startedAtMs),
             static_cast<long long>(rec.endedAtMs),
             rec.result.won ? 1 : 0,
             rec.result.ranked ? 1 : 0,
             rec.result.kdaKills,
             rec.result.kdaDeaths,
             rec.result.kdaAssists,
             rec.result.mvpScore,
             rec.result.teamScore,
             rec.result.enemyScore,
             rec.result.durationMin,
             rec.result.performancePercentile,
             serializeRank(rec.rankBefore).c_str(),
             serializeRank(rec.rankAfter).c_str(),
             rec.mmrDelta,
             rec.skillRating,
             rec.confidence,
             rec.mode.c_str());
    return std::string(buf);
}

MatchRecord deserializeMatch(const std::string& s) {
    MatchRecord rec;
    for (const auto& part : utils::split(s, ';')) {
        auto eq = part.find('=');
        if (eq == std::string::npos) continue;
        std::string key = part.substr(0, eq);
        std::string val = part.substr(eq + 1);
        if (key == "id") rec.matchId = std::atoll(val.c_str());
        else if (key == "start") rec.startedAtMs = std::atoll(val.c_str());
        else if (key == "end") rec.endedAtMs = std::atoll(val.c_str());
        else if (key == "won") rec.result.won = val == "1";
        else if (key == "ranked") rec.result.ranked = val == "1";
        else if (key == "k") rec.result.kdaKills = std::atoi(val.c_str());
        else if (key == "d") rec.result.kdaDeaths = std::atoi(val.c_str());
        else if (key == "a") rec.result.kdaAssists = std::atoi(val.c_str());
        else if (key == "mvp") rec.result.mvpScore = std::atoi(val.c_str());
        else if (key == "team") rec.result.teamScore = std::atoi(val.c_str());
        else if (key == "enemy") rec.result.enemyScore = std::atoi(val.c_str());
        else if (key == "dur") rec.result.durationMin = std::atof(val.c_str());
        else if (key == "perc") rec.result.performancePercentile = std::atoi(val.c_str());
        else if (key == "rankBefore") rec.rankBefore = deserializeRank(val);
        else if (key == "rankAfter") rec.rankAfter = deserializeRank(val);
        else if (key == "mmrDelta") rec.mmrDelta = std::atof(val.c_str());
        else if (key == "skill") rec.skillRating = std::atof(val.c_str());
        else if (key == "conf") rec.confidence = std::atof(val.c_str());
        else if (key == "mode") rec.mode = val;
    }
    return rec;
}

// ---------------------------------------------------------------------------
// Compact binary serialization (for caches/telemetry)
// ---------------------------------------------------------------------------

namespace {

void pushU16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

void pushU32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
    }
}

void pushI64(std::vector<uint8_t>& out, int64_t v) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<uint8_t>((static_cast<uint64_t>(v) >> (i * 8)) & 0xFF));
    }
}

uint16_t popU16(const std::vector<uint8_t>& in, size_t& pos) {
    uint16_t v = 0;
    if (pos + 2 > in.size()) return 0;
    v = static_cast<uint16_t>(in[pos]) | static_cast<uint16_t>(in[pos + 1]) << 8;
    pos += 2;
    return v;
}

int64_t popI64(const std::vector<uint8_t>& in, size_t& pos) {
    int64_t v = 0;
    for (int i = 0; i < 8 && pos < in.size(); ++i, ++pos) {
        v |= static_cast<int64_t>(in[pos]) << (i * 8);
    }
    return v;
}

}  // namespace

// Encode a match record into a compact byte vector.
std::vector<uint8_t> encodeMatchBinary(const MatchRecord& rec) {
    std::vector<uint8_t> out;
    pushI64(out, rec.matchId);
    pushI64(out, rec.startedAtMs);
    pushI64(out, rec.endedAtMs);
    out.push_back(rec.result.won ? 1 : 0);
    out.push_back(rec.result.ranked ? 1 : 0);
    pushU16(out, static_cast<uint16_t>(rec.result.kdaKills));
    pushU16(out, static_cast<uint16_t>(rec.result.kdaDeaths));
    pushU16(out, static_cast<uint16_t>(rec.result.kdaAssists));
    pushU16(out, static_cast<uint16_t>(rec.result.mvpScore));
    pushU16(out, static_cast<uint16_t>(rec.result.teamScore));
    pushU16(out, static_cast<uint16_t>(rec.result.enemyScore));
    pushU32(out, static_cast<uint32_t>(rec.result.durationMin * 10.0));
    out.push_back(static_cast<uint8_t>(rec.result.performancePercentile));
    pushU16(out, static_cast<uint16_t>(rec.rankBefore.absolute()));
    pushU16(out, static_cast<uint16_t>(rec.rankAfter.absolute()));
    pushI64(out, static_cast<int64_t>(rec.mmrDelta * 100.0));
    return out;
}

// Decode a match record from a compact byte vector.
MatchRecord decodeMatchBinary(const std::vector<uint8_t>& in) {
    MatchRecord rec;
    size_t pos = 0;
    rec.matchId = popI64(in, pos);
    rec.startedAtMs = popI64(in, pos);
    rec.endedAtMs = popI64(in, pos);
    if (pos < in.size()) rec.result.won = in[pos++] == 1;
    if (pos < in.size()) rec.result.ranked = in[pos++] == 1;
    rec.result.kdaKills = popU16(in, pos);
    rec.result.kdaDeaths = popU16(in, pos);
    rec.result.kdaAssists = popU16(in, pos);
    rec.result.mvpScore = popU16(in, pos);
    rec.result.teamScore = popU16(in, pos);
    rec.result.enemyScore = popU16(in, pos);
    rec.result.durationMin = static_cast<double>(popU16(in, pos)) / 10.0;
    if (pos < in.size()) rec.result.performancePercentile = in[pos++];
    rec.rankBefore = RankPoint::fromAbsolute(popU16(in, pos));
    rec.rankAfter = RankPoint::fromAbsolute(popU16(in, pos));
    rec.mmrDelta = static_cast<double>(popI64(in, pos)) / 100.0;
    return rec;
}

// Encode a profile into a compact byte vector.
std::vector<uint8_t> encodeProfileBinary(const PlayerProfile& p) {
    std::vector<uint8_t> out;
    out.push_back(static_cast<uint8_t>(p.rank.tier));
    out.push_back(static_cast<uint8_t>(p.rank.stars & 0xFF));
    pushU16(out, static_cast<uint16_t>(p.rank.protection));
    uint32_t mmrBits;
    float mmr = static_cast<float>(p.mmr);
    memcpy(&mmrBits, &mmr, 4);
    pushU32(out, mmrBits);
    uint32_t sigmaBits;
    float sigma = static_cast<float>(p.sigma);
    memcpy(&sigmaBits, &sigma, 4);
    pushU32(out, sigmaBits);
    pushI64(out, p.matchesPlayed);
    pushI64(out, p.matchesWon);
    pushI64(out, p.matchesLost);
    pushI64(out, p.streak);
    return out;
}

// Decode a profile from a compact byte vector.
PlayerProfile decodeProfileBinary(const std::vector<uint8_t>& in) {
    PlayerProfile p;
    size_t pos = 0;
    if (pos < in.size()) p.rank.tier = static_cast<RankTier>(in[pos++]);
    if (pos < in.size()) p.rank.stars = in[pos++];
    p.rank.protection = popU16(in, pos);
    uint32_t mmrBits = 0;
    for (int i = 0; i < 4 && pos < in.size(); ++i, ++pos) {
        mmrBits |= static_cast<uint32_t>(in[pos]) << (i * 8);
    }
    memcpy(&p.mmr, &mmrBits, 4);
    uint32_t sigmaBits = 0;
    for (int i = 0; i < 4 && pos < in.size(); ++i, ++pos) {
        sigmaBits |= static_cast<uint32_t>(in[pos]) << (i * 8);
    }
    memcpy(&p.sigma, &sigmaBits, 4);
    p.matchesPlayed = popI64(in, pos);
    p.matchesWon = popI64(in, pos);
    p.matchesLost = popI64(in, pos);
    p.streak = popI64(in, pos);
    return p;
}

// ---------------------------------------------------------------------------
// Type helpers
// ---------------------------------------------------------------------------

// Short code for a rank tier (used in compact logs).
const char* tierCode(RankTier tier) {
    switch (tier) {
        case RankTier::kWarrior: return "WR";
        case RankTier::kElite: return "EL";
        case RankTier::kMaster: return "MA";
        case RankTier::kGrandmaster: return "GM";
        case RankTier::kEpic: return "EP";
        case RankTier::kLegend: return "LG";
        case RankTier::kMythic: return "MY";
        case RankTier::kMythicalGlory: return "MG";
        case RankTier::kMythicalImmortal: return "MI";
        default: return "??";
    }
}

// Human-readable aggression name.
const char* aggressionName(AggressionLevel level) {
    switch (level) {
        case AggressionLevel::kConservative: return "conservative";
        case AggressionLevel::kBalanced: return "balanced";
        case AggressionLevel::kAggressive: return "aggressive";
        case AggressionLevel::kExtreme: return "extreme";
        default: return "unknown";
    }
}

// Human-readable booster state name.
const char* boosterStateName(BoosterState state) {
    switch (state) {
        case BoosterState::kIdle: return "idle";
        case BoosterState::kCollecting: return "collecting";
        case BoosterState::kAnalyzing: return "analyzing";
        case BoosterState::kTuning: return "tuning";
        case BoosterState::kReady: return "ready";
        case BoosterState::kActive: return "active";
        case BoosterState::kPaused: return "paused";
        case BoosterState::kError: return "error";
        default: return "unknown";
    }
}

// Parse a tier from a short code (case-insensitive).
bool parseTierCode(const std::string& code, RankTier& out) {
    std::string c = utils::toUpper(code);
    if (c == "WR") out = RankTier::kWarrior;
    else if (c == "EL") out = RankTier::kElite;
    else if (c == "MA") out = RankTier::kMaster;
    else if (c == "GM") out = RankTier::kGrandmaster;
    else if (c == "EP") out = RankTier::kEpic;
    else if (c == "LG") out = RankTier::kLegend;
    else if (c == "MY") out = RankTier::kMythic;
    else if (c == "MG") out = RankTier::kMythicalGlory;
    else if (c == "MI") out = RankTier::kMythicalImmortal;
    else return false;
    return true;
}

// ---------------------------------------------------------------------------
// Extended type implementations
// ---------------------------------------------------------------------------

std::string MatchSummary::line() const {
    char buf[256];
    snprintf(buf, sizeof(buf),
             "%s %s -> %s (%.1f) KDA %s perf %d dur %.1fm %s",
             won ? "WIN" : "LOSS", rankBefore.c_str(), rankAfter.c_str(),
             mmrDelta, kda.c_str(), performance, durationMin, mode.c_str());
    return std::string(buf);
}

std::string TierInfo::toString() const {
    char buf[128];
    snprintf(buf, sizeof(buf), "%s (%d/%d stars, mmr %.0f)",
             name.c_str(), winStars, lossStars, mmrThreshold);
    return std::string(buf);
}

std::string SessionBookmark::serialize() const {
    char buf[256];
    snprintf(buf, sizeof(buf), "at=%lld;rank=%s;mmr=%.1f;matches=%d;note=%s",
             static_cast<long long>(savedAtMs), serializeRank(rank).c_str(),
             mmr, matchesPlayed, note.c_str());
    return std::string(buf);
}

SessionBookmark SessionBookmark::deserialize(const std::string& s) {
    SessionBookmark b;
    for (const auto& part : utils::split(s, ';')) {
        auto eq = part.find('=');
        if (eq == std::string::npos) continue;
        std::string key = part.substr(0, eq);
        std::string val = part.substr(eq + 1);
        if (key == "at") b.savedAtMs = std::atoll(val.c_str());
        else if (key == "rank") b.rank = deserializeRank(val);
        else if (key == "mmr") b.mmr = std::atof(val.c_str());
        else if (key == "matches") b.matchesPlayed = std::atoi(val.c_str());
        else if (key == "note") b.note = val;
    }
    return b;
}

}  // namespace rb
}  // namespace arift