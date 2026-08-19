#include "memory_map.h"

#include <fstream>
#include <sstream>

#include "arift_log.h"
#include "arift_time.h"
#include "arift_utils.h"

namespace arift {

namespace {

MemRegionType classify(const std::string& path, bool executable, bool writable) {
    if (path.empty()) {
        if (writable) return MemRegionType::kMapped;
        return MemRegionType::kOther;
    }
    if (executable) return MemRegionType::kCode;
    if (writable) return MemRegionType::kData;
    return MemRegionType::kOther;
}

}  // namespace

std::string MemRegion::describe() const {
    char buf[512];
    snprintf(buf, sizeof(buf),
             "[%s] 0x%llx-0x%llx size=0x%llx %c%c%c %s",
             path.empty() ? "<anon>" : path.c_str(),
             static_cast<unsigned long long>(start),
             static_cast<unsigned long long>(end),
             static_cast<unsigned long long>(size),
             readable ? 'r' : '-',
             writable ? 'w' : '-',
             executable ? 'x' : '-',
             shared ? "shared" : "private");
    return std::string(buf);
}

MemoryMap& MemoryMap::instance() {
    static MemoryMap map;
    return map;
}

int MemoryMap::refresh(int pid) {
    regions_.clear();
    pid_ = pid;
    last_refresh_ms_ = utils::monotonicMs();

    std::string path = "/proc/" + std::to_string(pid) + "/maps";
    std::ifstream in(path);
    if (!in) {
        ARIFT_WARN(kTagMemory, "Cannot open %s", path.c_str());
        return 0;
    }

    std::string line;
    int count = 0;
    while (std::getline(in, line)) {
        MemRegion r;
        char perms[5] = {0};
        char dev[16] = {0};
        char pathbuf[512] = {0};
        int major = 0, minor = 0;
        long inode = 0;
        unsigned long long start = 0, end = 0, offset = 0;

        int parsed = sscanf(line.c_str(), "%llx-%llx %4s %llx %15s %ld %511[^\n]",
                            &start, &end, perms, &offset, dev, &inode, pathbuf);
        if (parsed < 6) continue;

        r.start = static_cast<uintptr_t>(start);
        r.end = static_cast<uintptr_t>(end);
        r.offset = offset;
        r.size = r.end - r.start;
        r.readable = perms[0] == 'r';
        r.writable = perms[1] == 'w';
        r.executable = perms[2] == 'x';
        r.shared = perms[3] == 's';
        r.path = pathbuf;
        if (sscanf(dev, "%d:%d", &major, &minor) == 2) {
            r.major = major;
            r.minor = minor;
        }
        r.inode = inode;
        r.type = classify(r.path, r.executable, r.writable);
        regions_.push_back(r);
        ++count;
    }

    ARIFT_DEBUG(kTagMemory, "Refreshed map for pid=%d: %d regions", pid, count);
    return count;
}

MemRegion* MemoryMap::findContaining(uintptr_t addr) {
    for (auto& r : regions_) {
        if (r.contains(addr)) return &r;
    }
    return nullptr;
}

std::vector<MemRegion*> MemoryMap::findByPath(const std::string& pathPart) {
    std::vector<MemRegion*> out;
    for (auto& r : regions_) {
        if (r.path.find(pathPart) != std::string::npos) {
            out.push_back(&r);
        }
    }
    return out;
}

MemRegion* MemoryMap::findExecutableBase(const std::string& libPath) {
    MemRegion* best = nullptr;
    for (auto& r : regions_) {
        if (r.executable && r.path == libPath) {
            if (!best || r.start < best->start) best = &r;
        }
    }
    return best;
}

uintptr_t MemoryMap::moduleBase(const std::string& libName) const {
    uintptr_t base = 0;
    for (const auto& r : regions_) {
        if (r.executable && r.path.find(libName) != std::string::npos) {
            if (base == 0 || r.start < base) base = r.start;
        }
    }
    return base;
}

}  // namespace arift