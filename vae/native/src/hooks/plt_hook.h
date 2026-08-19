#pragma once

#include <cstdint>
#include <map>
#include <string>

namespace arift {

// PLT/GOT hooking: intercepts calls that go through a library's PLT
// (e.g. malloc, free, connect, sendto). Useful for network-layer features.
class PltHook {
public:
    struct PltEntry {
        std::string symbol;
        uintptr_t gotAddr = 0;      // address of the GOT slot
        uintptr_t original = 0;     // original function pointer
    };

    // Locate the GOT slot for `symbol` in the module `libPath` of the
    // current process (self) or a target process (via /proc/<pid>/mem).
    static PltEntry* findSlot(const std::string& libPath, const std::string& symbol);

    // Overwrite the GOT slot to point at `replacement`.
    // Returns the previous value in *outOriginal.
    static bool hook(uintptr_t gotAddr, uintptr_t replacement,
                     uintptr_t* outOriginal = nullptr);

    // Restore the original pointer.
    static bool unhook(uintptr_t gotAddr, uintptr_t original);

    // Parse the ELF dynamic symbol table to find `symbol` -> GOT slot.
    // Requires the module base and its size (from MemoryMap).
    static uintptr_t findGotSlotInElf(const uint8_t* elfData, size_t elfSize,
                                      uintptr_t moduleBase,
                                      const std::string& symbol);

    // Registry of active PLT hooks for the current process.
    static std::map<uintptr_t, PltEntry>& activeHooks();
};

}  // namespace arift