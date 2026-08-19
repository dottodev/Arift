#pragma once

#include "esp_config.h"
#include "esp_entities.h"
#include "esp_types.h"

namespace arift {

// Builds a RenderFrame (lines, rects, circles, texts, bars) from entity
// snapshots. Host-agnostic: an adapter (overlay/GL) consumes the frame.
class EspRenderer {
public:
    explicit EspRenderer(const EspEntityReader* reader);

    void setScreen(int width, int height);
    int screenWidth() const { return width_; }
    int screenHeight() const { return height_; }

    // Produce a complete render frame for the current snapshots.
    RenderFrame render();

    // Individual draw helpers (used by render() and by tests).
    void drawPlayerBox(RenderFrame& frame, const PlayerSnapshot& p,
                       const ScreenProjection& proj);
    void drawHealthBar(RenderFrame& frame, const PlayerSnapshot& p,
                       const ScreenProjection& proj);
    void drawNameTag(RenderFrame& frame, const PlayerSnapshot& p,
                     const ScreenProjection& proj);
    void drawCooldowns(RenderFrame& frame, const PlayerSnapshot& p,
                       const ScreenProjection& proj);
    void drawObjectives(RenderFrame& frame, const ObjectiveSnapshot& obj,
                        const ScreenProjection& proj);

    // Box estimation from distance: taller when closer.
    Rect2D estimateBox(const PlayerSnapshot& p, const ScreenProjection& proj) const;

    // Color selection by team and health.
    uint32_t colorFor(const PlayerSnapshot& p) const;
    uint32_t healthColorFor(const PlayerSnapshot& p) const;

    // Frame statistics (perf telemetry).
    uint64_t framesRendered() const { return frames_; }
    uint64_t primitivesLastFrame() const { return primitives_last_; }

private:
    const EspEntityReader* reader_;
    int width_ = 1080;
    int height_ = 2340;
    uint64_t frames_ = 0;
    uint64_t primitives_last_ = 0;

    float projectHeight(const ScreenProjection& proj) const;
    void drawDistanceTag(RenderFrame& frame, const PlayerSnapshot& p,
                         const ScreenProjection& proj);
    void drawKdaTag(RenderFrame& frame, const PlayerSnapshot& p,
                    const ScreenProjection& proj);
    void drawLowHpAlert(RenderFrame& frame, const PlayerSnapshot& p,
                        const ScreenProjection& proj);
};

}  // namespace arift