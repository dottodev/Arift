#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace arift {

enum class MemRegionType : int {
    kUnknown = 0,
    kCode = 1,        // r-xp / r--p of .text
    kData = 2,        // rw-p data, heap
    kMapped = 3,      // anonymous mappings, JIT
    kStack = 4,
    kOther = 5,
};

struct MemRegion {
    uintptr_t start = 0;
    uintptr_t end = 0;
    uint64_t size = 0;
    bool readable = false;
    bool writable = false;
    bool executable = false;
    bool shared = false;
    MemRegionType type = MemRegionType::kUnknown;
    std::string path;
    uint64_t offset = 0;
    int major = 0;
    int minor = 0;
    long inode = 0;

    bool contains(uintptr_t addr) const { return addr >= start && addr < end; }
    bool isCode() const { return executable; }
    bool isWritable() const { return writable; }
    std::string describe() const;
};

class MemoryMap {
public:
    static MemoryMap& instance();

    // Refresh the map for the target pid. Returns number of regions.
    int refresh(int pid);

    const std::vector<MemRegion>& regions() const { return regions_; }

    MemRegion* findContaining(uintptr_t addr);
    std::vector<MemRegion*> findByPath(const std::string& pathPart);
    MemRegion* findExecutableBase(const std::string& libPath);

    // Returns the base (lowest exec address) of a mapped library.
    uintptr_t moduleBase(const std::string& libName) const;

    // Scan for a region satisfying predicate.
    template <typename Pred>
    MemRegion* firstRegion(Pred&& p) {
        for (auto& r : regions_) {
            if (p(r)) return &r;
        }
        return nullptr;
    }

    int pid() const { return pid_; }
    int64_t lastRefreshMs() const { return last_refresh_ms_; }

private:
    MemoryMap() = default;
    int pid_ = -1;
    int64_t last_refresh_ms_ = 0;
    std::vector<MemRegion> regions_;
};

}  // namespace arift