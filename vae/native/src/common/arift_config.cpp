#include "arift_config.h"

#include <sys/stat.h>

#include <fstream>
#include <map>
#include <sstream>

#include "arift_log.h"
#include "arift_utils.h"

namespace arift {

const char* featureName(int feature) {
    switch (feature) {
        case kFeatureEsp: return "ESP";
        case kFeatureMapHack: return "MAP_HACK";
        case kFeatureAutoRetri: return "AUTO_RETRI";
        case kFeatureAutoAim: return "AUTO_AIM";
        case kFeatureEnemyLag: return "ENEMY_LAG";
        case kFeatureRankBooster: return "RANK_BOOSTER";
        case kFeatureTankDefense: return "TANK_DEFENSE";
        case kFeaturePhysicalDamage: return "PHYSICAL_DAMAGE";
        case kFeatureVoidBan: return "VOID_BAN";
        case kFeatureMusic: return "MUSIC";
        default: return "UNKNOWN";
    }
}

namespace {

std::map<std::string, std::map<std::string, std::string>> g_sections;

std::string trim(const std::string& s) {
    const char* ws = " \t\r\n";
    size_t b = s.find_first_not_of(ws);
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

void rebuild_raw() {
    g_sections.clear();
    std::ifstream in(Config::instance().configPath());
    if (!in) return;
    std::string line;
    std::string section = "global";
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line[0] == '[' && line.back() == ']') {
            section = trim(line.substr(1, line.size() - 2));
            continue;
        }
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(line.substr(0, eq));
        std::string val = trim(line.substr(eq + 1));
        g_sections[section][key] = val;
    }
}

}  // namespace

Config& Config::instance() {
    static Config cfg;
    return cfg;
}

bool Config::init(const std::string& base_dir) {
    base_dir_ = base_dir;
    mkdir(base_dir_.c_str(), 0755);
    if (!load()) {
        ARIFT_WARN(kTagCore, "Config load failed, using defaults");
    }
    ARIFT_INFO(kTagCore, "Config initialized at %s", configPath().c_str());
    return true;
}

void Config::shutdown() {
    save();
    g_sections.clear();
}

bool Config::load() {
    std::ifstream in(configPath());
    if (!in) return false;
    std::stringstream ss;
    ss << in.rdbuf();
    raw_ = ss.str();
    rebuild_raw();
    return true;
}

bool Config::save() const {
    std::ofstream out(configPath(), std::ios::trunc);
    if (!out) return false;
    for (const auto& [section, kv] : g_sections) {
        out << "[" << section << "]\n";
        for (const auto& [k, v] : kv) {
            out << k << " = " << v << "\n";
        }
        out << "\n";
    }
    out.flush();
    return out.good();
}

std::string Config::getString(const std::string& section, const std::string& key,
                              const std::string& def) const {
    auto it = g_sections.find(section);
    if (it == g_sections.end()) return def;
    auto jt = it->second.find(key);
    if (jt == it->second.end()) return def;
    return jt->second;
}

int Config::getInt(const std::string& section, const std::string& key, int def) const {
    auto v = getString(section, key, "");
    if (v.empty()) return def;
    try {
        return std::stoi(v);
    } catch (...) {
        return def;
    }
}

float Config::getFloat(const std::string& section, const std::string& key, float def) const {
    auto v = getString(section, key, "");
    if (v.empty()) return def;
    try {
        return std::stof(v);
    } catch (...) {
        return def;
    }
}

bool Config::getBool(const std::string& section, const std::string& key, bool def) const {
    auto v = getString(section, key, "");
    if (v.empty()) return def;
    return v == "1" || v == "true" || v == "yes" || v == "on";
}

void Config::setString(const std::string& section, const std::string& key,
                       const std::string& value) {
    g_sections[section][key] = value;
    dirty_ = true;
}

void Config::setInt(const std::string& section, const std::string& key, int value) {
    setString(section, key, std::to_string(value));
}

void Config::setFloat(const std::string& section, const std::string& key, float value) {
    setString(section, key, utils::format("%.6f", value));
}

void Config::setBool(const std::string& section, const std::string& key, bool value) {
    setString(section, key, value ? "1" : "0");
}

}  // namespace arift