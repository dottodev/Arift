#pragma once

#include <cstdint>
#include <elf.h>
#include <map>
#include <string>
#include <vector>

#ifndef ElfW
#define ElfW(type) Elf64_##type
#endif

namespace arift {

// Minimal ELF64 parser — enough to resolve symbols, sections and dynamic
// metadata from a loaded library image in memory.
class ElfParser {
public:
    struct Section {
        std::string name;
        uint32_t type = 0;
        uint64_t offset = 0;
        uint64_t addr = 0;
        uint64_t size = 0;
        uint64_t link = 0;
        uint64_t info = 0;
        uint64_t align = 0;
        uint64_t entsize = 0;
    };

    struct Symbol {
        std::string name;
        uint8_t info = 0;
        uint8_t other = 0;
        uint16_t shndx = 0;
        uint64_t value = 0;
        uint64_t size = 0;
        bool isFunc() const { return (info & 0x0F) == STT_FUNC; }
    };

    bool parse(const uint8_t* data, size_t size);
    bool parseFromFile(const std::string& path);

    bool valid() const { return valid_; }
    uint32_t type() const { return type_; }
    uint64_t entry() const { return entry_; }
    uint64_t bias() const { return bias_; }

    const std::vector<Section>& sections() const { return sections_; }
    Section* findSection(const std::string& name);

    std::vector<Symbol> exportedSymbols();
    Symbol* findExported(const std::string& name);
    std::vector<Symbol> importedSymbols();

    // Resolve `name` to a runtime address given the module base.
    uintptr_t resolveExport(const std::string& name, uintptr_t moduleBase) const;

    std::string lastError() const { return error_; }

private:
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
    bool valid_ = false;
    uint32_t type_ = 0;
    uint64_t entry_ = 0;
    uint64_t bias_ = 0;
    std::string error_;
    std::vector<Section> sections_;
    std::vector<Symbol> dynsyms_;
    std::vector<Symbol> syms_;
    std::string dynstr_;
    std::string strtab_;
};

}  // namespace arift