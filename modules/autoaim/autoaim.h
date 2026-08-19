#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "arift_thread.h"
#include "esp_types.h"

namespace arift {
namespace autoaim {

// Target selection mode.
enum class TargetMode {
    kLowestHp = 0,
    kClosest = 1,
    kPriority = 2,     // priority hero (marksman / mage)
    kSquishiest = 3,   // lowest armor+magic resist
    kFocus = 4,        // player-selected focus target
};

// Aim assist behavior flags.
struct AutoAimConfig {
    bool enabled = true;
    bool assistBasicAttacks = true;
    bool assistSkillShots = true;
    bool leadMovingTargets = true;
    bool clampToScreen = true;
    float aimSmoothing = 0.55f;     // 0..1 (1 = instant)
    float skillShotRange = 800.0f;  // game units
    float basicRange = 250.0f;
    int cooldownMs = 150;
    TargetMode mode = TargetMode::kLowestHp;
    int focusHeroId = 0;
};

// A locked target.
struct TargetLock {
    bool locked = false;
    uint32_t heroId = 0;
    std::string heroName;
    Vec3 position;
    Vec3 velocity;
    float distance = 0.0f;
    float hpRatio = 1.0f;
    float priorityScore = 0.0f;
    int64_t lockedAtMs = 0;
};

// Aim direction (normalized) + confidence.
struct AimSolution {
    bool valid = false;
    Vec3 direction;             // normalized world direction
    Vec3 aimPoint;              // world position aimed at (with lead)
    float confidence = 0.0f;    // 0..1
    bool usesLead = false;
    float leadSeconds = 0.0f;
};

// Skill-shot parameters.
struct SkillShotParams {
    int skillIndex = 0;
    float travelSpeed = 0.0f;   // game units / s
    float width = 0.0f;
    float maxRange = 0.0f;
    float castTimeMs = 0.0f;
    bool pierce = false;
};

// Target scoring result.
struct TargetScore {
    uint32_t heroId = 0;
    std::string heroName;
    float score = 0.0f;
    float distance = 0.0f;
    float hpRatio = 1.0f;
    float armor = 0.0f;
    float magicResist = 0.0f;
    bool alive = false;
    bool visible = false;
};

// Free functions for the bridge / host.
void enable(bool on);
bool enabled();
void setMode(int mode);
void setFocus(uint32_t heroId);
TargetLock lockTarget(const std::vector<PlayerSnapshot>& players,
                      const PlayerSnapshot& self, int64_t nowMs);
AimSolution solveAim(const TargetLock& target, const PlayerSnapshot& self,
                     const SkillShotParams& params, int64_t nowMs);
void aimAt(const AimSolution& solution);
std::string autoAimDiag();

// Module class.
class AutoAim {
public:
    static AutoAim& instance();

    int start();
    int stop();
    bool running() const { return running_.load(); }

    void setEnabled(bool v);
    void setMode(int m);
    void setFocus(uint32_t heroId);

    void feed(const std::vector<PlayerSnapshot>& players,
              const PlayerSnapshot& self, int64_t nowMs);
    TargetLock currentLock() const;
    AimSolution currentSolution() const;
    std::string diag() const;
    const AutoAimConfig& cfg() const;

private:
    AutoAim() = default;
    ~AutoAim() { stop(); }
    void loop();

    Thread thread_{"arift-autoaim"};
    std::atomic<bool> running_{false};
    AutoAimConfig cfg_;
    mutable std::mutex state_mutex_;
    std::vector<PlayerSnapshot> players_;
    PlayerSnapshot self_;
    int64_t nowMs_ = 0;
    TargetLock lock_;
    AimSolution solution_;
};

}  // namespace autoaim
}  // namespace arift