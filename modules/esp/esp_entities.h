#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "esp_types.h"

namespace arift {

// Resolves game memory structures into PlayerSnapshot / ObjectiveSnapshot
// objects. Offsets are supplied externally (reverse engineering output);
// the engine validates every read and falls back gracefully.
class EspEntityReader {
public:
    struct Offsets {
        // Root pointer chain for the entity list.
        uint64_t entityListPtr = 0;        // absolute address (resolved at attach)
        int32_t entityCountOffset = 0;
        int32_t entityPtrArrayOffset = 0;
        // Per-entity fields
        int32_t idOffset = 0;
        int32_t namePtrOffset = 0;
        int32_t heroNamePtrOffset = 0;
        int32_t teamOffset = 0;
        int32_t kindOffset = 0;
        int32_t posOffset = 0;
        int32_t velOffset = 0;
        int32_t healthOffset = 0;
        int32_t maxHealthOffset = 0;
        int32_t manaOffset = 0;
        int32_t maxManaOffset = 0;
        int32_t attackRangeOffset = 0;
        int32_t moveSpeedOffset = 0;
        int32_t armorOffset = 0;
        int32_t magicResistOffset = 0;
        int32_t levelOffset = 0;
        int32_t aliveOffset = 0;
        int32_t visibleOffset = 0;
        int32_t killsOffset = 0;
        int32_t deathsOffset = 0;
        int32_t assistsOffset = 0;
        int32_t abilityArrayOffset = 0;
        int32_t abilityCooldownStride = 0;
        int32_t abilityCooldownOffset = 0;
        int32_t abilityMaxCooldownOffset = 0;
        int32_t respawnOffset = 0;

        void reset() {
            entityListPtr = 0;
            entityCountOffset = 0;
            entityPtrArrayOffset = 0;
            idOffset = 0;
            namePtrOffset = 0;
            heroNamePtrOffset = 0;
            teamOffset = 0;
            kindOffset = 0;
            posOffset = 0;
            velOffset = 0;
            healthOffset = 0;
            maxHealthOffset = 0;
            manaOffset = 0;
            maxManaOffset = 0;
            attackRangeOffset = 0;
            moveSpeedOffset = 0;
            armorOffset = 0;
            magicResistOffset = 0;
            levelOffset = 0;
            aliveOffset = 0;
            visibleOffset = 0;
            killsOffset = 0;
            deathsOffset = 0;
            assistsOffset = 0;
            abilityArrayOffset = 0;
            abilityCooldownStride = 0;
            abilityCooldownOffset = 0;
            abilityMaxCooldownOffset = 0;
            respawnOffset = 0;
        }
    };

    EspEntityReader();

    void setOffsets(const Offsets& offsets) { offsets_ = offsets; }
    const Offsets& offsets() const { return offsets_; }

    // Attach to a process via /proc/<pid>/mem.
    bool attach(int pid);

    // Refresh entity snapshots. Returns count of valid players found.
    size_t refresh();

    const std::vector<PlayerSnapshot>& players() const { return players_; }
    const std::vector<ObjectiveSnapshot>& objectives() const { return objectives_; }

    // Query helpers
    const PlayerSnapshot* findPlayer(uint32_t id) const;
    const PlayerSnapshot* localPlayer() const;
    std::vector<const PlayerSnapshot*> enemies() const;
    std::vector<const PlayerSnapshot*> allies() const;

    int pid() const { return pid_; }
    uint64_t lastRefreshMs() const { return last_refresh_ms_; }
    size_t readFailures() const { return read_failures_; }

    // World-to-screen projection using stored camera data.
    void setCamera(const Vec3& pos, const Vec3& forward, const Vec3& up,
                   float fovDeg, int screenW, int screenH);
    ScreenProjection projectToScreen(const Vec3& world) const;

    // Camera accessors
    const Vec3& cameraPos() const { return camera_pos_; }

    // Simulated data source (used when game memory offsets are unknown):
    // fills the snapshot list with synthetic entities for pipeline testing.
    void useSimulation(bool enabled, size_t entityCount = 10);
    bool usingSimulation() const { return simulation_; }

private:
    int pid_ = -1;
    Offsets offsets_;
    std::vector<PlayerSnapshot> players_;
    std::vector<ObjectiveSnapshot> objectives_;
    uint64_t last_refresh_ms_ = 0;
    size_t read_failures_ = 0;

    bool simulation_ = false;
    size_t sim_count_ = 10;
    uint64_t sim_clock_ = 0;

    Vec3 camera_pos_;
    Vec3 camera_forward_;
    Vec3 camera_up_;
    float fov_rad_ = 1.0f;
    int screen_w_ = 1080;
    int screen_h_ = 2340;

    bool readFloatSafe(uintptr_t addr, float& out) const;
    bool readU32Safe(uintptr_t addr, uint32_t& out) const;
    std::string readStringSafe(uintptr_t addr) const;

    void simulateEntities();
    void readEntity(uintptr_t base, PlayerSnapshot& out) const;
    void readObjectives(uintptr_t base);
};

}  // namespace arift