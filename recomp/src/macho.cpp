#include "macho.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace recomp {

namespace {

constexpr uint32_t MH_MAGIC = 0xfeedface;
constexpr uint32_t LC_SEGMENT = 0x1, LC_SYMTAB = 0x2, LC_DYSYMTAB = 0xb;
constexpr uint32_t S_NON_LAZY_SYMBOL_POINTERS = 6, S_LAZY_SYMBOL_POINTERS = 7,
                   S_SYMBOL_STUBS = 8;
constexpr uint32_t INDIRECT_SYMBOL_LOCAL = 0x80000000, INDIRECT_SYMBOL_ABS = 0x40000000;
constexpr uint8_t N_STAB = 0xe0, N_TYPE = 0x0e, N_SECT = 0x0e;

[[noreturn]] void fail(const std::string& msg) {
    std::fprintf(stderr, "macho: %s\n", msg.c_str());
    std::exit(1);
}

uint32_t u32(const std::vector<uint8_t>& d, size_t off) {
    if (off + 4 > d.size()) fail("truncated file");
    uint32_t v;
    std::memcpy(&v, d.data() + off, 4);
    return v;
}

std::string str16(const std::vector<uint8_t>& d, size_t off) {
    return std::string(reinterpret_cast<const char*>(d.data() + off),
                       strnlen(reinterpret_cast<const char*>(d.data() + off), 16));
}

}  // namespace

