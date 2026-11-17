// Collects the ARB assembly programs stored in the game's .forge archives, for
// gl_arb_probe to compile (risk check 1 beyond the programs embedded in the
// executable).
//
//   forge_shaders GAME.app OUT_FILE
//
// Scans every *.forge in GAME.app/Contents/Resources for compressed sections
// (format in tests/forge: marker, header, chunk sizes, then per chunk a
// checksum and the LZO1X data or the raw bytes), decodes each with liblzo2,
// and extracts each "!!ARBvp1.0"/"!!ARBfp1.0" program up to its END. Writes the
// distinct programs to OUT_FILE, each followed by a NUL. Reads the game from
// outside the repo; the output goes to the build directory.

#include <dirent.h>
#include <lzo/lzo1x.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

const uint8_t kMarker[8] = {0x33, 0xaa, 0xfb, 0x57, 0x99, 0xfa, 0x04, 0x10};

// Decodes the section at `s` (at most `n` bytes). Returns false if it does
// not parse or decompress cleanly.
bool decode_section(const uint8_t* s, size_t n, std::vector<uint8_t>* out, size_t* used) {
    if (n < 17 || s[10] != 0) return false;  // codec 0 (LZO1X) is all AC2 uses
    uint16_t chunks;
    std::memcpy(&chunks, s + 15, 2);
    size_t data = 17 + 4 * (size_t)chunks;
    if (data > n) return false;
    out->clear();
    for (uint16_t k = 0; k < chunks; k++) {
        uint16_t u, c;
        std::memcpy(&u, s + 17 + 4 * k, 2);
        std::memcpy(&c, s + 17 + 4 * k + 2, 2);
        data += 4;  // checksum
        if (c > u || data + c > n) return false;
        const size_t at = out->size();
        out->resize(at + u);
        if (c < u) {
            lzo_uint got = u;
            if (lzo1x_decompress_safe(s + data, c, out->data() + at, &got, nullptr) != LZO_E_OK || got != u)
                return false;
        } else {
            std::memcpy(out->data() + at, s + data, u);
        }
        data += c;
    }
    *used = data;
    return true;
}

void extract(const std::vector<uint8_t>& d, std::set<std::string>* programs) {
    const std::string_view v((const char*)d.data(), d.size());
    for (size_t p = v.find("!!ARB"); p != std::string_view::npos; p = v.find("!!ARB", p + 1)) {
        const std::string_view head = v.substr(p, 10);
        if (head != "!!ARBvp1.0" && head != "!!ARBfp1.0") continue;
        // The program ends at an END line.
        size_t e = p;
        for (;;) {
            e = v.find("END", e + 1);
            if (e == std::string_view::npos) break;
            const bool line_start = v[e - 1] == '\n' || v[e - 1] == '\r';
            const bool line_end = e + 3 >= v.size() || v[e + 3] == '\n' || v[e + 3] == '\r' ||
                                  v[e + 3] == '\0' || v[e + 3] == ' ';
            if (line_start && line_end) break;
        }
        if (e == std::string_view::npos) continue;
        programs->insert(std::string(v.substr(p, e + 3 - p)) + "\n");
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s GAME.app OUT_FILE\n", argv[0]);
        return 2;
    }
    if (lzo_init() != LZO_E_OK) return 2;
    const std::string dir = std::string(argv[1]) + "/Contents/Resources";
    std::vector<std::string> forges;
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d)) {
            const std::string n = e->d_name;
            if (n.size() > 6 && n.compare(n.size() - 6, 6, ".forge") == 0) forges.push_back(dir + "/" + n);
        }
        closedir(d);
    }
    std::sort(forges.begin(), forges.end());
    std::set<std::string> programs;
    size_t sections = 0, bad = 0, bytes = 0;
    for (const std::string& f : forges) {
        const int fd = open(f.c_str(), O_RDONLY);
        struct stat st;
        if (fd < 0 || fstat(fd, &st) != 0) continue;
        const size_t n = (size_t)st.st_size;
        const uint8_t* p = (const uint8_t*)mmap(nullptr, n, PROT_READ, MAP_PRIVATE, fd, 0);
        close(fd);
        if (p == MAP_FAILED) continue;
        std::vector<uint8_t> out;
        const size_t before = programs.size();
        for (size_t at = 0; at + 17 <= n;) {
            const void* m = memmem(p + at, n - at, kMarker, 8);
            if (!m) break;
            const size_t s = (const uint8_t*)m - p;
            size_t used = 0;
            if (decode_section(p + s, n - s, &out, &used)) {
                sections++;
                bytes += out.size();
                extract(out, &programs);
                at = s + used;
            } else {
                bad++;
                at = s + 1;
            }
        }
        munmap((void*)p, n);
        std::printf("%s: %zu new programs\n", f.substr(f.rfind('/') + 1).c_str(), programs.size() - before);
    }
    FILE* out = std::fopen(argv[2], "wb");
    if (!out) return 1;
    size_t vp = 0, fp = 0;
    for (const std::string& s : programs) {
        (s.compare(0, 10, "!!ARBvp1.0") == 0 ? vp : fp)++;
        std::fwrite(s.data(), 1, s.size(), out);
        std::fputc(0, out);
    }
    std::fclose(out);
    std::printf("forge_shaders: %zu archives, %zu sections (%zu MB decompressed, %zu markers not "
                "sections); %zu distinct programs (%zu vertex, %zu fragment)\n",
                forges.size(), sections, bytes >> 20, bad, programs.size(), vp, fp);
    return programs.empty() ? 1 : 0;
}
