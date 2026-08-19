#include "rb_api.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

#include "rb_math.h"

namespace arift {
namespace rb {

// ---------------------------------------------------------------------------
// RbAi — profile building, performance prediction, playstyle modelling.
// ---------------------------------------------------------------------------

namespace {

double weight(double x, double strength) {
    return rb_utils::clamp01(x) * strength;
}

}  // namespace

AiProfile RbAi::buildProfile(const PlayerProfile& p,
                             const RbStatistics& stats) const {
    AiProfile prof;

    // Aggression: driven by KDA participation and win streak.
    double participation = stats.avgKills + stats.avgAssists;
    prof.aggression = rb_utils::clamp01(
        0.4 + participation / 18.0 + static_cast<double>(p.streak) * 0.03);

    // Risk taking: deaths vs kills ratio.
    double risk = stats.avgDeaths > 0.0
                      ? stats.avgKills / (stats.avgKills + stats.avgDeaths)
                      : 0.7;
    prof.riskTaking = rb_utils::clamp01(1.0 - risk * 0.5 + prof.aggression * 0.3);

    // Map awareness: survival rate + objective focus.
    prof.mapAwareness = rb_utils::clamp01(
        0.3 + stats.survivalRate * 0.4 + stats.firstBloodRate * 0.3);

    // Mechanical skill: performance percentile normalized.
    prof.mechanicalSkill = rb_utils::clamp01(stats.avgPerformance / 100.0);

    // Macro skill: win rate vs. KDA efficiency.
    prof.macroSkill = rb_utils::clamp01(
        stats.winRate * 0.7 + stats.kdaRatio / 6.0 * 0.3);

    // Decision speed: match duration efficiency.
    double avgDur = stats.avgMatchDuration;
    prof.decisionSpeed = rb_utils::clamp01(1.0 - (avgDur - 10.0) / 20.0);

    // Hero diversity: approximated from match mode variance.
    prof.heroDiversity = rb_utils::clamp01(
        0.3 + static_cast<double>(stats.matchesThisWeek) / 40.0);

    // Consistency from stats variance.
    prof.consistency = rb_utils::clamp01(
        1.0 - std::fabs(stats.avgPerformance - 50.0) / 50.0 * 0.5);

    // Adaptation: improvement over time (positive mmr trend).
    MmrTrend trend = analyzeTrend(p, ProfileStore::instance().history(), 20);
    prof.adaptation = rb_utils::clamp01(0.5 + trend.slope / 20.0);

    // Emotional stability: inverse of volatility.
    prof.emotionalStability = rb_utils::clamp01(
        1.0 - p.volatility * 4.0 - std::fabs(static_cast<double>(p.streak)) * 0.05);

    prof.predictedPerformance = predictPerformance(p, stats, RbConfig{});
    return prof;
}

double RbAi::predictPerformance(const PlayerProfile& p,
                                const RbStatistics& stats,
                                const RbConfig& cfg) const {
    double score = 50.0;

    score += weight(stats.winRate, 18.0) * 100.0 - 9.0;            // win rate
    score += weight(stats.avgPerformance / 100.0, 25.0) * 100.0 - 12.5;
    score += weight(stats.kdaRatio / 4.0, 12.0) * 100.0 - 6.0;
    score += weight(1.0 - p.sigma / 350.0, 10.0) * 100.0 - 5.0;
    score += weight(1.0 - p.volatility / 0.12, 8.0) * 100.0 - 4.0;

    // Streak momentum.
    double streakBonus = rb_utils::clamp(static_cast<double>(p.streak) * 1.5,
                                         -8.0, 8.0);
    score += streakBonus;

    // Session fatigue: fewer expected stars later in session.
    if (cfg.enabled) {
        score -= weight(cfg.aggression == AggressionLevel::kExtreme ? 0.1 : 0.0,
                        5.0);
    }

    return rb_utils::clamp(score, 0.0, 100.0);
}

double RbAi::recommendedPlaystyleIndex(const AiProfile& profile,
                                       const RbConfig& cfg) const {
    double idx = 0.5;
    idx += (profile.aggression - 0.5) * 0.3;
    idx += (profile.riskTaking - 0.5) * 0.2;
    idx += (profile.decisionSpeed - 0.5) * 0.15;
    idx += (profile.macroSkill - 0.5) * 0.15;

    switch (cfg.aggression) {
        case AggressionLevel::kConservative: idx -= 0.15; break;
        case AggressionLevel::kAggressive: idx += 0.15; break;
        case AggressionLevel::kExtreme: idx += 0.25; break;
        default: break;
    }
    return rb_utils::clamp01(idx);
}

std::string RbAi::playstyleLabel(double index) const {
    if (index < 0.25) return "defensive";
    if (index < 0.45) return "balanced-defensive";
    if (index < 0.6) return "balanced";
    if (index < 0.8) return "aggressive";
    return "hyper-aggressive";
}

std::string AiProfile::toString() const {
    char buf[512];
    snprintf(buf, sizeof(buf),
             "aggression=%.2f risk=%.2f awareness=%.2f mech=%.2f "
             "macro=%.2f speed=%.2f diversity=%.2f consistency=%.2f "
             "adapt=%.2f stability=%.2f predictedPerf=%.1f",
             aggression, riskTaking, mapAwareness, mechanicalSkill,
             macroSkill, decisionSpeed, heroDiversity, consistency,
             adaptation, emotionalStability, predictedPerformance);
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// Deeper profile dimensions
// ---------------------------------------------------------------------------

namespace {

// Exponential smoothing of a value series.
double smoothSeries(const std::vector<double>& series, double alpha) {
    if (series.empty()) return 0.0;
    double acc = series.front();
    for (size_t i = 1; i < series.size(); ++i) {
        acc = alpha * series[i] + (1.0 - alpha) * acc;
    }
    return acc;
}

}  // namespace

// Momentum: recent win-rate vs. career win-rate.
double momentumIndex(const PlayerProfile& p, const RbStatistics& stats) {
    double recent = stats.recentWinRate;
    double career = p.winRate;
    if (career <= 0.0) return 0.5;
    return rb_utils::clamp01(0.5 + (recent - career) * 2.0);
}

// Clutch factor: performance in wins vs losses.
double clutchFactor(const std::vector<MatchRecord>& history) {
    double winPerf = 0.0;
    double lossPerf = 0.0;
    int wn = 0;
    int ln = 0;
    for (const auto& rec : history) {
        if (rec.result.won) {
            winPerf += static_cast<double>(rec.result.performancePercentile);
            wn += 1;
        } else {
            lossPerf += static_cast<double>(rec.result.performancePercentile);
            ln += 1;
        }
    }
    if (wn == 0 || ln == 0) return 0.5;
    double w = winPerf / static_cast<double>(wn);
    double l = lossPerf / static_cast<double>(ln);
    return rb_utils::clamp01(0.5 + (w - l) / 50.0);
}

// Fatigue trend: performance decline across long sessions.
double fatigueIndex(const std::vector<MatchRecord>& history) {
    if (history.size() < 6) return 0.5;
    std::vector<double> recent;
    for (size_t i = history.size() >= 10 ? history.size() - 10 : 0;
         i < history.size(); ++i) {
        recent.push_back(static_cast<double>(history[i].result.performancePercentile));
    }
    double firstHalf = rb_utils::mean(std::vector<double>(
        recent.begin(), recent.begin() + static_cast<long>(recent.size() / 2)));
    double secondHalf = rb_utils::mean(std::vector<double>(
        recent.begin() + static_cast<long>(recent.size() / 2), recent.end()));
    if (firstHalf <= 0.0) return 0.5;
    return rb_utils::clamp01(1.0 - (firstHalf - secondHalf) / firstHalf);
}

// Objective focus: tower/gold proxy via team score efficiency.
double objectiveFocus(const RbStatistics& stats) {
    double eff = stats.winRate * 0.5 + stats.avgPerformance / 200.0;
    return rb_utils::clamp01(eff);
}

// Lane pressure style: from KDA asymmetry.
double lanePressureStyle(const RbStatistics& stats) {
    double total = stats.avgKills + stats.avgDeaths + stats.avgAssists;
    if (total <= 0.0) return 0.5;
    return rb_utils::clamp01(0.3 + stats.avgKills / total * 0.7);
}

// Predict performance trajectory over the next N matches.
std::vector<double> performanceProjection(const PlayerProfile& p,
                                          const RbStatistics& stats,
                                          int n) {
    std::vector<double> out;
    double base = stats.avgPerformance;
    double momentum = momentumIndex(p, stats);
    for (int i = 0; i < n; ++i) {
        double fatigue = 1.0 - 0.02 * static_cast<double>(i);
        double v = base * fatigue * (0.9 + momentum * 0.2);
        out.push_back(rb_utils::clamp(v, 0.0, 100.0));
    }
    return out;
}

// Role-fit heuristic: which role the profile suits best (0..4).
int bestRoleForProfile(const AiProfile& prof) {
    double best = -1.0;
    int role = 0;
    double scores[5] = {
        prof.mechanicalSkill * 0.7 + prof.riskTaking * 0.3,   // assassin
        prof.macroSkill * 0.6 + prof.mapAwareness * 0.4,      // support/tank
        prof.aggression * 0.5 + prof.mechanicalSkill * 0.5,   // fighter
        prof.aggression * 0.4 + prof.decisionSpeed * 0.6,     // marksman
        prof.macroSkill * 0.5 + prof.decisionSpeed * 0.5,     // mage
    };
    for (int i = 0; i < 5; ++i) {
        if (scores[i] > best) {
            best = scores[i];
            role = i;
        }
    }
    return role;
}

// Style vector distance between two profiles (for party synergy).
double profileDistance(const AiProfile& a, const AiProfile& b) {
    double d = 0.0;
    d += std::fabs(a.aggression - b.aggression);
    d += std::fabs(a.riskTaking - b.riskTaking);
    d += std::fabs(a.decisionSpeed - b.decisionSpeed);
    d += std::fabs(a.mapAwareness - b.mapAwareness);
    return d / 4.0;
}

// Compressed profile digest for caches/telemetry.
std::string profileDigest(const AiProfile& prof) {
    char buf[128];
    snprintf(buf, sizeof(buf), "%.2f|%.2f|%.2f|%.2f|%.2f",
             prof.aggression, prof.riskTaking, prof.mechanicalSkill,
             prof.macroSkill, prof.predictedPerformance);
    return std::string(buf);
}

// Series smoothing helper used by prediction models.
double seriesSmoothed(const std::vector<double>& series) {
    return smoothSeries(series, 0.3);
}

// ---------------------------------------------------------------------------
// Hero pool and style modelling
// ---------------------------------------------------------------------------

// Hero pool breadth by role coverage.
double heroPoolBreadth(const std::vector<int>& roleCounts) {
    if (roleCounts.empty()) return 0.0;
    int covered = 0;
    for (int c : roleCounts) {
        if (c > 0) covered += 1;
    }
    return static_cast<double>(covered) / static_cast<double>(roleCounts.size());
}

// Style transition tendency: how often playstyle shifts between sessions.
double styleDrift(const std::vector<double>& styleHistory) {
    if (styleHistory.size() < 3) return 0.0;
    double acc = 0.0;
    for (size_t i = 1; i < styleHistory.size(); ++i) {
        acc += std::fabs(styleHistory[i] - styleHistory[i - 1]);
    }
    return acc / static_cast<double>(styleHistory.size() - 1);
}

// Lane preference from hero role frequencies.
int preferredLane(const std::vector<int>& roleCounts) {
    if (roleCounts.empty()) return 0;
    int best = 0;
    for (size_t i = 1; i < roleCounts.size(); ++i) {
        if (roleCounts[i] > roleCounts[static_cast<size_t>(best)]) {
            best = static_cast<int>(i);
        }
    }
    return best;
}

// Adaptability: speed of style adjustment after losing streaks.
double adaptabilityIndex(const std::vector<MatchRecord>& history) {
    if (history.size() < 8) return 0.5;
    // Count performance rebounds after losses.
    int rebounds = 0;
    int losses = 0;
    for (size_t i = 1; i < history.size(); ++i) {
        if (!history[i - 1].result.won) {
            losses += 1;
            if (history[i].result.performancePercentile >
                history[i - 1].result.performancePercentile + 10) {
                rebounds += 1;
            }
        }
    }
    return losses > 0 ? rb_utils::clamp01(static_cast<double>(rebounds) /
                                          static_cast<double>(losses)) : 0.5;
}

// Meta awareness: share of matches using high-performance heroes.
double metaAwareness(const std::vector<MatchRecord>& history) {
    if (history.empty()) return 0.5;
    int good = 0;
    for (const auto& rec : history) {
        if (rec.result.performancePercentile >= 70) good += 1;
    }
    return rb_utils::clamp01(static_cast<double>(good) /
                             static_cast<double>(history.size()) * 1.4);
}

// Composure under pressure: performance in even/losing games.
double composureIndex(const std::vector<MatchRecord>& history) {
    int n = 0;
    double sum = 0.0;
    for (const auto& rec : history) {
        if (rec.result.enemyScore >= rec.result.teamScore) {
            sum += static_cast<double>(rec.result.performancePercentile);
            n += 1;
        }
    }
    return n > 0 ? rb_utils::clamp01(sum / static_cast<double>(n) / 100.0)
                 : 0.5;
}

// Early-game aggression share (kills in first minutes proxy).
double earlyAggressionShare(const RbStatistics& stats) {
    double total = stats.avgKills + stats.avgAssists;
    if (total <= 0.0) return 0.5;
    return rb_utils::clamp01(stats.firstBloodRate + stats.avgKills / total * 0.3);
}

// Objective efficiency: gold/level efficiency proxy via match duration.
double objectiveEfficiency(const RbStatistics& stats) {
    double expected = 1.0 - (stats.avgMatchDuration - 10.0) / 20.0;
    return rb_utils::clamp01(stats.winRate * 0.6 + expected * 0.4);
}

// Combined AI verdict for a session (label + confidence).
struct AiVerdict {
    std::string label;
    double confidence = 0.5;
    std::string detail;
};

AiVerdict aiVerdict(const AiProfile& prof, const RbStatistics& stats) {
    AiVerdict v;
    double score = prof.predictedPerformance;
    if (score >= 75.0) {
        v.label = "strong carry";
    } else if (score >= 60.0) {
        v.label = "reliable core";
    } else if (score >= 45.0) {
        v.label = "supportive";
    } else {
        v.label = "developing";
    }
    v.confidence = rb_utils::clamp01(1.0 - stats.sigma / 350.0);
    char buf[192];
    snprintf(buf, sizeof(buf), "%s (conf %.0f%%)", v.label.c_str(),
             v.confidence * 100.0);
    v.detail = buf;
    return v;
}

// Match-by-match prediction error (for model calibration).
double predictionError(const std::vector<double>& predicted,
                       const std::vector<double>& actual) {
    size_t n = std::min(predicted.size(), actual.size());
    if (n == 0) return 0.0;
    double acc = 0.0;
    for (size_t i = 0; i < n; ++i) {
        acc += std::fabs(predicted[i] - actual[i]);
    }
    return acc / static_cast<double>(n);
}

// ---------------------------------------------------------------------------
// Session profiling
// ---------------------------------------------------------------------------

// Profile summary of the current session's likely outcome.
double sessionOutlook(const PlayerProfile& p, const RbStatistics& stats,
                      const RbConfig& cfg) {
    double outlook = 0.5;
    outlook += (stats.winRate - 0.5) * 0.4;
    outlook += (stats.avgPerformance - 50.0) / 100.0 * 0.3;
    outlook += (p.mmr - 1500.0) / 3000.0 * 0.2;
    if (cfg.enabled) outlook += 0.05;
    return rb_utils::clamp01(outlook);
}

// Whether to recommend a break between sessions.
bool recommendBreak(const RbStatistics& stats, const RbConfig& cfg) {
    if (!cfg.autoPauseOnRisk) return false;
    if (stats.matchesThisWeek >= 40) return true;
    if (stats.currentStreak <= -3 && stats.totalMatches >= 10) return true;
    if (stats.avgPerformance < 45.0 && stats.totalMatches >= 8) return true;
    return false;
}

// Break length suggestion (minutes).
double breakMinutesSuggestion(const RbStatistics& stats) {
    double base = 15.0;
    if (stats.currentStreak <= -5) base += 30.0;
    if (stats.avgPerformance < 40.0) base += 15.0;
    if (stats.matchesThisWeek >= 50) base += 60.0;
    return rb_utils::clamp(base, 10.0, 240.0);
}

// Confidence that a given hero role will perform this session.
double roleConfidence(int role, const AiProfile& prof) {
    switch (role) {
        case 0: return prof.mechanicalSkill * 0.5 + prof.riskTaking * 0.5;
        case 1: return prof.macroSkill * 0.6 + prof.mapAwareness * 0.4;
        case 2: return prof.aggression * 0.5 + prof.mechanicalSkill * 0.5;
        case 3: return prof.aggression * 0.4 + prof.decisionSpeed * 0.6;
        case 4: return prof.macroSkill * 0.5 + prof.decisionSpeed * 0.5;
        default: return 0.5;
    }
}

// Adversary difficulty estimate from the player's own strengths.
double adversaryDifficulty(const AiProfile& prof) {
    double skill = prof.mechanicalSkill * 0.4 + prof.macroSkill * 0.3 +
                   prof.decisionSpeed * 0.3;
    return rb_utils::clamp01(0.5 + (skill - 0.5) * 0.6);
}

// Playlist-style session variety score (avoid same-role repetition).
double sessionVariety(const std::vector<int>& rolesPlayed) {
    if (rolesPlayed.empty()) return 1.0;
    std::set<int> unique(rolesPlayed.begin(), rolesPlayed.end());
    return static_cast<double>(unique.size()) /
           static_cast<double>(std::max<size_t>(rolesPlayed.size(), 1)) * 5.0;
}

// Learning rate: performance improvement per match.
double learningRate(const std::vector<MatchRecord>& history) {
    if (history.size() < 10) return 0.0;
    std::vector<double> perfs;
    for (const auto& rec : history) {
        perfs.push_back(static_cast<double>(rec.result.performancePercentile));
    }
    size_t q1 = perfs.size() / 4;
    size_t q4 = perfs.size() * 3 / 4;
    double early = rb_utils::mean(
        std::vector<double>(perfs.begin(), perfs.begin() + static_cast<long>(q1)));
    double late = rb_utils::mean(
        std::vector<double>(perfs.begin() + static_cast<long>(q4), perfs.end()));
    return (late - early) / 100.0;
}

// Tilt probability: likelihood of a losing streak continuing.
double tiltProbability(const PlayerProfile& p, const RbStatistics& stats) {
    if (p.streak >= 0) return 0.2;
    double losses = static_cast<double>(-p.streak);
    return rb_utils::clamp01(0.2 + losses * 0.1 -
                             stats.avgPerformance / 100.0 * 0.1);
}

// Best hero role suggestion for the next match (0..4).
int suggestedRole(const AiProfile& prof) {
    double best = -1.0;
    int role = 2;
    for (int r = 0; r < 5; ++r) {
        double score = roleConfidence(r, prof);
        if (score > best) {
            best = score;
            role = r;
        }
    }
    return role;
}

// Playlist advice: which role to avoid next.
int avoidRole(const AiProfile& prof) {
    double worst = 1e9;
    int role = 0;
    for (int r = 0; r < 5; ++r) {
        double score = roleConfidence(r, prof);
        if (score < worst) {
            worst = score;
            role = r;
        }
    }
    return role;
}

// Likelihood the current session should end (fatigue + streak).
double endSessionProbability(const PlayerProfile& p,
                             const RbStatistics& stats) {
    double prob = rb_utils::clamp01(static_cast<double>(stats.matchesThisWeek) / 60.0) * 0.5;
    if (p.streak <= -3) prob += 0.25;
    if (stats.matchesThisWeek >= 45) prob += 0.15;
    return rb_utils::clamp01(prob);
}

// One-line AI verdict for the overlay.
std::string aiVerdictLine(const PlayerProfile& p, const RbStatistics& stats,
                          const RbConfig& cfg) {
    std::string line;
    if (!cfg.enabled) line = "off";
    else if (endSessionProbability(p, stats) > 0.55) line = "stop soon";
    else if (recommendBreak(stats, cfg)) line = "break";
    else if (sessionOutlook(p, stats, cfg) > 0.55) line = "push";
    else line = "steady";
    return line;
}

}  // namespace rb
}  // namespace arift