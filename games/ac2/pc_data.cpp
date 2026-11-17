// AC2 with the Windows release's data (docs/PC_DATA.md): the records that
// differ from the Mac's converted as they load.
//
// The PC and Mac archives hold the same objects (textures, meshes and
// animations byte for byte), with three differences the Mac engine trips over.
//
// 1. A scimitar::ShaderPermutation (and its lighting variant,
// LightingShaderPermutation) carries D3D9 shader bytecode on PC and an ARB
// assembly program on the Mac, and its serialized form (FastLoad) differs
// accordingly:
//
//   both:  ShaderPermutation: 15 flag bytes, u32; LightingShaderPermutation:
//          u32, u32
//   both:  u32 length + program (bytecode; ARB text with its NUL)
//   both:  ShaderPermutation only: u32 count + material parameters
//          (ShaderParameter: object reference, type hash, 22 bytes)
//   Mac:   u32 count + ShaderConstantInfo (object reference 0, type hash,
//          u16 register, u16 g_OGLShaderConstantNames index)
//   both:  8 bytes
//
// The PC engine builds what the Mac's ShaderConstantInfo array says from the
// bytecode's constant table (D3DXGetShaderConstantTable). Here a PC record is
// read whole, its bytecode translated to ARB (scimitar/d3d9_arb.h) and its
// constant table turned into ShaderConstantInfo entries, and the original
// FastLoad then reads the record as the Mac would have stored it, from a
// buffer of our own. Mac records go straight to the original.
//
// 2. A HavokChunk holds its Havok packfile in another array, and the
// packfile's header names MSVC's layout rules (havok_chunk below).
//
// 3. A CompiledMeshInstance has 0x20 at +0x2c where the Mac's has 2.
//
// The engine reaches these FastLoads only through member-function pointers
// and vtables (never by a direct call), so replacing their entries in the
// runtime's function table is enough.
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "game.h"
#include "scimitar/d3d9_arb.h"
#include "scimitar/guest_heap.h"
#include "scimitar/image_loader.h"
#include "scimitar/runtime.h"

namespace {

using scimitar::Context;

// FastLoadSerializer: +0x14 read position, +0x18 end of the buffered bytes;
// ReadFromStream moves what is left to the buffer's start and appends the
// stream's next bytes (up to 32 KB).
constexpr uint32_t kSerCursor = 0x14, kSerEnd = 0x18;
constexpr uint32_t kConstantInfoType = 0xf2a24f9c;  // ShaderConstantInfo's type hash
constexpr uint32_t kParameterSize = 30;             // a serialized ShaderParameter

// The replaced FastLoads (filled in by install_pc_data_support).
enum Hooked { kShaderPermutation, kLightingShaderPermutation, kHavokChunk, kCompiledMeshInstance, kHookedCount };
const char* const kHookedSymbols[kHookedCount] = {
    "__ZN8scimitar17ShaderPermutation8FastLoadERNS_18FastLoadSerializerE",
    "__ZN8scimitar25LightingShaderPermutation8FastLoadERNS_18FastLoadSerializerE",
    "__ZN8scimitar10HavokChunk8FastLoadERNS_18FastLoadSerializerE",
    "__ZN8scimitar20CompiledMeshInstance8FastLoadERNS_18FastLoadSerializerE",
};
scimitar::GuestFn g_original[kHookedCount];
// Set by the first D3D9 shader: the archives are the Windows release's.
std::atomic<bool> g_pc_data{false};
uint32_t g_read_from_stream = 0;
uint32_t g_constant_names = 0;  // char[325][128], indexed by ShaderConstantEnum
uint32_t g_constant_count = 0;

struct Reader {
    Context& c;
    uint32_t ser;
    uint32_t cursor() { return c.mem->read<uint32_t>(ser + kSerCursor); }
    uint32_t end() { return c.mem->read<uint32_t>(ser + kSerEnd); }
    // At least n bytes buffered (n small); false at the stream's end.
    bool want(uint32_t n) {
        for (int tries = 0; end() - cursor() < n; tries++) {
            const uint32_t before = end() - cursor();
            scimitar::call_guest(c, g_read_from_stream, {ser});
            if (end() - cursor() == before && tries) return false;
        }
        return true;
    }
    void read(void* dst, uint32_t n) {
        auto* out = static_cast<uint8_t*>(dst);
        while (n) {
            if (cursor() == end() && !want(1)) scimitar::fatal("pc_data: record runs past its stream");
            const uint32_t k = std::min(n, end() - cursor());
            std::memcpy(out, c.mem->ptr(cursor()), k);
            c.mem->write<uint32_t>(ser + kSerCursor, cursor() + k);
            out += k, n -= k;
        }
    }
    uint32_t u32() {
        uint32_t v;
        read(&v, 4);
        return v;
    }
};

void put32(std::vector<uint8_t>& v, uint32_t x) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&x);
    v.insert(v.end(), p, p + 4);
}

