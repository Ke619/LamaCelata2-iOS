#pragma once
// Minimal reader for the games' executables: thin i386 Mach-O (the Mac ports)
// and PE32 (the Windows releases, recomp/src/pe.cpp).
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace recomp {

// PE sections are described in Mach-O terms: segname "__TEXT" for read-only
// sections (code, constants, resources), "__DATA" for writable ones; sectname
// is the PE section name (".text", ".rdata"...). `flags` keeps the PE
// characteristics, so type() means nothing for them.
struct Section {
    std::string segname, sectname;
    uint32_t addr = 0, size = 0, offset = 0, flags = 0, reserved1 = 0, reserved2 = 0;
    uint32_t type() const { return flags & 0xff; }
};

struct Segment {
    std::string name;
    uint32_t vmaddr = 0, vmsize = 0, fileoff = 0, filesize = 0, initprot = 0;
};

struct Function {
    uint32_t addr = 0, end = 0;
    std::string name;  // mangled
};

// An import stub (`jmp *slot`) or pointer slot bound by dyld to `name`.
// For weak symbols the image defines itself, `internal` is the definition.
// For PE, the import address table slots play the role of stubs: `call
// [slot]` and `jmp [slot]` are import calls. `name` is the function's name
// (ordinal imports of known DLLs get their documented name) and `dll` its
// library.
struct Import {
    std::string name;
    uint32_t internal = 0;
    std::string dll;  // PE only
};

// PE: an exported function or datum.
struct Export {
    std::string name;
    uint32_t addr = 0;
};

// PE: a virtual function table found through MSVC RTTI (the complete object
// locator in the word before it), with the class's decorated type name
// (".?AVFoo@scimitar@@") and its function pointers.
struct Vtable {
    uint32_t addr = 0;
    std::string type;
    std::vector<uint32_t> slots;
};

// PE: the static TLS directory (template data, index slot, callbacks).
struct TlsDirectory {
    uint32_t start = 0, end = 0, index_addr = 0, callbacks = 0, zero_fill = 0;
};

class Image {
public:
    explicit Image(const std::string& path);

    bool is_pe() const { return pe_; }
    uint32_t entry() const { return entry_; }  // PE: the entry point; Mach-O: 0
    uint32_t image_base() const { return image_base_; }
    const std::vector<Export>& exports() const { return exports_; }
    const std::vector<Vtable>& vtables() const { return vtables_; }
    const TlsDirectory& tls() const { return tls_; }
    // PE: base relocations (IMAGE_REL_BASED_HIGHLOW), site -> the address the
    // word there holds. Empty for images without a .reloc section (AC1).
    const std::map<uint32_t, uint32_t>& relocations() const { return relocations_; }
    // The executable section holding `addr`, or nullptr.
    const Section* code_section(uint32_t addr) const;
    // PE functions have no symbols: discover_functions finds their starts and
    // installs them here (names "sub_XXXXXXXX" unless known from exports or
    // vtables). Each extends to the end of its section; decoding follows
    // control flow and stops at other functions' starts.
    void set_function_starts(const std::map<uint32_t, std::string>& starts);

    const std::vector<uint8_t>& data() const { return data_; }
    const std::vector<Segment>& segments() const { return segments_; }
    const std::vector<Section>& sections() const { return sections_; }
    const Section* text() const { return text_; }

    // Bytes at a virtual address (nullptr when unmapped or past the file data).
    const uint8_t* at(uint32_t addr, uint32_t len) const;

    // Functions in __text, sorted by address; extents run to the next symbol.
    const std::vector<Function>& functions() const { return functions_; }
    const Function* function_at(uint32_t addr) const;  // exact start
    const Function* function_containing(uint32_t addr) const;
    const Function* function_named(const std::string& name) const;

    // Symbol stubs by stub address, and pointer slots by slot address.
    const std::map<uint32_t, Import>& stubs() const { return stubs_; }
    const std::map<uint32_t, Import>& pointer_slots() const { return slots_; }

    // Address of a defined symbol, or 0.
    uint32_t symbol(const std::string& name) const;

    // Makes `addr`, which lies inside a function's extent, the start of a new
    // function ending where the old one ended (whose extent now stops at
    // `addr`). For code without a symbol, such as stripped static helpers
    // that direct calls reach. Named "<old>+0xOFFSET". False if `addr` is not
    // inside a function or already starts one.
    bool split_function(uint32_t addr);

    // If `addr` lies inside a C++ vtable symbol (__ZTV...), the functions its
    // slots point to; empty otherwise.
    std::vector<uint32_t> vtable_functions(uint32_t addr) const;

private:
    std::vector<uint8_t> data_;
    std::vector<Segment> segments_;
    std::vector<Section> sections_;
    const Section* text_ = nullptr;
    std::vector<Function> functions_;
    std::map<std::string, uint32_t> defined_;
    std::map<uint32_t, std::string> data_symbols_;  // non-__text symbols by address
    std::map<uint32_t, Import> stubs_, slots_;

    bool pe_ = false;
    uint32_t entry_ = 0, image_base_ = 0;
    std::vector<Export> exports_;
    std::vector<Vtable> vtables_;
    std::map<uint32_t, size_t> vtable_at_;  // vtable address -> index in vtables_
    TlsDirectory tls_;
    std::map<uint32_t, uint32_t> relocations_;
    void load_pe();
};

}  // namespace recomp
