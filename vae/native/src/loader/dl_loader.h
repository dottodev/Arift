#pragma once

#include <cstdint>
#include <map>
#include <string>

#include "elf_parser.h"

namespace arift {

// Hidden library loader: maps a .so from memory/disk without touching the
// standard linker namespace (stealth requirement for injected payloads).
class DlLoader {
public:
    struct LoadedModule {
        std::string name;
        uintptr_t base = 0;
        size_t size = 0;
        ElfParser parser;
        bool valid = false;
    };

    // Map a library from a byte buffer. `flags` accepts MAP_PRIVATE etc.
    static LoadedModule* loadFromBuffer(const std::string& name,
                                        const uint8_t* data, size_t size,
                                        uintptr_t preferBase = 0);

    // Load from disk path (reads file then delegates).
    static LoadedModule* loadFromFile(const std::string& path,
                                      uintptr_t preferBase = 0);

    // Resolve an exported symbol within a loaded module.
    static uintptr_t resolve(const LoadedModule* mod, const std::string& name);

    // Lookup previously loaded module by name.
    static LoadedModule* find(const std::string& name);

    // Unload (unmap) a module we loaded.
    static bool unload(LoadedModule* mod);

    static size_t loadedCount();

private:
    static std::map<std::string, LoadedModule>& registry();
};

}  // namespace arift