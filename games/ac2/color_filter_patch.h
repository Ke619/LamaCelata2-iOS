// AC2's color filter, switched off.
//
// A world's default ambiance (sky, fog, light, and the color filter the game
// puts over the picture) is a WorldAmbiance record: 1687 bytes, laid out the
// same way in every world. The PC mod "AC2's True Colors" (on Nexus Mods)
// removes the filter by replacing those records, and every one of its 21 files
// differs from the game's own in the same 11 bytes: three on/off bytes that
// head groups of color values (97, 158, 183), set to 0, and two floats (1423,
// 0.15 and 0.35 in Florence's) set to 0. This does the same to the record as the
// game reads it, so nothing in the game's data changes and the setting can be
// turned off again (SCIMITAR_NO_COLOR_FILTER, color_filter.cpp).
//
// The record is recognised by its layout, not its place: 16 four-byte values
// that every world's record has at the same offsets (found by comparing the
// mod's 21 files; the values that differ from world to world are left out),
// 15 of which must be there. These 64 bytes are all that this file has of the
// game's data.
//
// The loader may already have read the first bytes of a record (a header) when it calls
// WorldAmbiance's FastLoad, so the read position can be a few bytes into the record: a
// run's log showed 1677 bytes left, 10 short of 1687. patch_at_read_position handles
// that: it tries each possible number of bytes already read (up to kMaxLead) and uses
// the anchors that are still in view.
//
// Written against the Mac release's Florence record (tests/ac2 builds its own
// look-alike); the other worlds follow from the mod's files all sharing the
// layout.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ac2 {

inline constexpr size_t kAmbianceSize = 1687;

struct Anchor {
    uint16_t offset;
    uint32_t value;  // little endian, as stored
};

inline constexpr Anchor kAnchors[] = {
    {   6, 0x21c2d472u},
    {  21, 0xacf73d89u},
    {  76, 0xc83b8907u},
    {  93, 0xa8820575u},
    { 154, 0x583f4578u},
    { 220, 0x3d7bdf26u},
    { 550, 0x7b3e4cccu},
    { 640, 0x6fb71f68u},
    { 690, 0xbe81df9fu},
    { 753, 0x45603fbeu},
    { 936, 0xccfe1f35u},
    {1352, 0x5b1df2efu},
    {1360, 0x23d70a3bu},
    {1440, 0x6b2c9f07u},
    {1453, 0x864dc837u},
    {1663, 0x2eda22fdu},
};
inline constexpr size_t kAnchorCount = sizeof(kAnchors) / sizeof(kAnchors[0]);
inline constexpr size_t kMinAnchors = 15;

// What the filter is made of.
inline constexpr uint16_t kFlagOffsets[3] = {97, 158, 183};  // 1 on, 0 off
inline constexpr uint16_t kStrengthsOffset = 1423;           // two floats
inline constexpr size_t kStrengthsSize = 8;

inline uint32_t load32(const uint8_t* p) {  // the game's data is little endian, and so are the hosts (arm64, x86-64)
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

// Whether the kAmbianceSize bytes at p are an ambiance record.
inline bool is_ambiance(const uint8_t* p) {
    size_t hits = 0;
    for (const Anchor& a : kAnchors) hits += load32(p + a.offset) == a.value;
    if (hits < kMinAnchors) return false;
    for (uint16_t o : kFlagOffsets)
        if (p[o] > 1) return false;
    return true;
}

// Switches the filter off in every ambiance record in data[0, size): returns how many.
inline size_t patch_color_filter(uint8_t* data, size_t size) {
    size_t patched = 0;
    if (size < kAmbianceSize) return patched;
    const Anchor& seed = kAnchors[0];
    const uint8_t first = static_cast<uint8_t>(seed.value & 0xff);
    size_t at = 0;  // where a record might start
    while (at + kAmbianceSize <= size) {
        const void* hit = std::memchr(data + at + seed.offset, first, size - kAmbianceSize - at + 1);
        if (!hit) break;
        const size_t start = static_cast<size_t>(static_cast<const uint8_t*>(hit) - data) - seed.offset;
        if (is_ambiance(data + start)) {
            for (uint16_t o : kFlagOffsets) data[start + o] = 0;
            std::memset(data + start + kStrengthsOffset, 0, kStrengthsSize);
            patched++;
            at = start + kAmbianceSize;
        } else {
            at = start + 1;
        }
    }
    return patched;
}

// ---- a record whose first bytes were already read ----

inline constexpr size_t kMaxLead = 32;

struct Fit {
    size_t lead = 0;  // bytes of the record that come before the read position
    size_t seen = 0;  // anchors at or after the read position
    size_t hits = 0;  // of those, the ones that match
};

// How well the bytes at the read position fit the layout, if the record began `lead` bytes earlier.
inline Fit fit_at(const uint8_t* data, size_t size, size_t lead) {
    Fit f;
    f.lead = lead;
    for (const Anchor& a : kAnchors) {
        if (a.offset < lead || a.offset - lead + 4 > size) continue;
        f.seen++;
        f.hits += load32(data + a.offset - lead) == a.value;
    }
    return f;
}

// Whether data is the rest of an ambiance record whose first `lead` bytes are gone: all of the
// rest is there, at least 12 anchors are still in view, and at most one of them is wrong (as
// for a whole record).
inline bool is_ambiance_from(const uint8_t* data, size_t size, size_t lead) {
    if (lead >= kAmbianceSize || size < kAmbianceSize - lead) return false;
    const Fit f = fit_at(data, size, lead);
    if (f.seen < kMinAnchors - 3 || f.hits + (kAnchorCount - kMinAnchors) < f.seen) return false;
    for (uint16_t o : kFlagOffsets)
        if (o >= lead && data[o - lead] > 1) return false;
    return true;
}

// Switches the filter off in the record the read position is in (or at the start of). Returns how
// many bytes of it had been read already, or -1 if there is no record there.
inline int patch_at_read_position(uint8_t* data, size_t size) {
    for (size_t lead = 0; lead <= kMaxLead; lead++) {
        if (!is_ambiance_from(data, size, lead)) continue;
        for (uint16_t o : kFlagOffsets)
            if (o >= lead) data[o - lead] = 0;
        std::memset(data + kStrengthsOffset - lead, 0, kStrengthsSize);
        return static_cast<int>(lead);
    }
    return -1;
}

// For the log, when nothing fits: the number of bytes already read that fits best.
inline Fit best_fit(const uint8_t* data, size_t size) {
    Fit best;
    double score = -1;
    for (size_t lead = 0; lead <= kMaxLead; lead++) {
        const Fit f = fit_at(data, size, lead);
        if (f.seen < 8) continue;
        const double s = static_cast<double>(f.hits) / static_cast<double>(f.seen);
        if (s > score || (s == score && f.seen > best.seen)) best = f, score = s;
    }
    return best;
}

}  // namespace ac2
