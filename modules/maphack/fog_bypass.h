#pragma once

#include <cstdint>

#include "maphack_types.h"

namespace arift {

// Fog-of-war bypass: forces the engine's visibility model to consider all
// entity positions visible, then recomputes the VisionGrid so the minimap
// and overlay agree. Works with memory reads + optional engine hooks.
class FogBypass {
public:
    FogBypass();

    void setTarget(int pid);
    void setGrid(const VisionGrid& grid) { grid_ = grid; }
    const VisionGrid& grid() const { return grid_; }
    VisionGrid& grid() { return grid_; }

    // Update vision from simulated/game sources. Returns number of cells
    // changed to visible this frame.
    size_t update(const std::vector<VisionSource>& sources, int64_t nowMs);

    // Force-reveal everything (raw mode).
    void forceRevealAll();

    // Enable/disable bypass.
    void setEnabled(bool enabled) { enabled_ = enabled; }
    bool enabled() const { return enabled_; }

    // Hook point: called when the game would apply fog culling.
    bool onFogCullBegin(uintptr_t enginePtr);
    bool onFogCullEnd(uintptr_t enginePtr);

    // Diagnostics
    std::string diag() const;
    uint64_t cellsVisible() const;
    uint64_t framesProcessed() const { return frames_; }

private:
    int pid_ = -1;
    bool enabled_ = false;
    uint64_t frames_ = 0;
    VisionGrid grid_;
    bool raw_ = false;

    size_t recomputeFromSources(const std::vector<VisionSource>& sources,
                              int64_t nowMs);
    void applyGridToEngine(uintptr_t enginePtr);
};

}  // namespace arift