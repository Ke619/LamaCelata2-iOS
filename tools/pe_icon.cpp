// pe_icon EXE OUT.ico: writes the first icon group of a Windows executable
// (its RT_GROUP_ICON resource and the RT_ICON images it names) as an .ico
// file. The Windows ports' apps take their icon from the installed game this
// way at build time (games/common/launcher/make_app.cmake); game art never
// enters the repository. Exit status 1 when the executable has no icon.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <optional>
#include <vector>

namespace {

std::vector<uint8_t> g_file;

uint32_t u16(size_t off) { return off + 2 <= g_file.size() ? g_file[off] | g_file[off + 1] << 8 : 0; }
uint32_t u32(size_t off) { return off + 4 <= g_file.size() ? u16(off) | u16(off + 2) << 16 : 0; }

struct Section {
    uint32_t rva, size, raw, raw_size;
};
std::vector<Section> g_sections;

std::optional<size_t> file_offset(uint32_t rva) {
    for (const Section& s : g_sections)
        if (rva >= s.rva && rva < s.rva + std::max(s.size, s.raw_size) && rva - s.rva < s.raw_size)
            return s.raw + (rva - s.rva);
    return std::nullopt;
}

size_t g_root = 0;  // the resource directory's file offset

// The entry of the directory at `dir` (a file offset) with the numeric id
// `id` (any id: the first entry). Returns the entry's value (high bit: a
// subdirectory, relative to the root).
std::optional<uint32_t> entry(size_t dir, std::optional<uint32_t> id) {
    const uint32_t named = u16(dir + 12), ids = u16(dir + 14);
    for (uint32_t k = 0; k < named + ids; k++) {
        const size_t e = dir + 16 + 8 * k;
        const uint32_t name = u32(e);
        if (!id || (!(name & 0x80000000u) && name == *id)) return u32(e + 4);
    }
    return std::nullopt;
}

// The data of resource type/id (first language), as a file range.
std::optional<std::pair<size_t, uint32_t>> resource(uint32_t type, std::optional<uint32_t> id) {
    auto t = entry(g_root, type);
    if (!t || !(*t & 0x80000000u)) return std::nullopt;
    auto n = entry(g_root + (*t & 0x7fffffffu), id);
    if (!n || !(*n & 0x80000000u)) return std::nullopt;
    auto l = entry(g_root + (*n & 0x7fffffffu), std::nullopt);
    if (!l || (*l & 0x80000000u)) return std::nullopt;
    const size_t data = g_root + *l;
    auto off = file_offset(u32(data));
    const uint32_t size = u32(data + 4);
    if (!off || *off + size > g_file.size()) return std::nullopt;
    return std::make_pair(*off, size);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: pe_icon EXE OUT.ico\n");
        return 2;
    }
    std::ifstream in(argv[1], std::ios::binary);
    g_file.assign(std::istreambuf_iterator<char>(in), {});
    if (g_file.size() < 0x40 || u16(0) != 0x5a4d) return std::fprintf(stderr, "pe_icon: %s: not an executable\n", argv[1]), 1;
    const size_t pe = u32(0x3c);
    if (u32(pe) != 0x4550) return std::fprintf(stderr, "pe_icon: %s: not a PE file\n", argv[1]), 1;
    const uint32_t sections = u16(pe + 6), optional_size = u16(pe + 20);
    const size_t opt = pe + 24;
    const size_t dirs = opt + (u16(opt) == 0x20b ? 112 : 96);  // PE32+ or PE32
    const uint32_t resource_rva = u32(dirs + 2 * 8);
    for (uint32_t k = 0; k < sections; k++) {
        const size_t s = opt + optional_size + 40 * k;
        g_sections.push_back({u32(s + 12), u32(s + 8), u32(s + 20), u32(s + 16)});
    }
    auto root = file_offset(resource_rva);
    if (!resource_rva || !root) return std::fprintf(stderr, "pe_icon: %s: no resources\n", argv[1]), 1;
    g_root = *root;

    auto group = resource(14, std::nullopt);  // RT_GROUP_ICON
    if (!group) return std::fprintf(stderr, "pe_icon: %s: no icon\n", argv[1]), 1;
    const size_t g = group->first;
    const uint32_t count = u16(g + 4);
    // ICONDIR, then 16-byte entries (the group's 14-byte ones with an offset
    // in place of the RT_ICON id), then the images.
    std::vector<std::pair<size_t, uint32_t>> images;
    std::vector<uint8_t> entries;
    for (uint32_t k = 0; k < count; k++) {
        const size_t e = g + 6 + 14 * k;
        auto image = resource(3, u16(e + 12));  // RT_ICON
        if (!image) continue;
        entries.insert(entries.end(), &g_file[e], &g_file[e + 8]);
        const uint32_t size = image->second;
        for (int b = 0; b < 4; b++) entries.push_back(size >> (8 * b) & 0xff);
        entries.insert(entries.end(), 4, 0);  // offset, below
        images.push_back(*image);
    }
    if (images.empty()) return std::fprintf(stderr, "pe_icon: %s: no icon images\n", argv[1]), 1;
    std::vector<uint8_t> ico = {0, 0, 1, 0, (uint8_t)images.size(), (uint8_t)(images.size() >> 8)};
    uint32_t offset = 6 + 16 * images.size();
    for (size_t k = 0; k < images.size(); k++) {
        for (int b = 0; b < 4; b++) entries[16 * k + 12 + b] = offset >> (8 * b) & 0xff;
        offset += images[k].second;
    }
    ico.insert(ico.end(), entries.begin(), entries.end());
    for (auto [off, size] : images) ico.insert(ico.end(), &g_file[off], &g_file[off + size]);
    std::ofstream out(argv[2], std::ios::binary);
    out.write((const char*)ico.data(), ico.size());
    return out ? 0 : 1;
}
