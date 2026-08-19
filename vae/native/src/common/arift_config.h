#pragma once

#include <cstdint>
#include <string>

namespace arift {

// Feature identifiers — must stay in sync with InjectionManager.kt.
enum FeatureId : int {
    kFeatureEsp = 1,
    kFeatureMapHack = 2,
    kFeatureAutoRetri = 3,
    kFeatureAutoAim = 4,
    kFeatureEnemyLag = 5,
    kFeatureRankBooster = 6,
    kFeatureTankDefense = 7,
    kFeaturePhysicalDamage = 8,
    kFeatureVoidBan = 9,
    kFeatureMusic = 10,
    kFeatureCount = 10,
};

const char* featureName(int feature);

// Global configuration store, backed by an INI-style file in app files dir.
class Config {
public:
    static Config& instance();

    bool init(const std::string& base_dir);
    void shutdown();

    std::string getString(const std::string& section, const std::string& key,
                          const std::string& def) const;
    int getInt(const std::string& section, const std::string& key, int def) const;
    float getFloat(const std::string& section, const std::string& key, float def) const;
    bool getBool(const std::string& section, const std::string& key, bool def) const;

    void setString(const std::string& section, const std::string& key,
                   const std::string& value);
    void setInt(const std::string& section, const std::string& key, int value);
    void setFloat(const std::string& section, const std::string& key, float value);
    void setBool(const std::string& section, const std::string& key, bool value);

    bool save() const;
    bool load();

    const std::string& baseDir() const { return base_dir_; }
    std::string configPath() const { return base_dir_ + "/arift.ini"; }

private:
    Config() = default;
    std::string base_dir_;
    std::string raw_;
    bool dirty_ = false;
};

}  // namespace arift