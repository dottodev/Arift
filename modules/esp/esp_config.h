#pragma once

#include <cstdint>

#include "esp_types.h"

namespace arift {

// Runtime-configurable ESP settings, mirrored to Config on change.
struct EspSettings {
    bool enabled = true;
    bool drawBoxes = true;
    bool drawHealthBars = true;
    bool drawNames = true;
    bool drawCooldowns = true;
    bool drawObjectives = true;
    bool drawDistance = true;
    bool drawLines = false;
    bool drawCircleEnemies = false;
    bool drawItems = false;
    bool drawHeroLevel = true;
    bool drawKda = false;
    bool drawManaBar = false;
    bool drawLowHpAlert = true;

    EspRenderMode renderMode = EspRenderMode::kScreenOverlay;
    int maxEntities = 256;
    int maxRenderFps = 30;

    uint32_t enemyBoxColor = 0xFF00FF00;       // ARGB
    uint32_t allyBoxColor = 0xFF00CCFF;
    uint32_t neutralBoxColor = 0xFFFFFF00;
    uint32_t lowHpColor = 0xFFFF0000;
    uint32_t textColor = 0xFFFFFFFF;
    uint32_t healthColor = 0xFF39FF14;
    uint32_t manaColor = 0xFF00A2FF;

    float lowHpThreshold = 0.25f;   // ratio below which alert shows
    float boxPadding = 4.0f;
    float textSize = 13.0f;
    float healthBarWidth = 40.0f;
    float healthBarHeight = 4.0f;
    float objectiveTextSize = 14.0f;

    int visionScanMs = 250;          // entity refresh interval
    int cooldownScanMs = 500;

    // Objective tracking
    bool trackLord = true;
    bool trackTurtle = true;
    bool trackJungleTimers = true;

    // World-to-screen projection parameters
    float fovDegrees = 60.0f;
    float nearPlane = 0.1f;
    float farPlane = 2000.0f;

    void loadFromConfig();
    void saveToConfig() const;
};

class EspConfig {
public:
    static EspConfig& instance();

    EspSettings& settings() { return settings_; }
    const EspSettings& settings() const { return settings_; }

    void load();
    void save();

private:
    EspConfig() = default;
    EspSettings settings_;
};

}  // namespace arift