Image::Image(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) fail("cannot open " + path);
    data_.assign(std::istreambuf_iterator<char>(in), {});
    if (data_.size() > 0x40 && data_[0] == 'M' && data_[1] == 'Z') {
        load_pe();
        return;
    }
    if (u32(data_, 0) != MH_MAGIC || u32(data_, 4) != 7) fail("not a thin i386 Mach-O");

    const uint32_t ncmds = u32(data_, 16);
    uint32_t symoff = 0, nsyms = 0, stroff = 0, indirectoff = 0, nindirect = 0, extreloff = 0, nextrel = 0;
    size_t off = 28;
    for (uint32_t k = 0; k < ncmds; k++) {
        const uint32_t cmd = u32(data_, off), size = u32(data_, off + 4);
        if (cmd == LC_SEGMENT) {
            Segment s;
            s.name = str16(data_, off + 8);
            s.vmaddr = u32(data_, off + 24);
            s.vmsize = u32(data_, off + 28);
            s.fileoff = u32(data_, off + 32);
            s.filesize = u32(data_, off + 36);
            s.initprot = u32(data_, off + 44);
            segments_.push_back(s);
            const uint32_t nsects = u32(data_, off + 48);
            for (uint32_t i = 0; i < nsects; i++) {
                const size_t p = off + 56 + i * 68;
                Section sec;
                sec.sectname = str16(data_, p);
                sec.segname = str16(data_, p + 16);
                sec.addr = u32(data_, p + 32);
                sec.size = u32(data_, p + 36);
                sec.offset = u32(data_, p + 40);
                sec.flags = u32(data_, p + 56);
                sec.reserved1 = u32(data_, p + 60);
                sec.reserved2 = u32(data_, p + 64);
                sections_.push_back(sec);
            }
        } else if (cmd == LC_SYMTAB) {
            symoff = u32(data_, off + 8);
            nsyms = u32(data_, off + 12);
            stroff = u32(data_, off + 16);
        } else if (cmd == LC_DYSYMTAB) {
            indirectoff = u32(data_, off + 56);
            nindirect = u32(data_, off + 60);
            extreloff = u32(data_, off + 64);
            nextrel = u32(data_, off + 68);
        }
        off += size;
    }

    // Symbols. Section numbers are 1-based over all sections in load order.
    struct Sym {
        uint32_t value;
        uint8_t type, sect;
        std::string name;
    };
    std::vector<Sym> syms(nsyms);
    for (uint32_t i = 0; i < nsyms; i++) {
        const size_t p = symoff + i * 12;
        const uint32_t strx = u32(data_, p);
        syms[i].type = data_[p + 4];
        syms[i].sect = data_[p + 5];
        syms[i].value = u32(data_, p + 8);
        syms[i].name = reinterpret_cast<const char*>(data_.data() + stroff + strx);
    }
    uint8_t text_index = 0;
    for (size_t i = 0; i < sections_.size(); i++)
        if (sections_[i].segname == "__TEXT" && sections_[i].sectname == "__text") {
            text_ = &sections_[i];
            text_index = static_cast<uint8_t>(i + 1);
        }
    if (!text_) fail("no __TEXT,__text");

    std::map<uint32_t, std::string> starts;
    for (const Sym& s : syms) {
        if (s.type & N_STAB || (s.type & N_TYPE) != N_SECT) continue;
        defined_.emplace(s.name, s.value);
        if (s.sect != text_index) data_symbols_.emplace(s.value, s.name);
        if (s.sect == text_index) {
            auto it = starts.find(s.value);
            // Prefer external names; keep the first otherwise.
            if (it == starts.end() || ((s.type & 1) && it->second.empty()))
                starts[s.value] = s.name;
        }
    }
    const uint32_t text_end = text_->addr + text_->size;
    for (auto it = starts.begin(); it != starts.end(); ++it) {
        auto next = std::next(it);
        functions_.push_back({it->first, next == starts.end() ? text_end : next->first,
                              it->second});
    }

    // Static initializers in other code sections (GCC's __TEXT,__StaticInit)
    // have no symbols: take their starts from __mod_init_func, plus the start of
    // such a section (a helper the stubs tail-jump to); each runs to the next one
    // or the end of its section.
    std::map<uint32_t, const Section*> extra;
    for (const Section& sec : sections_) {
        if (sec.type() != 9) continue;  // S_MOD_INIT_FUNC_POINTERS
        for (uint32_t a = sec.addr; a + 4 <= sec.addr + sec.size; a += 4) {
            const uint8_t* p = at(a, 4);
            if (!p) break;
            uint32_t v;
            std::memcpy(&v, p, 4);
            if (v >= text_->addr && v < text_end) continue;
            for (const Section& code : sections_)
                if (code.segname == "__TEXT" && v >= code.addr &&
                    v < code.addr + code.size) {
                    extra[v] = &code;
                    // The stubs tail-jump to a shared helper at the section start.
                    extra[code.addr] = &code;
                }
        }
    }
    for (auto it = extra.begin(); it != extra.end(); ++it) {
        auto next = std::next(it);
        const uint32_t sec_end = it->second->addr + it->second->size;
        const uint32_t end = next != extra.end() && next->first < sec_end ? next->first : sec_end;
        char name[64];
        std::snprintf(name, sizeof name, "%s+0x%x", it->second->sectname.c_str(),
                      it->first - it->second->addr);
        functions_.push_back({it->first, end, name});
    }
    std::sort(functions_.begin(), functions_.end(),
              [](const Function& a, const Function& b) { return a.addr < b.addr; });

    // Stubs and pointer slots via the indirect symbol table.
    for (const Section& sec : sections_) {
        const uint32_t t = sec.type();
        if (t != S_SYMBOL_STUBS && t != S_NON_LAZY_SYMBOL_POINTERS && t != S_LAZY_SYMBOL_POINTERS)
            continue;
        const uint32_t entry = t == S_SYMBOL_STUBS ? sec.reserved2 : 4;
        if (!entry) fail("stub section without entry size");
        for (uint32_t i = 0; i < sec.size / entry; i++) {
            const uint32_t idx = sec.reserved1 + i;
            if (idx >= nindirect) fail("indirect symbol index out of range");
            const uint32_t symi = u32(data_, indirectoff + idx * 4);
            if (symi & (INDIRECT_SYMBOL_LOCAL | INDIRECT_SYMBOL_ABS)) continue;
            Import imp;
            imp.name = syms.at(symi).name;
            auto d = defined_.find(imp.name);
            if (d != defined_.end()) imp.internal = d->second;
            (t == S_SYMBOL_STUBS ? stubs_ : slots_)[sec.addr + i * entry] = imp;
        }
    }

    // External relocations: words in data that dyld fills with a symbol's
    // address (Brotherhood's linker emits them for every reference to a weak
    // definition, so dyld can coalesce those: its vtables are zeros in the
    // file). For symbols the image defines, the word is filled in here, so the
    // data reads as dyld would leave it; for imports it becomes a pointer slot.
    for (uint32_t k = 0; k < nextrel; k++) {
        const size_t p = extreloff + 8 * (size_t)k;
        const uint32_t addr = u32(data_, p), info = u32(data_, p + 4);
        const uint32_t symnum = info & 0xffffff;
        const bool pcrel = info >> 24 & 1, external = info >> 27 & 1;
        const uint32_t length = info >> 25 & 3, type = info >> 28;
        if (!external || pcrel || length != 2 || type != 0)
            fail("unsupported external relocation at " + std::to_string(addr));
        Import imp;
        imp.name = syms.at(symnum).name;
        auto d = defined_.find(imp.name);
        if (d == defined_.end()) {
            slots_[addr] = imp;
            continue;
        }
        size_t file = 0;
        for (const Segment& seg : segments_)
            if (addr >= seg.vmaddr && (uint64_t)addr + 4 <= (uint64_t)seg.vmaddr + seg.filesize)
                file = seg.fileoff + (addr - seg.vmaddr);
        if (!file || file + 4 > data_.size()) fail("external relocation outside the file: " + std::to_string(addr));
        const uint32_t v = u32(data_, file) + d->second;  // the addend is in place
        std::memcpy(data_.data() + file, &v, 4);
    }
}

