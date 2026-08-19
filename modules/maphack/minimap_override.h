#pragma once

#include <cstdint>
#include <vector>

#include "esp_types.h"
#include "maphack_types.h"

namespace arift {

// Minimap override: renders every tracked entity onto the in-game minimap
// regardless of fog state, and feeds an updated minimap texture stream to
// the renderer adapter.
class MinimapOverride {
public:
    MinimapOverride();

    void setParams(const MinimapParams& params) { params_ = params; }
    const MinimapParams& params() const { return params_; }

    // Feed the current entity list (world positions).
    void updateEntities(const std::vector<PlayerSnapshot>& players,
                        const std::vector<ObjectiveSnapshot>& objectives);

    // Project a world position to minimap pixel coordinates.
    Vec2 worldToMinimap(const Vec2& world) const;
    Vec2 minimapToWorld(const Vec2& pixel) const;

    // Overlay draw list for the minimap (dots + icons).
    struct MinimapDot {
        Vec2 pixel;
        EntityTeam team = EntityTeam::kUnknown;
        EntityKind kind = EntityKind::kUnknown;
        bool objective = false;
        bool lowHp = false;
    };
    const std::vector<MinimapDot>& dots() const { return dots_; }

    // Whether the override is active.
    void setEnabled(bool enabled) { enabled_ = enabled; }
    bool enabled() const { return enabled_; }

    // Teammate vision radii (for drawing vision circles on minimap).
    void setVisionCircles(const std::vector<std::pair<Vec2, float>>& circles);
    const std::vector<std::pair<Vec2, float>>& visionCircles() const {
        return vision_circles_;
    }

    // Diagnostics
    std::string diag() const;
    uint64_t dotCount() const { return dots_.size(); }

private:
    bool enabled_ = false;
    MinimapParams params_;
    std::vector<MinimapDot> dots_;
    std::vector<std::pair<Vec2, float>> vision_circles_;

    void addDot(const PlayerSnapshot& p);
    void addObjective(const ObjectiveSnapshot& o);
};

}  // namespace arift