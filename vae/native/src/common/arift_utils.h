#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace arift {
namespace utils {

std::string format(const char* fmt, ...);

std::string hex(uint64_t value);
std::string hexDump(const uint8_t* data, size_t len, size_t max = 256);

std::vector<std::string> split(const std::string& s, char delim);
std::string toLower(const std::string& s);
std::string toUpper(const std::string& s);
bool startsWith(const std::string& s, const std::string& prefix);
bool endsWith(const std::string& s, const std::string& suffix);

std::string randomString(size_t len);
uint32_t random32();
uint64_t random64();
float randomFloat(float lo, float hi);
double gaussian(double mean, double stddev);

uint64_t fnv1a64(const void* data, size_t len);
uint64_t fnv1a64(const std::string& s);

uint32_t crc32(const void* data, size_t len);

int64_t nowMs();
int64_t monotonicMs();

// Clamp / clampf / lerp helpers
template <typename T>
T clamp(T v, T lo, T hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

inline float clampf(float v, float lo, float hi) { return clamp(v, lo, hi); }
inline float lerp(float a, float b, float t) { return a + (b - a) * t; }
inline double lerpd(double a, double b, double t) { return a + (b - a) * t; }
inline float smoothstep(float a, float b, float t) {
    t = clampf((t - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

size_t alignUp(size_t v, size_t align);
uintptr_t alignUpPtr(uintptr_t v, size_t align);

bool isPow2(size_t v);

// Root / SELinux helpers (used by ProcessMemory open fallback).
bool isRootAvailable();
bool setSelinuxPermissive();

}  // namespace utils
}  // namespace arift