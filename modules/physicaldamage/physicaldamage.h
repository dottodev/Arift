#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "arift_thread.h"
#include "esp_types.h"

namespace arift {
namespace physicaldamage {

// Damage rotation style.
enum class RotationStyle {
    kBalanced = 0,     // keep skills for trades
    kBurst = 1,        // dump everything in burst windows
    kPoke = 2,         // poke from range, save escape
    kAllIn = 3,        // fight to the end
};

// Damage config.
struct PhysicalDamageConfig {
    bool enabled = true;
    bool autoCastSkills = true;
    bool critAssist = true;
    bool squishyPriority = true;
    bool burstWindows = true;
    float pokeRange = 600.0f;
    float engageRange = 400.0f;
    int skillCastCooldownMs = 600;
    int burstCooldownMs = 20000;
    RotationStyle style = RotationStyle::kBurst;
    int critThresholdPct = 60;      // crit chance % to enable crit assist
    float damageMultiplier = 1.0f;  // local model only (UI display)
};

// Damage skill state.
struct DamageSkill {
    int index = 0;
    float damage = 0.0f;
    float range = 0.0f;
    float cooldownMs = 0.0f;
    float physicalRatio = 1.0f;   // how much of the damage is physical
    bool ready = true;
    int64_t readyAtMs = 0;
    bool isUltimate = false;
};

// Burst window state.
struct BurstState {
    bool inBurst = false;
    int64_t burstUntilMs = 0;
    int castsInBurst = 0;
    int maxCasts = 5;
    float damageInBurst = 0.0f;
};

// Rotation decision.
struct RotationDecision {
    bool cast = false;
    int skillIndex = -1;
    uint32_t targetId = 0;
    std::string targetName;
    float predictedDamage = 0.0f;
    const char* reason = "none";
    bool usesCrit = false;
};

// Target summary for damage selection.
struct DamageTarget {
    uint32_t heroId = 0;
    std::string heroName;
    float distance = 0.0f;
    float hpRatio = 1.0f;
    float armor = 0.0f;
    float physicalDamageTaken = 0.0f;   // sensitivity to physical damage
    float score = 0.0f;
    bool alive = false;
    bool visible = false;
};

// Free functions.
void enable(bool on);
bool enabled();
void setStyle(int style);
void setAutoCast(bool on);
RotationDecision decide(const std::vector<PlayerSnapshot>& players,
                        const PlayerSnapshot& self,
                        const std::vector<DamageSkill>& skills,
                        int64_t nowMs);
void executeRotation(const RotationDecision& d, int64_t nowMs);
std::string physicalDamageDiag();

// Module class.
class PhysicalDamage {
public:
    static PhysicalDamage& instance();

    int start();
    int stop();
    bool running() const { return running_.load(); }

    void setEnabled(bool v);
    void setStyle(int s);
    void setAutoCast(bool v);

    void feed(const std::vector<PlayerSnapshot>& players,
              const PlayerSnapshot& self,
              const std::vector<DamageSkill>& skills, int64_t nowMs);
    RotationDecision lastDecision() const;
    BurstState burstState() const;
    BurstState& burstStateRef();
    std::string diag() const;
    const PhysicalDamageConfig& cfg() const;

private:
    PhysicalDamage() = default;
    ~PhysicalDamage() { stop(); }
    void loop();

    Thread thread_{"arift-physicaldamage"};
    std::atomic<bool> running_{false};
    PhysicalDamageConfig cfg_;
    mutable std::mutex state_mutex_;
    std::vector<PlayerSnapshot> players_;
    PlayerSnapshot self_;
    std::vector<DamageSkill> skills_;
    int64_t nowMs_ = 0;
    RotationDecision lastDecision_;
    BurstState burst_;
};

}  // namespace physicaldamage
}  // namespace arift