const uint8_t* Image::at(uint32_t addr, uint32_t len) const {
    for (const Segment& s : segments_)
        if (addr >= s.vmaddr && (uint64_t)addr + len <= (uint64_t)s.vmaddr + s.filesize)
            return data_.data() + s.fileoff + (addr - s.vmaddr);
    return nullptr;
}

const Function* Image::function_at(uint32_t addr) const {
    auto it = std::lower_bound(functions_.begin(), functions_.end(), addr,
                               [](const Function& f, uint32_t a) { return f.addr < a; });
    return it != functions_.end() && it->addr == addr ? &*it : nullptr;
}

const Function* Image::function_containing(uint32_t addr) const {
    auto it = std::upper_bound(functions_.begin(), functions_.end(), addr,
                               [](uint32_t a, const Function& f) { return a < f.addr; });
    if (it == functions_.begin()) return nullptr;
    --it;
    return addr < it->end ? &*it : nullptr;
}

bool Image::split_function(uint32_t addr) {
    auto it = std::upper_bound(functions_.begin(), functions_.end(), addr,
                               [](uint32_t a, const Function& f) { return a < f.addr; });
    if (it == functions_.begin()) return false;
    --it;
    if (addr >= it->end || addr == it->addr) return false;
    char off[16];
    std::snprintf(off, sizeof off, "+0x%x", addr - it->addr);
    Function g{addr, it->end, it->name + off};
    it->end = addr;
    functions_.insert(it + 1, g);
    return true;
}

const Function* Image::function_named(const std::string& name) const {
    const uint32_t a = symbol(name);
    return a ? function_at(a) : nullptr;
}

std::vector<uint32_t> Image::vtable_functions(uint32_t addr) const {
    std::vector<uint32_t> out;
    if (pe_) {
        auto v = vtable_at_.find(addr);
        if (v != vtable_at_.end())
            for (uint32_t f : vtables_[v->second].slots)
                if (function_at(f)) out.push_back(f);
        return out;
    }
    auto it = data_symbols_.upper_bound(addr);
    if (it == data_symbols_.begin()) return out;
    auto next = it;
    --it;
    if (it->second.rfind("__ZTV", 0) != 0) return out;
    const uint32_t end = next == data_symbols_.end() ? it->first : next->first;
    if (addr >= end) return out;
    for (uint32_t a = it->first; a + 4 <= end; a += 4) {
        const uint8_t* p = at(a, 4);
        if (!p) break;
        uint32_t v;
        std::memcpy(&v, p, 4);
        if (function_at(v)) out.push_back(v);
    }
    return out;
}

const Section* Image::code_section(uint32_t addr) const {
    for (const Section& s : sections_) {
        const bool code = pe_ ? (s.flags & 0x20000000u) != 0  // IMAGE_SCN_MEM_EXECUTE
                              : s.segname == "__TEXT" && (s.flags & 0x80000400u) != 0;
        if (code && addr >= s.addr && addr < s.addr + s.size) return &s;
    }
    return nullptr;
}

void Image::set_function_starts(const std::map<uint32_t, std::string>& starts) {
    functions_.clear();
    functions_.reserve(starts.size());
    for (const auto& [a, name] : starts) {
        const Section* s = code_section(a);
        functions_.push_back({a, s ? s->addr + s->size : a + 1, name});
    }
    defined_.clear();
    for (const Function& f : functions_) defined_.emplace(f.name, f.addr);
    for (const Export& e : exports_) defined_.emplace(e.name, e.addr);
}

uint32_t Image::symbol(const std::string& name) const {
    auto it = defined_.find(name);
    return it == defined_.end() ? 0 : it->second;
}

}  // namespace recomp
