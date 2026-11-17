// The color filter patch (games/ac2/color_filter_patch.h) on look-alike records
// built here from the fingerprint and filler (no game data).
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "color_filter_patch.h"

using namespace ac2;

static int g_failed = 0;
#define EXPECT(cond)                                                    \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAILED line %d: %s\n", __LINE__, #cond);       \
            g_failed++;                                                 \
        }                                                               \
    } while (0)

// A record like the game's: the fingerprint in place, the filter on, the rest from `seed`.
static std::vector<uint8_t> record(unsigned seed) {
    std::srand(seed);
    std::vector<uint8_t> r(kAmbianceSize);
    for (auto& b : r) b = (uint8_t)std::rand();
    for (const Anchor& a : kAnchors) std::memcpy(&r[a.offset], &a.value, 4);
    for (uint16_t o : kFlagOffsets) r[o] = 1;
    const float strengths[2] = {0.15f, 0.35f};
    std::memcpy(&r[kStrengthsOffset], strengths, 8);
    return r;
}

static bool off(const std::vector<uint8_t>& d, size_t at) {
    for (uint16_t o : kFlagOffsets)
        if (d[at + o] != 0) return false;
    for (size_t k = 0; k < kStrengthsSize; k++)
        if (d[at + kStrengthsOffset + k] != 0) return false;
    return true;
}

int main() {
    // 1. A record in the middle of other data: patched, and nothing else touched.
    {
        std::vector<uint8_t> d(40000);
        std::srand(1);
        for (auto& b : d) b = (uint8_t)std::rand();
        const std::vector<uint8_t> r = record(5);
        std::copy(r.begin(), r.end(), d.begin() + 12345);
        const std::vector<uint8_t> before = d;
        EXPECT(patch_color_filter(d.data(), d.size()) == 1);
        EXPECT(off(d, 12345));
        size_t changed = 0;
        for (size_t i = 0; i < d.size(); i++) changed += d[i] != before[i];
        EXPECT(changed <= 3 + 8);  // the flags and the floats (those already 0 do not count)
        EXPECT(d[12345 + 97] == 0 && before[12345 + 97] == 1);
        // twice is the same as once
        const std::vector<uint8_t> once = d;
        EXPECT(patch_color_filter(d.data(), d.size()) == 1);
        EXPECT(d == once);
    }
    // 2. At the very start and the very end of a buffer, and two of them.
    {
        std::vector<uint8_t> d;
        for (unsigned k = 0; k < 2; k++) {
            const std::vector<uint8_t> r = record(10 + k);
            d.insert(d.end(), r.begin(), r.end());
        }
        EXPECT(patch_color_filter(d.data(), d.size()) == 2);
        EXPECT(off(d, 0) && off(d, kAmbianceSize));
    }
    // 3. Not a record: the fingerprint short by two (15 of 16 is enough, 14 is not).
    {
        std::vector<uint8_t> d = record(3);
        d[kAnchors[5].offset] ^= 0xff;
        EXPECT(patch_color_filter(d.data(), d.size()) == 1);  // 15 of 16
        d = record(3);
        d[kAnchors[5].offset] ^= 0xff;
        d[kAnchors[9].offset] ^= 0xff;
        EXPECT(patch_color_filter(d.data(), d.size()) == 0);  // 14 of 16
        EXPECT(d[kFlagOffsets[0]] == 1);                       // untouched
    }
    // 4. A flag that is not 0 or 1: not the layout.
    {
        std::vector<uint8_t> d = record(4);
        d[kFlagOffsets[1]] = 7;
        EXPECT(patch_color_filter(d.data(), d.size()) == 0);
    }
    // 5. Cut short: nothing patched and nothing read past the end.
    {
        std::vector<uint8_t> d = record(6);
        d.resize(kAmbianceSize - 1);
        EXPECT(patch_color_filter(d.data(), d.size()) == 0);
        EXPECT(patch_color_filter(d.data(), 0) == 0);
    }
    // 6. Filler only (many bytes that match the seed's first byte): nothing found.
    {
        std::vector<uint8_t> d(100000, (uint8_t)(kAnchors[0].value & 0xff));
        EXPECT(patch_color_filter(d.data(), d.size()) == 0);
    }
    // 7. The read position a few bytes into the record (a run's log showed 1677 of 1687 bytes left).
    {
        for (size_t lead : {0u, 1u, 10u, 32u}) {
            const std::vector<uint8_t> r = record(20 + (unsigned)lead);
            std::vector<uint8_t> d(r.begin() + (std::ptrdiff_t)lead, r.end());
            const std::vector<uint8_t> before = d;
            EXPECT(patch_at_read_position(d.data(), d.size()) == (int)lead);
            for (uint16_t o : kFlagOffsets) EXPECT(d[o - lead] == 0 && before[o - lead] == 1);
            for (size_t k = 0; k < kStrengthsSize; k++) EXPECT(d[kStrengthsOffset - lead + k] == 0);
            size_t changed = 0;
            for (size_t k = 0; k < d.size(); k++) changed += d[k] != before[k];
            EXPECT(changed <= 3 + 8);
        }
        // the record from byte `lead` on, `cut` bytes short at the end
        const auto tail = [](unsigned seed, size_t lead, size_t cut = 0) {
            const std::vector<uint8_t> r = record(seed);
            return std::vector<uint8_t>(r.begin() + lead, r.end() - cut);
        };
        // with other bytes after it (the next record of the stream), and one anchor wrong
        std::vector<uint8_t> d = tail(30, 10);
        d.resize(d.size() + 500, 0x5a);
        d[kAnchors[7].offset - 10] ^= 0xff;
        EXPECT(patch_at_read_position(d.data(), d.size()) == 10);
        // too many bytes gone, too few left, a bad flag, plain noise: nothing
        d = tail(31, 33);
        EXPECT(patch_at_read_position(d.data(), d.size()) == -1);
        d = tail(32, 10, 1);
        EXPECT(patch_at_read_position(d.data(), d.size()) == -1);
        d = tail(33, 10);
        d[kFlagOffsets[2] - 10] = 9;
        EXPECT(patch_at_read_position(d.data(), d.size()) == -1);
        std::srand(7);
        d.assign(4000, 0);
        for (auto& b : d) b = (uint8_t)std::rand();
        EXPECT(patch_at_read_position(d.data(), d.size()) == -1);
        EXPECT(patch_at_read_position(d.data(), 0) == -1);
        // the closest fit names the right number of bytes
        d = tail(34, 10);
        d[kFlagOffsets[0] - 10] = 9;
        const Fit f = best_fit(d.data(), d.size());
        EXPECT(f.lead == 10 && f.hits == f.seen && f.seen == kAnchorCount - 1);
    }
    std::printf(g_failed ? "color filter: %d check(s) failed\n" : "color filter: all checks passed\n", g_failed);
    return g_failed != 0;
}