int constant_index(scimitar::GuestMemory& m, const std::string& name) {
    for (uint32_t k = 0; k < g_constant_count; k++)
        if (name == reinterpret_cast<const char*>(m.ptr(g_constant_names + 128 * k))) return (int)k;
    return -1;
}

// The float register the Mac shaders give a boolean the PC ones keep in a
// bool register (the engine sets it as a float constant).
int bool_register(const std::string& name) {
    if (name == "g_DeconstructionEnabled") return 79;
    if (name == "g_HasSun") return 77;
    return -1;
}

// Runs the original FastLoad on `record` (the Mac form of what the PC
// stream held) instead of the stream, then returns the stream to where the
// PC record ended.
void replay(Context& c, Reader& r, Hooked which, std::vector<uint8_t> record) {
    const uint32_t size = (uint32_t)record.size();
    record.resize(size + 64);  // no read reaches past the end
    const uint32_t buf = scimitar::heap::alloc(*c.mem, (uint32_t)record.size());
    std::memcpy(c.mem->ptr(buf), record.data(), record.size());
    const uint32_t cursor = r.cursor(), end = r.end();
    c.mem->write<uint32_t>(r.ser + kSerCursor, buf);
    c.mem->write<uint32_t>(r.ser + kSerEnd, buf + (uint32_t)record.size());
    g_original[which](c);
    if (c.mem->read<uint32_t>(r.ser + kSerCursor) != buf + size)
        scimitar::fatal("pc_data: %s read %d bytes of a %u-byte record", kHookedSymbols[which],
                        (int)(c.mem->read<uint32_t>(r.ser + kSerCursor) - buf), size);
    c.mem->write<uint32_t>(r.ser + kSerCursor, cursor);
    c.mem->write<uint32_t>(r.ser + kSerEnd, end);
    scimitar::heap::release(*c.mem, buf);
}

// ShaderPermutation and LightingShaderPermutation.
void shader_record(Context& c, Hooked which) {
    const uint32_t head_size = which == kShaderPermutation ? 19 : 8;
    const bool parameters = which == kShaderPermutation;
    Reader r{c, scimitar::arg32(c, 1)};
    // The head, the program's length and its first word.
    const bool pc = r.want(head_size + 8) && [&] {
        const uint32_t at = r.cursor() + head_size;
        const uint32_t len = c.mem->read<uint32_t>(at), version = c.mem->read<uint32_t>(at + 4);
        return len >= 8 && ((version >> 16) == 0xfffe || (version >> 16) == 0xffff);
    }();
    if (!pc) return g_original[which](c);
    g_pc_data = true;

    // The PC record.
    std::vector<uint8_t> head(head_size);
    r.read(head.data(), head_size);
    std::vector<uint32_t> code(r.u32() / 4);
    r.read(code.data(), (uint32_t)code.size() * 4);
    const uint32_t params = parameters ? r.u32() : 0;
    std::vector<uint8_t> param_bytes(params * kParameterSize);
    r.read(param_bytes.data(), (uint32_t)param_bytes.size());
    uint8_t tail[8];
    r.read(tail, sizeof tail);
    // Translate, and the constant table's engine constants.
    scimitar::d3d9::Program program = scimitar::d3d9::parse(code.data(), code.size());
    scimitar::d3d9::ArbOptions options;
    std::vector<std::pair<uint16_t, uint16_t>> infos;  // register, constant index
    for (const scimitar::d3d9::Constant& k : program.constants) {
        if (k.set == scimitar::d3d9::RegisterSet::Bool) {
            const int reg = bool_register(k.name);
            if (reg >= 0) options.bool_registers[k.reg] = reg;
            const int index = constant_index(*c.mem, k.name);
            if (index >= 0 && reg >= 0) infos.emplace_back((uint16_t)reg, (uint16_t)index);
            continue;
        }
        if (k.set == scimitar::d3d9::RegisterSet::Sampler && k.name.rfind("g_ProjectorCookies", 0) == 0)
            for (int u = 0; u < k.count; u++) options.shadow_samplers |= 1u << (k.reg + u);
        const int index = constant_index(*c.mem, k.name);
        if (index < 0) continue;  // a material parameter: in the record's parameters already
        // Arrays and structures: register 0, as the Mac build's tools stored
        // them (the engine knows where those go).
        const bool aggregate = k.elements > 1 || k.is_struct;
        infos.emplace_back((uint16_t)(aggregate ? 0 : k.reg), (uint16_t)index);
    }
    std::string error;
    std::string arb = scimitar::d3d9::to_arb(program, options, &error);
    if (arb.empty()) {
        static int reported = 0;
        if (reported++ < 20) game::log("pc_data: shader not translated (%s)\n", error.c_str());
        arb = scimitar::d3d9::fallback_arb(program);
    }

    // The record as the Mac stores it.
    std::vector<uint8_t> mac = head;
    put32(mac, (uint32_t)arb.size() + 1);
    mac.insert(mac.end(), arb.begin(), arb.end());
    mac.push_back(0);
    if (parameters) put32(mac, params);
    mac.insert(mac.end(), param_bytes.begin(), param_bytes.end());
    put32(mac, (uint32_t)infos.size());
    for (auto [reg, index] : infos) {
        put32(mac, 0);
        put32(mac, kConstantInfoType);
        put32(mac, reg | (uint32_t)index << 16);
    }
    mac.insert(mac.end(), tail, tail + sizeof tail);
    replay(c, r, which, std::move(mac));
}

