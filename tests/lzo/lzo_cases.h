// Shared by the Unicorn oracle and the recompiled-function test: loads the LZO
// vectors and defines one guest memory layout so both run the function under
// identical conditions.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace lzo_cases {

// Guest layout. The fixture itself sits at its original address (see the .json).
constexpr uint32_t kSrc = 0x10000000;       // compressed input
constexpr uint32_t kDst = 0x20000000;       // output buffer
constexpr uint32_t kDstLen = 0x30000000;    // the dst_len word
constexpr uint32_t kStackLo = 0x3ff00000;   // 1 MB stack
constexpr uint32_t kStackTop = 0x40000000;
constexpr uint32_t kRegion = 0x00100000;    // size committed for src and dst
constexpr uint32_t kGuard = 64;             // bytes after dst that must stay untouched
constexpr uint8_t kGuardByte = 0xA5;
constexpr uint32_t kReturnSentinel = 0xdead0000;

// Recognisable values for callee-saved registers, checked after the call.
constexpr uint32_t kEbx = 0x11111111, kEsi = 0x22222222, kEdi = 0x33333333,
                   kEbp = 0x44444444;

struct Case {
    int id = 0;
    std::string file;
    bool valid = false;
    uint32_t dst_capacity = 0;
    int expect_ret = 0;
    uint32_t expect_out_len = 0;
    uint64_t expect_fnv = 0;
    std::vector<uint8_t> input;
};

// What one run of the function produced.
struct Result {
    int32_t ret = 0;
    uint32_t out_len = 0;
    uint64_t out_fnv = 0;
    bool guard_ok = true;
    bool regs_ok = true;  // callee-saved registers restored and esp popped correctly
};

inline uint64_t fnv64(const uint8_t* p, size_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

inline std::vector<Case> load(const std::string& dir) {
    std::ifstream in(dir + "/cases.tsv");
    if (!in) {
        std::fprintf(stderr, "cannot open %s/cases.tsv\n", dir.c_str());
        std::exit(2);
    }
    std::vector<Case> cases;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream f(line);
        Case c;
        int valid;
        std::string fnv;
        size_t input_size;
        f >> c.id >> c.file >> valid >> c.dst_capacity >> input_size >> c.expect_ret >>
            c.expect_out_len >> fnv;
        c.valid = valid != 0;
        c.expect_fnv = std::stoull(fnv, nullptr, 16);
        std::ifstream bin(dir + "/" + c.file, std::ios::binary);
        c.input.assign(std::istreambuf_iterator<char>(bin), {});
        if (c.input.size() != input_size) {
            std::fprintf(stderr, "%s: size mismatch\n", c.file.c_str());
            std::exit(2);
        }
        cases.push_back(std::move(c));
    }
    return cases;
}

inline std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "cannot open %s\n", path.c_str());
        std::exit(2);
    }
    return {std::istreambuf_iterator<char>(in), {}};
}

// Oracle results file: one line per case, "id ret out_len fnv guard_ok regs_ok".
inline void write_results(const std::string& path, const std::vector<Case>& cases,
                          const std::vector<Result>& results, const char* header) {
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) {
        std::perror(path.c_str());
        std::exit(2);
    }
    std::fprintf(f, "# %s\n# id\tret\tout_len\tout_fnv64\tguard_ok\tregs_ok\n", header);
    for (size_t i = 0; i < cases.size(); i++) {
        const Result& r = results[i];
        std::fprintf(f, "%d\t%d\t%u\t%016llx\t%d\t%d\n", cases[i].id, r.ret, r.out_len,
                     (unsigned long long)r.out_fnv, r.guard_ok, r.regs_ok);
    }
    std::fclose(f);
}

inline std::vector<Result> read_results(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        std::fprintf(stderr, "cannot open %s\n", path.c_str());
        std::exit(2);
    }
    std::vector<Result> out;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream f(line);
        int id, guard, regs;
        std::string fnv;
        Result r;
        f >> id >> r.ret >> r.out_len >> fnv >> guard >> regs;
        r.out_fnv = std::stoull(fnv, nullptr, 16);
        r.guard_ok = guard != 0;
        r.regs_ok = regs != 0;
        out.push_back(r);
    }
    return out;
}

}  // namespace lzo_cases
