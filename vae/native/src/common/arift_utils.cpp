#include "arift_utils.h"

#include <sys/time.h>
#include <time.h>

#include <chrono>
#include <cstdarg>
#include <cmath>
#include <cstdlib>
#include <random>
#include <sstream>

namespace arift {
namespace utils {

namespace {
std::mt19937_64& rng() {
    static thread_local std::mt19937_64 gen(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    return gen;
}
}  // namespace

std::string format(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char buf[1024];
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    return std::string(buf);
}

std::string hex(uint64_t value) {
    char buf[32];
    snprintf(buf, sizeof(buf), "0x%llx", static_cast<unsigned long long>(value));
    return buf;
}

std::string hexDump(const uint8_t* data, size_t len, size_t max) {
    std::ostringstream oss;
    size_t n = len < max ? len : max;
    for (size_t i = 0; i < n; ++i) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%02x ", data[i]);
        oss << buf;
        if ((i + 1) % 16 == 0) oss << "\n";
    }
    if (len > max) oss << "... (+" << (len - max) << " bytes)";
    return oss.str();
}

std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == delim) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    out.push_back(cur);
    return out;
}

std::string toLower(const std::string& s) {
    std::string out = s;
    for (char& c : out) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string toUpper(const std::string& s) {
    std::string out = s;
    for (char& c : out) c = static_cast<char>(::toupper(static_cast<unsigned char>(c)));
    return out;
}

bool startsWith(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool endsWith(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string randomString(size_t len) {
    static const char* alphabet =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    std::string out;
    out.reserve(len);
    for (size_t i = 0; i < len; ++i) {
        out.push_back(alphabet[rng()() % 62]);
    }
    return out;
}

uint32_t random32() {
    return static_cast<uint32_t>(rng()());
}

uint64_t random64() {
    return rng()();
}

float randomFloat(float lo, float hi) {
    std::uniform_real_distribution<float> d(lo, hi);
    return d(rng());
}

double gaussian(double mean, double stddev) {
    std::normal_distribution<double> d(mean, stddev);
    return d(rng());
}

uint64_t fnv1a64(const void* data, size_t len) {
    const auto* p = static_cast<const uint8_t*>(data);
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < len; ++i) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

uint64_t fnv1a64(const std::string& s) {
    return fnv1a64(s.data(), s.size());
}

uint32_t crc32(const void* data, size_t len) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            }
            table[i] = c;
        }
        init = true;
    }
    const auto* p = static_cast<const uint8_t*>(data);
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

int64_t nowMs() {
    struct timeval tv{};
    gettimeofday(&tv, nullptr);
    return static_cast<int64_t>(tv.tv_sec) * 1000 + tv.tv_usec / 1000;
}

int64_t monotonicMs() {
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

size_t alignUp(size_t v, size_t align) {
    return (v + align - 1) & ~(align - 1);
}

uintptr_t alignUpPtr(uintptr_t v, size_t align) {
    return (v + align - 1) & ~static_cast<uintptr_t>(align - 1);
}

bool isPow2(size_t v) {
    return v && !(v & (v - 1));
}

}  // namespace utils
}  // namespace arift