// HavokChunk: its Havok packfile in the first of three arrays on PC, in the
// fourth of four on the Mac (the others are empty in both), with the
// packfile's layout rules saying MSVC's (no reuse of base classes' tail
// padding) where the Mac's say GCC's. The packfiles are otherwise identical
// byte for byte: their classes have no tail padding to reuse.
void havok_chunk(Context& c) {
    if (!g_pc_data) return g_original[kHavokChunk](c);
    Reader r{c, scimitar::arg32(c, 1)};
    std::vector<uint8_t> arrays[3];
    for (auto& a : arrays) {
        a.resize(r.u32());
        r.read(a.data(), (uint32_t)a.size());
    }
    const uint32_t tail = r.u32();
    std::vector<uint8_t>& packfile = arrays[0];
    if (!arrays[1].empty() || !arrays[2].empty()) game::log("pc_data: HavokChunk with more than one packfile\n");
    // hkPackfileHeader: +0x10 layout rules (pointer size, little endian,
    // reuse padding, empty base class optimization).
    if (packfile.size() > 0x13 && packfile[0x10] == 4 && packfile[0x12] == 0) packfile[0x12] = 1;
    std::vector<uint8_t> mac;
    for (int k = 0; k < 3; k++) put32(mac, 0);
    put32(mac, (uint32_t)packfile.size());
    mac.insert(mac.end(), packfile.begin(), packfile.end());
    put32(mac, tail);
    replay(c, r, kHavokChunk, std::move(mac));
}

// CompiledMeshInstance: the last u32 but three of its record, kept at +0x2c,
// is 0x20 in every PC record and 2 in the same record on the Mac (its use was
// not traced; the Mac value is what the Mac engine was built against).
void compiled_mesh_instance(Context& c) {
    const uint32_t self = scimitar::arg32(c, 0);
    g_original[kCompiledMeshInstance](c);
    if (g_pc_data && c.mem->read<uint32_t>(self + 0x2c) == 0x20) c.mem->write<uint32_t>(self + 0x2c, 2);
}

void shader_permutation(Context& c) { shader_record(c, kShaderPermutation); }
void lighting_shader_permutation(Context& c) { shader_record(c, kLightingShaderPermutation); }

}  // namespace

namespace ac2 {

void install_pc_data_support() {
    g_read_from_stream = scimitar::guest_symbol("__ZN8scimitar18FastLoadSerializer14ReadFromStreamEv");
    g_constant_names = scimitar::guest_symbol("__ZN8scimitar24g_OGLShaderConstantNamesE");
    g_constant_count = 325;
    // The runtime's table entries are writable; lookups return them by pointer.
    const scimitar::GuestFn wrappers[kHookedCount] = {shader_permutation, lighting_shader_permutation, havok_chunk,
                                                       compiled_mesh_instance};
    for (int k = 0; k < kHookedCount; k++) {
        auto* entry = const_cast<scimitar::FuncEntry*>(scimitar::find_function(scimitar::guest_symbol(kHookedSymbols[k])));
        if (!entry) scimitar::fatal("pc_data: %s is not translated", kHookedSymbols[k]);
        g_original[k] = entry->fn;
        entry->fn = wrappers[k];
    }
}

}  // namespace ac2
