#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "esp_types.h"

namespace arift {

// Vision grid: the fog-of-war state machine. A 2D grid over the map where
// each cell carries a visibility flag, an owner mask and a decay timer.
struct VisionCell {
    bool explored = false;      // was ever seen
    bool visible = false;       // currently visible
    uint8_t allyMask = 0;       // which allies have vision
    uint8_t enemyMask = 0;      // which enemies have vision
    int64_t lastSeenMs = 0;
};

struct VisionGrid {
    int cellsX = 64;
    int cellsY = 64;
    float cellSize = 100.0f;    // game units per cell
    Vec2 origin;                // map origin (min corner)
    std::vector<VisionCell> cells;

    void resize(int x, int y, float size, const Vec2& origin);
    VisionCell& cellAt(const Vec2& world);
    const VisionCell& cellAt(const Vec2& world) const;
    void clear();
    void revealAll();
    void setVisible(const Vec2& world, bool visible, bool explored);
    void decay(int64_t nowMs, int64_t keepVisibleMs);

    bool worldVisible(const Vec2& world) const;
};

// Camera/visibility model used by the fog bypass to recompute vision.
struct VisionSource {
    Vec3 position;
    float radius = 1500.0f;     // vision radius in game units
    bool active = true;
    uint8_t team = 1;
};

struct MapBounds {
    Vec2 min;
    Vec2 max;
    float width() const { return max.x - min.x; }
    float height() const { return max.y - min.y; }
    bool contains(const Vec2& p) const {
        return p.x >= min.x && p.x <= max.x && p.y >= min.y && p.y <= max.y;
    }
};

struct MinimapParams {
    float worldScale = 0.0f;        // pixels per game unit
    int width = 160;
    int height = 160;
    Vec2 offset;                    // top-left of minimap in screen space
    MapBounds bounds;
};

// Runtime config for the map hack module.
struct MapHackSettings {
    bool enabled = true;
    bool fogBypass = true;
    bool minimapOverride = true;
    bool revealWards = false;
    bool revealTraps = false;
    bool revealJungle = true;
    bool revealEnemyRecall = false;
    bool highlightObjectives = true;
    float visionRadius = 2000.0f;
    float revealFadeMs = 3000.0f;

    void loadFromConfig();
    void saveToConfig() const;
};

}  // namespace arift