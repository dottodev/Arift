#include "elf_parser.h"

#include <elf.h>
#include <fcntl.h>
#include <unistd.h>

#include <cstring>

#include "arift_log.h"

namespace arift {

bool ElfParser::parse(const uint8_t* data, size_t size) {
    data_ = data;
    size_ = size;
    valid_ = false;
    sections_.clear();
    dynsyms_.clear();
    syms_.clear();
    dynstr_.clear();
    strtab_.clear();
    error_.clear();

    if (!data_ || size_ < sizeof(ElfW(Ehdr))) {
        error_ = "too small for ehdr";
        return false;
    }
    const auto* ehdr = reinterpret_cast<const ElfW(Ehdr)*>(data_);
    if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0) {
        error_ = "bad magic";
        return false;
    }
    if (ehdr->e_ident[EI_CLASS] != ELFCLASS64) {
        error_ = "not 64-bit";
        return false;
    }
    if (ehdr->e_machine != EM_AARCH64 && ehdr->e_machine != EM_X86_64) {
        error_ = "unsupported machine";
        return false;
    }
    type_ = ehdr->e_type;
    entry_ = ehdr->e_entry;

    // Section headers
    if (ehdr->e_shoff > 0 && ehdr->e_shnum > 0) {
        const auto* shdr = reinterpret_cast<const ElfW(Shdr)*>(data_ + ehdr->e_shoff);
        size_t shnum = ehdr->e_shnum;
        const char* shstr = nullptr;
        if (ehdr->e_shstrndx < shnum) {
            shstr = reinterpret_cast<const char*>(data_ + shdr[ehdr->e_shstrndx].sh_offset);
        }
        for (size_t i = 0; i < shnum; ++i) {
            Section s;
            s.name = shstr ? shstr + shdr[i].sh_name : "";
            s.type = shdr[i].sh_type;
            s.offset = shdr[i].sh_offset;
            s.addr = shdr[i].sh_addr;
            s.size = shdr[i].sh_size;
            s.link = shdr[i].sh_link;
            s.info = shdr[i].sh_info;
            s.align = shdr[i].sh_addralign;
            s.entsize = shdr[i].sh_entsize;
            sections_.push_back(s);
        }
    }

    // Dynamic symbol table + string table (usually via section headers).
    Section* dynsymSec = findSection(".dynsym");
    Section* dynstrSec = findSection(".dynstr");
    if (dynsymSec && dynstrSec &&
        dynsymSec->offset + dynsymSec->size <= size_ &&
        dynstrSec->offset + dynstrSec->size <= size_) {
        dynstr_.assign(reinterpret_cast<const char*>(data_ + dynstrSec->offset),
                       dynstrSec->size);
        const auto* syms = reinterpret_cast<const ElfW(Sym)*>(data_ + dynsymSec->offset);
        size_t count = dynsymSec->size / sizeof(ElfW(Sym));
        for (size_t i = 0; i < count; ++i) {
            Symbol s;
            s.name = syms[i].st_name < dynstr_.size()
                         ? dynstr_.c_str() + syms[i].st_name
                         : "";
            s.info = syms[i].st_info;
            s.other = syms[i].st_other;
            s.shndx = syms[i].st_shndx;
            s.value = syms[i].st_value;
            s.size = syms[i].st_size;
            dynsyms_.push_back(s);
        }
    }

    Section* symSec = findSection(".symtab");
    Section* strSec = findSection(".strtab");
    if (symSec && strSec &&
        symSec->offset + symSec->size <= size_ &&
        strSec->offset + strSec->size <= size_) {
        strtab_.assign(reinterpret_cast<const char*>(data_ + strSec->offset),
                       strSec->size);
        const auto* syms = reinterpret_cast<const ElfW(Sym)*>(data_ + symSec->offset);
        size_t count = symSec->size / sizeof(ElfW(Sym));
        for (size_t i = 0; i < count; ++i) {
            Symbol s;
            s.name = syms[i].st_name < strtab_.size()
                         ? strtab_.c_str() + syms[i].st_name
                         : "";
            s.info = syms[i].st_info;
            s.other = syms[i].st_other;
            s.shndx = syms[i].st_shndx;
            s.value = syms[i].st_value;
            s.size = syms[i].st_size;
            syms_.push_back(s);
        }
    }

    valid_ = true;
    return true;
}

bool ElfParser::parseFromFile(const std::string& path) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        error_ = "cannot open " + path;
        return false;
    }
    off_t len = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    if (len <= 0) {
        ::close(fd);
        error_ = "empty file";
        return false;
    }
    std::vector<uint8_t> buf(static_cast<size_t>(len));
    size_t got = 0;
    while (got < buf.size()) {
        ssize_t n = read(fd, buf.data() + got, buf.size() - got);
        if (n <= 0) break;
        got += static_cast<size_t>(n);
    }
    ::close(fd);
    if (got != buf.size()) {
        error_ = "short read";
        return false;
    }
    return parse(buf.data(), buf.size());
}

ElfParser::Section* ElfParser::findSection(const std::string& name) {
    for (auto& s : sections_) {
        if (s.name == name) return &s;
    }
    return nullptr;
}

std::vector<ElfParser::Symbol> ElfParser::exportedSymbols() {
    std::vector<Symbol> out;
    for (const auto& s : dynsyms_) {
        if (s.name.empty()) continue;
        if (ELF64_ST_BIND(s.info) == STB_GLOBAL ||
            ELF64_ST_BIND(s.info) == STB_WEAK) {
            out.push_back(s);
        }
    }
    return out;
}

ElfParser::Symbol* ElfParser::findExported(const std::string& name) {
    for (auto& s : dynsyms_) {
        if (s.name == name) return &s;
    }
    for (auto& s : syms_) {
        if (s.name == name) return &s;
    }
    return nullptr;
}

std::vector<ElfParser::Symbol> ElfParser::importedSymbols() {
    std::vector<Symbol> out;
    for (const auto& s : dynsyms_) {
        if (s.shndx == SHN_UNDEF && !s.name.empty()) out.push_back(s);
    }
    return out;
}

uintptr_t ElfParser::resolveExport(const std::string& name,
                                   uintptr_t moduleBase) const {
    for (const auto& s : dynsyms_) {
        if (s.name == name && s.value != 0) {
            return moduleBase + s.value;
        }
    }
    return 0;
}

}  // namespace arift