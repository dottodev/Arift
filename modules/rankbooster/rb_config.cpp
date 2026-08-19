#include "rb_api.h"

#include "arift_config.h"
#include "arift_log.h"
#include "arift_utils.h"

namespace arift {
namespace rb {

// ---------------------------------------------------------------------------
// RbConfig persistence
// ---------------------------------------------------------------------------

void RbConfig::loadFromConfig() {
    Config& cfg = Config::instance();
    enabled = cfg.getBool("rankbooster", "enabled", enabled);
    aggression = static_cast<AggressionLevel>(
        cfg.getInt("rankbooster", "aggression", static_cast<int>(aggression)));
    targetTier = static_cast<RankTier>(
        cfg.getInt("rankbooster", "target_tier", static_cast<int>(targetTier)));
    targetStars = cfg.getInt("rankbooster", "target_stars", targetStars);
    desiredWinRate = cfg.getFloat("rankbooster", "desired_win_rate", desiredWinRate);
    maxQueueSeconds = cfg.getFloat("rankbooster", "max_queue_seconds", maxQueueSeconds);
    tuneIntervalSec = cfg.getFloat("rankbooster", "tune_interval_sec", tuneIntervalSec);
    telemetryRetentionDays = cfg.getFloat("rankbooster", "telemetry_retention_days",
                                          telemetryRetentionDays);
    guardSensitivity = cfg.getInt("rankbooster", "guard_sensitivity", guardSensitivity);
    autoPauseOnRisk = cfg.getBool("rankbooster", "auto_pause_on_risk", autoPauseOnRisk);
    protectStreak = cfg.getBool("rankbooster", "protect_streak", protectStreak);
    preferredRoleEnabled = cfg.getBool("rankbooster", "preferred_role_enabled",
                                       preferredRoleEnabled);
    preferredRole = cfg.getInt("rankbooster", "preferred_role", preferredRole);
    partyAssistEnabled = cfg.getBool("rankbooster", "party_assist_enabled",
                                     partyAssistEnabled);
    adaptiveDifficulty = cfg.getBool("rankbooster", "adaptive_difficulty",
                                     adaptiveDifficulty);
    mmrTarget = cfg.getFloat("rankbooster", "mmr_target", mmrTarget);
    sigmaFloor = cfg.getFloat("rankbooster", "sigma_floor", sigmaFloor);
    volatilityCap = cfg.getFloat("rankbooster", "volatility_cap", volatilityCap);
    performanceBias = cfg.getInt("rankbooster", "performance_bias", performanceBias);
    lobbyTimingEnabled = cfg.getBool("rankbooster", "lobby_timing_enabled",
                                     lobbyTimingEnabled);
    lobbyDelayMinMs = cfg.getFloat("rankbooster", "lobby_delay_min_ms", lobbyDelayMinMs);
    lobbyDelayMaxMs = cfg.getFloat("rankbooster", "lobby_delay_max_ms", lobbyDelayMaxMs);
    randomizePlaystyle = cfg.getBool("rankbooster", "randomize_playstyle",
                                     randomizePlaystyle);
    burstPatternEnabled = cfg.getBool("rankbooster", "burst_pattern_enabled",
                                      burstPatternEnabled);
    burstLimitMatches = cfg.getFloat("rankbooster", "burst_limit_matches",
                                     burstLimitMatches);
}

void RbConfig::saveToConfig() const {
    Config& cfg = Config::instance();
    cfg.setBool("rankbooster", "enabled", enabled);
    cfg.setInt("rankbooster", "aggression", static_cast<int>(aggression));
    cfg.setInt("rankbooster", "target_tier", static_cast<int>(targetTier));
    cfg.setInt("rankbooster", "target_stars", targetStars);
    cfg.setFloat("rankbooster", "desired_win_rate", desiredWinRate);
    cfg.setFloat("rankbooster", "max_queue_seconds", maxQueueSeconds);
    cfg.setFloat("rankbooster", "tune_interval_sec", tuneIntervalSec);
    cfg.setFloat("rankbooster", "telemetry_retention_days", telemetryRetentionDays);
    cfg.setInt("rankbooster", "guard_sensitivity", guardSensitivity);
    cfg.setBool("rankbooster", "auto_pause_on_risk", autoPauseOnRisk);
    cfg.setBool("rankbooster", "protect_streak", protectStreak);
    cfg.setBool("rankbooster", "preferred_role_enabled", preferredRoleEnabled);
    cfg.setInt("rankbooster", "preferred_role", preferredRole);
    cfg.setBool("rankbooster", "party_assist_enabled", partyAssistEnabled);
    cfg.setBool("rankbooster", "adaptive_difficulty", adaptiveDifficulty);
    cfg.setFloat("rankbooster", "mmr_target", mmrTarget);
    cfg.setFloat("rankbooster", "sigma_floor", sigmaFloor);
    cfg.setFloat("rankbooster", "volatility_cap", volatilityCap);
    cfg.setInt("rankbooster", "performance_bias", performanceBias);
    cfg.setBool("rankbooster", "lobby_timing_enabled", lobbyTimingEnabled);
    cfg.setFloat("rankbooster", "lobby_delay_min_ms", lobbyDelayMinMs);
    cfg.setFloat("rankbooster", "lobby_delay_max_ms", lobbyDelayMaxMs);
    cfg.setBool("rankbooster", "randomize_playstyle", randomizePlaystyle);
    cfg.setBool("rankbooster", "burst_pattern_enabled", burstPatternEnabled);
    cfg.setFloat("rankbooster", "burst_limit_matches", burstLimitMatches);
}

RbConfigStore& RbConfigStore::instance() {
    static RbConfigStore store;
    return store;
}

void RbConfigStore::load() {
    config_.loadFromConfig();
    ARIFT_DEBUG(kTagRankBooster, "Rank booster config loaded");
}

void RbConfigStore::save() {
    config_.saveToConfig();
    Config::instance().save();
}

void RbConfigStore::resetToDefaults() {
    config_ = RbConfig{};
    save();
}

// ---------------------------------------------------------------------------
// Config validation and helpers
// ---------------------------------------------------------------------------

namespace {

// Clamp a float field into a sane range.
double clampField(double v, double lo, double hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

}  // namespace

// Validate all config fields; repairs out-of-range values in place.
void RbConfig::sanitize() {
    desiredWinRate = clampField(desiredWinRate, 0.30, 0.95);
    maxQueueSeconds = clampField(maxQueueSeconds, 15.0, 600.0);
    tuneIntervalSec = clampField(tuneIntervalSec, 30.0, 3600.0);
    telemetryRetentionDays = clampField(telemetryRetentionDays, 1.0, 90.0);
    guardSensitivity = rb_utils::clampInt(guardSensitivity, 1, 5);
    preferredRole = rb_utils::clampInt(preferredRole, 0, 4);
    mmrTarget = clampField(mmrTarget, 900.0, 4500.0);
    sigmaFloor = clampField(sigmaFloor, 20.0, 350.0);
    volatilityCap = clampField(volatilityCap, 0.02, 0.30);
    performanceBias = rb_utils::clampInt(performanceBias, 0, 50);
    lobbyDelayMinMs = clampField(lobbyDelayMinMs, 200.0, 15000.0);
    lobbyDelayMaxMs = clampField(lobbyDelayMaxMs, lobbyDelayMinMs, 60000.0);
    burstLimitMatches = clampField(burstLimitMatches, 1.0, 10.0);
}

// Readable dump of the active config.
std::string RbConfig::toString() const {
    char buf[1024];
    snprintf(buf, sizeof(buf),
             "rankbooster: enabled=%d aggression=%s target=%s winrate=%.2f "
             "queue=%.0fs tune=%.0fs\n"
             "guard_sens=%d auto_pause=%d protect_streak=%d role=%d party=%d "
             "adaptive=%d\n"
             "mmr_target=%.0f sigma_floor=%.0f vol_cap=%.3f perf_bias=%d\n"
             "lobby_timing=%d delay=%.0f-%.0fms random_style=%d burst=%d "
             "burst_limit=%.0f",
             enabled ? 1 : 0, aggressionName(aggression),
             targetTier == RankTier::kCount ? "?" :
                 RankPoint{targetTier, targetStars, 0, 0}.toString().c_str(),
             desiredWinRate, maxQueueSeconds, tuneIntervalSec,
             guardSensitivity, autoPauseOnRisk ? 1 : 0, protectStreak ? 1 : 0,
             preferredRole, partyAssistEnabled ? 1 : 0,
             adaptiveDifficulty ? 1 : 0,
             mmrTarget, sigmaFloor, volatilityCap, performanceBias,
             lobbyTimingEnabled ? 1 : 0, lobbyDelayMinMs, lobbyDelayMaxMs,
             randomizePlaystyle ? 1 : 0, burstPatternEnabled ? 1 : 0,
             burstLimitMatches);
    return std::string(buf);
}

// Profile of config changes vs another config.
std::vector<std::string> diffConfigs(const RbConfig& a, const RbConfig& b) {
    std::vector<std::string> out;
    if (a.desiredWinRate != b.desiredWinRate)
        out.push_back("desired_win_rate");
    if (a.maxQueueSeconds != b.maxQueueSeconds)
        out.push_back("max_queue_seconds");
    if (a.guardSensitivity != b.guardSensitivity)
        out.push_back("guard_sensitivity");
    if (a.mmrTarget != b.mmrTarget) out.push_back("mmr_target");
    if (a.sigmaFloor != b.sigmaFloor) out.push_back("sigma_floor");
    if (a.aggression != b.aggression) out.push_back("aggression");
    return out;
}

// Marshal config to a compact string (for cache/telemetry).
std::string RbConfig::toCacheString() const {
    char buf[256];
    snprintf(buf, sizeof(buf), "v1:%d:%.2f:%.0f:%.0f:%d:%.0f:%d",
             static_cast<int>(aggression), desiredWinRate, maxQueueSeconds,
             mmrTarget, guardSensitivity, sigmaFloor, enabled ? 1 : 0);
    return std::string(buf);
}

// Unmarshal config from a cache string (best-effort).
bool RbConfig::fromCacheString(const std::string& s) {
    auto parts = utils::split(s, ':');
    if (parts.size() < 7) return false;
    if (parts[0] != "v1") return false;
    aggression = static_cast<AggressionLevel>(std::atoi(parts[1].c_str()));
    desiredWinRate = std::atof(parts[2].c_str());
    maxQueueSeconds = std::atof(parts[3].c_str());
    mmrTarget = std::atof(parts[4].c_str());
    guardSensitivity = std::atoi(parts[5].c_str());
    sigmaFloor = std::atof(parts[6].c_str());
    enabled = parts.size() > 7 && parts[7] == "1";
    return true;
}

// Effective aggression for a given performance band.
AggressionLevel aggressionForPerformance(double avgPerformance) {
    if (avgPerformance >= 80.0) return AggressionLevel::kAggressive;
    if (avgPerformance >= 60.0) return AggressionLevel::kBalanced;
    return AggressionLevel::kConservative;
}

}  // namespace rb
}  // namespace arift