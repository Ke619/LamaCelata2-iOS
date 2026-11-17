// Reads the real DataMAC.forge with the engine's own code, twice: once with the
// recompiled functions and once with the original x86 code in Unicorn. Both
// runs must produce identical guest memory, return values and import traces,
// and the results must make sense as a forge archive.
//
//   forge_test GAME.app [--dump]
//
// Part 1 reads the archive's tables (BigFile/BigFileFat readers). Part 2 reads
// files' data with BigFile::ReadFile and decompresses it with
// CompressBinStream::DecompressBuffer (LZO), allocating through a native memory
// manager installed where the engine expects its global one.
//
// Needs the game installed locally; nothing from it is stored in the repo.

#include <fcntl.h>
#include <sys/stat.h>
#include <unicorn/unicorn.h>
#include <unistd.h>

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <lzo/lzo1x.h>

#include "scimitar/image_loader.h"
#include "scimitar/imports.h"
#include "scimitar/runtime.h"

using namespace scimitar;

namespace {

// Guest layout for the test's own objects.
constexpr uint32_t kScratch = 0x60000000, kScratchSize = 0x200000;
constexpr uint32_t kBigFile = kScratch;             // scimitar::BigFile (zeroed)
constexpr uint32_t kHeader = kBigFile + 0x8;        // BigFile::HeaderFile member
constexpr uint32_t kFile = kBigFile + 0x22c;        // scimitar::File member
constexpr uint32_t kFat = kScratch + 0x1000;        // scimitar::BigFileFat
constexpr uint32_t kDescs = kScratch + 0x2000;      // BigFileFatDescription[]
constexpr uint32_t kEntries = kScratch + 0x10000;   // FatFile[] (16 bytes each)
constexpr uint32_t kCells = kScratch + 0x1e000;     // out-parameters
constexpr uint32_t kManager = kScratch + 0x1f000;   // memory manager object
constexpr uint32_t kManagerVtbl = kScratch + 0x1f100;
constexpr uint32_t kMemIface = kScratch + 0x1f200;  // Gear::MemInterface object
constexpr uint32_t kMemIfaceVtbl = kScratch + 0x1f300;
constexpr uint32_t kManagerCell = kScratch + 0x1f400;
constexpr uint32_t kHeapPtr = kScratch + 0x1f500;   // bump pointer (per engine)
constexpr uint32_t kMaxDescs = 64, kMaxEntries = 4096;
constexpr uint32_t kHeap = 0x70000000, kHeapSize = 0x4000000;
constexpr uint32_t kStackLo = 0x50000000, kStackTop = 0x50100000;
constexpr uint32_t kSentinel = 0xdead0000;
constexpr int kGuestFd = 100;  // same descriptor number in both runs
constexpr int kVtblSlots = 32;

// The engine reaches its global memory manager through this pointer slot:
// *(*(*slot)) is the manager object.
constexpr uint32_t kManagerSlot = 0x0278f944;

const char* kSymbols[] = {
    "__ZN8scimitar7BigFile10HeaderFile4ReadEPS0_RNS_4FileE",
    "__ZN8scimitar10BigFileFat10ReadHeaderEPNS_7BigFileEm",
    "__ZN8scimitar4File4SeekExNS0_8SeekTypeE",
    "__ZN8scimitar21BigFileFatDescription4ReadEPNS_7BigFileERNS_4FileEmm",
    "__ZN8scimitar7FatFile4ReadEPNS_7BigFileERNS_4FileEm",
    "__ZN8scimitar7BigFile8ReadFileENS_12BigFileIndexERm",
    "__ZN8scimitar17CompressBinStream16DecompressBufferEPhmRS1_RmRN4Gear12MemInterfaceE",
    // Static initializer that fills CompressBinStream's codec table (LZO1X_1,
    // LZO1X_999, LZO2A, LZX); the game runs it at startup.
    "__GLOBAL__I__ZN8scimitar27CreateXDecompressionContextEv",
};
enum {
    kHeaderRead, kFatReadHeader, kFileSeek, kDescRead, kFatFileRead, kReadFile,
    kDecompressBuffer, kCodecTableInit, kNumSymbols
};

// ---- native memory manager (host functions called through vtables) ----

uint32_t heap_alloc(GuestMemory& m, uint32_t size, uint32_t align = 16) {
    if (align < 16 || (align & (align - 1))) align = 16;
    uint32_t p = m.read<uint32_t>(kHeapPtr);
    const uint32_t a = (p + align - 1) & ~(align - 1);
    if ((uint64_t)a + size > (uint64_t)kHeap + kHeapSize) fatal("test heap exhausted");
    m.write<uint32_t>(kHeapPtr, a + size);
    return a;
}

// Manager vtable +0x8: Alloc(this, kind, size, MemInterface*, 0, 0, 0, 0, 0).
void manager_alloc(Context& c) {
    const uint32_t size = arg32(c, 2);
    const uint32_t a = heap_alloc(*c.mem, size);
    import_trace().push_back("alloc(" + std::to_string(size) + ") = " + std::to_string(a));
    shim_return(c, a);
}

// Manager vtable +0xc: AllocAligned(this, kind, size, alignment, MemInterface*, 0, 0, 0, 0, 0).
void manager_alloc_aligned(Context& c) {
    const uint32_t size = arg32(c, 2), align = arg32(c, 3);
    const uint32_t a = heap_alloc(*c.mem, size, align);
    import_trace().push_back("alloc_aligned(" + std::to_string(size) + ", " +
                             std::to_string(align) + ") = " + std::to_string(a));
    shim_return(c, a);
}

// Manager vtable +0x18: Free(this, kind, pointer, 0, 0). The bump heap never
// reuses memory, so frees are only recorded.
void manager_free(Context& c) {
    import_trace().push_back("free(" + std::to_string(arg32(c, 1)) + ", " +
                             std::to_string(arg32(c, 2)) + ")");
    shim_return(c, 0);
}

// Any other virtual call is reported with its object and slot.
template <int Object, int Slot>
void trap(Context& c) {
    fatal("unexpected virtual call: %s vtable slot %d (+0x%x) from %08x, args %08x %08x %08x "
          "%08x %08x",
          Object == 0 ? "memory manager" : "MemInterface", Slot, Slot * 4,
          c.mem->read<uint32_t>(c.esp), arg32(c, 0), arg32(c, 1), arg32(c, 2), arg32(c, 3),
          arg32(c, 4));
}

template <int Object, int... Slots>
void register_traps(uint32_t* out, std::integer_sequence<int, Slots...>) {
    ((out[Slots] = register_host_function(trap<Object, Slots>, "trap")), ...);
}

struct HostVtables {
    uint32_t manager[kVtblSlots], mem_iface[kVtblSlots];
    HostVtables() {
        register_traps<0>(manager, std::make_integer_sequence<int, kVtblSlots>());
        register_traps<1>(mem_iface, std::make_integer_sequence<int, kVtblSlots>());
        manager[2] = register_host_function(manager_alloc, "manager_alloc");
        manager[3] = register_host_function(manager_alloc_aligned, "manager_alloc_aligned");
        manager[6] = register_host_function(manager_free, "manager_free");
    }
};
const HostVtables& host_vtables() {
    static HostVtables v;
    return v;
}

// ---- engines ----

class Engine {
public:
    explicit Engine(const std::string& binary) {
        image_ = load_image(mem_, binary);
        mem_.commit(kScratch, kScratchSize);
        mem_.commit(kStackLo, kStackTop - kStackLo);
        mem_.commit(kHeap, kHeapSize);
        mem_.commit(kHostFnBase, kHostFnSpan);
        std::memset(mem_.ptr(kHostFnBase), 0xc3, kHostFnSpan);  // ret, never executed
        for (int i = 0; i < kNumSymbols; i++) {
            fn_[i] = image_.image->symbol(kSymbols[i]);
            if (!fn_[i]) fatal("symbol %s not found", kSymbols[i]);
        }
        install_memory_manager();
    }
    virtual ~Engine() = default;
    virtual const char* name() const = 0;

    // Calls game function `which` (cdecl) and returns eax.
    uint32_t call(int which, std::vector<uint32_t> args) {
        uint32_t esp = kStackTop - 0x1000;
        mem_.write<uint32_t>(esp, kSentinel);
        for (size_t i = 0; i < args.size(); i++) mem_.write<uint32_t>(esp + 4 + 4 * i, args[i]);
        return run(fn_[which], esp);
    }

    GuestMemory& mem() { return mem_; }
    const recomp::Image& image() const { return *image_.image; }

protected:
    virtual uint32_t run(uint32_t fn, uint32_t esp) = 0;

    void install_memory_manager() {
        const HostVtables& v = host_vtables();
        for (int i = 0; i < kVtblSlots; i++) {
            mem_.write<uint32_t>(kManagerVtbl + 4 * i, v.manager[i]);
            mem_.write<uint32_t>(kMemIfaceVtbl + 4 * i, v.mem_iface[i]);
        }
        mem_.write<uint32_t>(kManager, kManagerVtbl);
        mem_.write<uint32_t>(kMemIface, kMemIfaceVtbl);
        // slot -> global -> cell -> manager
        const uint32_t global = mem_.read<uint32_t>(kManagerSlot);
        mem_.write<uint32_t>(global, kManagerCell);
        mem_.write<uint32_t>(kManagerCell, kManager);
        mem_.write<uint32_t>(kHeapPtr, kHeap);
    }

    static constexpr uint32_t kEbx = 0x11111111, kEsi = 0x22222222, kEdi = 0x33333333,
                              kEbp = 0x44444444;
    GuestMemory mem_;
    LoadedImage image_;
    uint32_t fn_[kNumSymbols] = {};
};

class Recompiled : public Engine {
public:
    using Engine::Engine;
    const char* name() const override { return "recompiled"; }

protected:
    uint32_t run(uint32_t fn, uint32_t esp) override {
        const FuncEntry* f = find_function(fn);
        if (!f) fatal("%08x was not recompiled", fn);
        Context c;
        c.mem = &mem_;
        c.esp = esp;
        c.ebx = kEbx, c.esi = kEsi, c.edi = kEdi, c.ebp = kEbp;
        f->fn(c);
        if (c.esp != esp + 4 || c.ebx != kEbx || c.esi != kEsi || c.edi != kEdi || c.ebp != kEbp)
            fatal("%s: stack or callee-saved registers not restored", f->name);
        return c.eax;
    }
};

class Emulated : public Engine {
public:
    explicit Emulated(const std::string& binary) : Engine(binary) {
        check(uc_open(UC_ARCH_X86, UC_MODE_32, &uc_), "open");
        // Share guest memory with Unicorn: map every committed range in place.
        std::vector<std::pair<uint64_t, uint64_t>> r;
        for (auto [a, n] : mem_.committed()) r.emplace_back(a, a + n);
        std::sort(r.begin(), r.end());
        std::vector<std::pair<uint64_t, uint64_t>> merged;
        for (auto x : r) {
            if (!merged.empty() && x.first <= merged.back().second)
                merged.back().second = std::max(merged.back().second, x.second);
            else merged.push_back(x);
        }
        for (auto [a, e] : merged)
            check(uc_mem_map_ptr(uc_, a, e - a, UC_PROT_ALL, mem_.ptr((uint32_t)a)), "map");
        // Import stubs and host functions run the same native code as the
        // recompiled build.
        uc_hook h;
        for (const auto& sec : image().sections())
            if (sec.type() == 8)
                check(uc_hook_add(uc_, &h, UC_HOOK_CODE, (void*)&on_stub, this, sec.addr,
                                  sec.addr + sec.size - 1),
                      "hook stubs");
        check(uc_hook_add(uc_, &h, UC_HOOK_CODE, (void*)&on_host_fn, this, kHostFnBase,
                          kHostFnBase + kHostFnSpan - 1),
              "hook host functions");
        check(uc_hook_add(uc_, &h, UC_HOOK_MEM_UNMAPPED, (void*)&on_unmapped, this, 1, 0),
              "hook unmapped");
        check(uc_hook_add(uc_, &h, UC_HOOK_BLOCK, (void*)&on_block, this, 1, 0), "hook blocks");
    }
    ~Emulated() override { uc_close(uc_); }
    const char* name() const override { return "emulated"; }

protected:
    uint32_t run(uint32_t fn, uint32_t esp) override {
        set(UC_X86_REG_ESP, esp);
        set(UC_X86_REG_EBX, kEbx), set(UC_X86_REG_ESI, kEsi), set(UC_X86_REG_EDI, kEdi);
        set(UC_X86_REG_EBP, kEbp), set(UC_X86_REG_EFLAGS, 0x202);
        check(uc_emu_start(uc_, fn, kSentinel, 0, 0), "run");
        if (get(UC_X86_REG_EIP) != kSentinel)
            fatal("emulation stopped at %08x", get(UC_X86_REG_EIP));
        return get(UC_X86_REG_EAX);
    }

private:
    static void check(uc_err e, const char* what) {
        if (e != UC_ERR_OK) fatal("unicorn %s: %s", what, uc_strerror(e));
    }
    uint32_t get(int r) {
        uint32_t v = 0;
        uc_reg_read(uc_, r, &v);
        return v;
    }
    void set(int r, uint32_t v) { uc_reg_write(uc_, r, &v); }

    // Runs a native function that follows the shim convention, then returns
    // to the guest caller.
    void run_native(GuestFn fn) {
        Context c;
        c.mem = &mem_;
        c.eax = get(UC_X86_REG_EAX), c.ecx = get(UC_X86_REG_ECX);
        c.edx = get(UC_X86_REG_EDX), c.esp = get(UC_X86_REG_ESP);
        const uint32_t ret = mem_.read<uint32_t>(c.esp);
        fn(c);
        set(UC_X86_REG_EAX, c.eax), set(UC_X86_REG_ECX, c.ecx);
        set(UC_X86_REG_EDX, c.edx), set(UC_X86_REG_ESP, c.esp);
        set(UC_X86_REG_EIP, ret);
    }

    static void on_stub(uc_engine*, uint64_t addr, uint32_t, void* user) {
        auto* self = static_cast<Emulated*>(user);
        const auto& stubs = self->image().stubs();
        auto it = stubs.find((uint32_t)addr);
        if (it == stubs.end()) return;  // not a stub boundary
        if (it->second.internal) {      // weak symbol bound to the image itself
            self->set(UC_X86_REG_EIP, it->second.internal);
            return;
        }
        const ImportShim* shim = find_import_shim(it->second.name);
        if (!shim) fatal("no shim for import %s", it->second.name.c_str());
        self->run_native(shim->fn);
    }

    // Last basic blocks executed, for diagnosing wild jumps.
    uint32_t recent_[16] = {};
    unsigned recent_pos_ = 0;
    static void on_block(uc_engine*, uint64_t addr, uint32_t, void* user) {
        auto* self = static_cast<Emulated*>(user);
        self->recent_[self->recent_pos_++ % 16] = (uint32_t)addr;
    }

    static bool on_unmapped(uc_engine* uc, uc_mem_type type, uint64_t addr, int size, int64_t,
                            void* user) {
        auto* self = static_cast<Emulated*>(user);
        std::fprintf(stderr, "recent blocks:");
        for (unsigned k = 0; k < 16; k++)
            std::fprintf(stderr, " %08x", self->recent_[(self->recent_pos_ + k) % 16]);
        std::fprintf(stderr, "\n");
        uint32_t eip = 0, esp = 0;
        uc_reg_read(uc, UC_X86_REG_EIP, &eip);
        uc_reg_read(uc, UC_X86_REG_ESP, &esp);
        std::fprintf(stderr, "unmapped %s of %d bytes at %08x, eip %08x, esp %08x\n",
                     type == UC_MEM_READ_UNMAPPED ? "read" : type == UC_MEM_WRITE_UNMAPPED
                                                                 ? "write" : "fetch",
                     size, (uint32_t)addr, eip, esp);
        return false;
    }

    static void on_host_fn(uc_engine*, uint64_t addr, uint32_t, void* user) {
        const FuncEntry* f = find_function((uint32_t)addr);
        if (!f) fatal("call to unregistered host function address %08x", (uint32_t)addr);
        static_cast<Emulated*>(user)->run_native(f->fn);
    }

    uc_engine* uc_ = nullptr;
};

// ---- the scenario ----

struct SectionResult {
    uint32_t in_ptr = 0, in_size = 0, declared = 0;
    uint32_t ret = 0, out_ptr = 0, out_size = 0;
    uint64_t out_fnv = 0;
};

struct FileResult {
    uint32_t index = 0, id = 0, stored_size = 0;
    uint32_t read_ret = 0, read_size = 0;
    std::vector<SectionResult> sections;
};

struct Outcome {
    std::vector<uint32_t> rets;
    std::vector<uint8_t> scratch, heap;
    std::vector<std::string> trace;
    uint32_t descs = 0, entries = 0;
    std::vector<FileResult> files;
};

uint64_t rd64(GuestMemory& m, uint32_t a) { return m.read<uint64_t>(a); }

uint64_t fnv64(const uint8_t* p, size_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

// A file's data, as stored: a leading u32 (0 in AC2) followed by one or more
// sections. Each section is [marker u64][u16 version][u8 codec][u16][u16]
// [u16 chunks][chunks x (u16 decompressed, u16 stored)] and then, per chunk,
// [u32 checksum][stored bytes]; a chunk is LZO1X-compressed when stored <
// decompressed. CompressBinStream decodes one section per stream.
struct Section {
    size_t offset = 0, length = 0;
    uint32_t decompressed = 0;
};

bool parse_sections(const uint8_t* d, size_t n, std::vector<Section>* out) {
    static const uint8_t kMarker[8] = {0x33, 0xaa, 0xfb, 0x57, 0x99, 0xfa, 0x04, 0x10};
    out->clear();
    size_t pos = 4;
    while (pos < n) {
        if (pos + 17 > n || std::memcmp(d + pos, kMarker, 8) != 0) return false;
        if (d[pos + 10] != 0) return false;  // codec 0 (LZO1X) is all AC2 uses
        uint16_t chunks;
        std::memcpy(&chunks, d + pos + 15, 2);
        Section sec;
        sec.offset = pos;
        size_t data = pos + 17 + 4 * (size_t)chunks;
        for (uint16_t k = 0; k < chunks; k++) {
            uint16_t u, c;
            std::memcpy(&u, d + pos + 17 + 4 * k, 2);
            std::memcpy(&c, d + pos + 17 + 4 * k + 2, 2);
            if (c > u) return false;
            data += 4 + c;  // checksum + stored bytes
            sec.decompressed += u;
        }
        if (data > n) return false;
        sec.length = data - pos;
        out->push_back(sec);
        pos = data;
    }
    return pos == n && !out->empty();
}

// Independent liblzo2 decode of one section.
bool decode_section(const uint8_t* s, size_t len, std::vector<uint8_t>* out) {
    uint16_t chunks;
    std::memcpy(&chunks, s + 15, 2);
    size_t data = 17 + 4 * (size_t)chunks;
    out->clear();
    for (uint16_t k = 0; k < chunks; k++) {
        uint16_t u, c;
        std::memcpy(&u, s + 17 + 4 * k, 2);
        std::memcpy(&c, s + 17 + 4 * k + 2, 2);
        data += 4;
        if (data + c > len) return false;
        const size_t at = out->size();
        out->resize(at + u);
        if (c < u) {
            lzo_uint got = u;
            if (lzo1x_decompress_safe(s + data, c, out->data() + at, &got, nullptr) != LZO_E_OK ||
                got != u)
                return false;
        } else {
            std::memcpy(out->data() + at, s + data, u);
        }
        data += c;
    }
    return data == len;
}

// Picks which entries to decompress: the smallest and the largest compressed
// entries up to 2 MB, judged from the raw archive (independent of the engine).
std::vector<uint32_t> pick_files(const std::string& forge, GuestMemory& m, uint32_t n) {
    FILE* f = std::fopen(forge.c_str(), "rb");
    std::vector<std::pair<uint32_t, uint32_t>> compressed;  // (size, index)
    for (uint32_t i = 0; i < n; i++) {
        const uint64_t off = rd64(m, kEntries + i * 0x10);
        const uint32_t size = m.read<uint32_t>(kEntries + i * 0x10 + 12);
        uint8_t head[12];
        fseeko(f, (off_t)off + 0x1b8, SEEK_SET);
        if (size > 64 && size <= 2u << 20 && std::fread(head, 1, 12, f) == 12 &&
            std::memcmp(head + 4, "\x33\xaa\xfb\x57\x99\xfa\x04\x10", 8) == 0)
            compressed.emplace_back(size, i);
    }
    std::fclose(f);
    std::sort(compressed.begin(), compressed.end());
    if (compressed.empty()) fatal("no compressed entries found");
    return {compressed.front().second, compressed.back().second};
}

Outcome run_scenario(Engine& e, const std::string& forge, bool dump) {
    const int fd = open(forge.c_str(), O_RDONLY);
    if (fd < 0 || dup2(fd, kGuestFd) < 0) fatal("cannot open %s", forge.c_str());
    close(fd);
    import_trace().clear();
    set_import_trace(true);

    GuestMemory& m = e.mem();
    Outcome o;
    m.write<uint32_t>(kFile, kGuestFd);
    e.call(kCodecTableInit, {});  // as at game startup

    // Part 1: the archive's tables, in the engine's own order
    // (BigFile::ReadAllFats / BigFileFat::ReadAllFats).
    o.rets.push_back(e.call(kHeaderRead, {kHeader, kBigFile, kFile}));
    const uint32_t version = m.read<uint32_t>(kBigFile + 0x14);
    m.write<uint32_t>(kFat + 0x28, kFile);
    m.write<uint64_t>(kFat + 0x2c, rd64(m, kBigFile + 0x18));
    o.rets.push_back(e.call(kFatReadHeader, {kFat, kBigFile, version}));

    const uint32_t count = std::min(m.read<uint32_t>(kFat + 0x1c), kMaxDescs);
    const uint32_t per_desc = m.read<uint32_t>(kFat + 0x18);
    const uint64_t first = rd64(m, kFat + 0x20);
    o.rets.push_back(e.call(kFileSeek, {kFile, (uint32_t)first, (uint32_t)(first >> 32), 2}));
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t d = kDescs + i * 0x30;
        o.rets.push_back(e.call(kDescRead, {d, kBigFile, kFile, version, per_desc}));
        const uint64_t next = rd64(m, d + 0x20);
        if (next != ~0ull)
            o.rets.push_back(e.call(kFileSeek, {kFile, (uint32_t)next, (uint32_t)(next >> 32), 2}));
    }
    o.descs = count;

    // Entries of the first table: count at +0x0, table offset at +0x8.
    const uint32_t n = std::min(m.read<uint32_t>(kDescs), kMaxEntries);
    const uint64_t table = rd64(m, kDescs + 0x8);
    o.rets.push_back(e.call(kFileSeek, {kFile, (uint32_t)table, (uint32_t)(table >> 32), 2}));
    for (uint32_t i = 0; i < n; i++)
        o.rets.push_back(e.call(kFatFileRead, {kEntries + i * 0x10, kBigFile, kFile, version}));
    o.entries = n;

    // Part 2: read files' data and decompress it. BigFile::ReadFile finds the
    // entry through BigFile+0x28 -> table object, whose +0x40 is the entries.
    m.write<uint32_t>(kBigFile + 0x28, kFat);
    m.write<uint32_t>(kFat + 0x40, kEntries);
    for (uint32_t index : pick_files(forge, m, n)) {
        FileResult r;
        r.index = index;
        r.id = m.read<uint32_t>(kEntries + index * 0x10 + 8);
        r.stored_size = m.read<uint32_t>(kEntries + index * 0x10 + 12);
        const uint32_t size_cell = kCells, out_cell = kCells + 4, out_size_cell = kCells + 8;
        m.write<uint32_t>(size_cell, 0);
        r.read_ret = e.call(kReadFile, {kBigFile, index, size_cell});
        r.read_size = m.read<uint32_t>(size_cell);
        o.rets.insert(o.rets.end(), {r.read_ret, r.read_size});

        // Each section goes to DecompressBuffer in its own buffer, prefixed with
        // its decompressed size (the format DecompressBuffer reads).
        std::vector<Section> secs;
        if (!parse_sections(m.ptr(r.read_ret), r.read_size, &secs))
            fatal("entry %u: data is not a sequence of compressed sections", index);
        for (const Section& sec : secs) {
            SectionResult sr;
            sr.declared = sec.decompressed;
            sr.in_size = 4 + (uint32_t)sec.length;
            sr.in_ptr = heap_alloc(m, sr.in_size);
            m.write<uint32_t>(sr.in_ptr, sec.decompressed);
            std::memcpy(m.ptr(sr.in_ptr + 4), m.ptr(r.read_ret + (uint32_t)sec.offset), sec.length);
            m.write<uint32_t>(out_cell, 0);
            m.write<uint32_t>(out_size_cell, 0);
            sr.ret = e.call(kDecompressBuffer,
                            {sr.in_ptr, sr.in_size, out_cell, out_size_cell, kMemIface});
            sr.out_ptr = m.read<uint32_t>(out_cell);
            sr.out_size = m.read<uint32_t>(out_size_cell);
            sr.out_fnv = sr.out_ptr ? fnv64(m.ptr(sr.out_ptr), sr.out_size) : 0;
            r.sections.push_back(sr);
            o.rets.insert(o.rets.end(), {sr.ret, sr.out_ptr, sr.out_size});
        }
        o.files.push_back(r);
    }

    set_import_trace(false);
    o.trace = import_trace();
    close(kGuestFd);
    o.scratch.assign(m.ptr(kScratch), m.ptr(kScratch) + kScratchSize);
    const uint32_t heap_end = m.read<uint32_t>(kHeapPtr);
    o.heap.assign(m.ptr(kHeap), m.ptr(heap_end));

    if (dump) {
        std::printf("[%s] header: magic '%.8s' version %u, table header at 0x%" PRIx64 "\n",
                    e.name(), (const char*)m.ptr(kHeader), version, rd64(m, kBigFile + 0x18));
        for (const FileResult& r : o.files) {
            std::printf("[%s] entry %u id 0x%x: read %u bytes at %08x\n", e.name(), r.index, r.id,
                        r.read_size, r.read_ret);
            for (const SectionResult& sr : r.sections)
                std::printf("[%s]   section: %u bytes in -> %u bytes out at %08x (consumed %u)\n",
                            e.name(), sr.in_size, sr.out_size, sr.out_ptr, sr.ret);
        }
    }
    return o;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s GAME.app [--dump]\n", argv[0]);
        return 2;
    }
    const std::string app = argv[1];
    const bool dump = argc > 2 && std::string(argv[2]) == "--dump";
    const std::string binary = app + "/Contents/MacOS/scimitar_sf";
    const std::string forge = app + "/Contents/Resources/DataMAC.forge";

    host_vtables();  // register host functions before any engine is built
    // --only-emulated runs just the original code (useful when the recompiled
    // run crashes and the two need to be told apart).
    if (argc > 2 && std::string(argv[argc - 1]) == "--only-emulated") {
        Emulated emu(binary);
        run_scenario(emu, forge, true);
        return 0;
    }
    Recompiled rec(binary);
    const Outcome a = run_scenario(rec, forge, dump);
    Emulated emu(binary);
    const Outcome b = run_scenario(emu, forge, dump);

    int failures = 0;
    auto expect = [&](bool ok, const char* what) {
        std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
        failures += !ok;
    };
    expect(a.rets == b.rets, "return values identical (recompiled vs emulated)");
    expect(a.trace == b.trace, "read/lseek/alloc traces identical");
    expect(a.scratch == b.scratch, "all guest objects byte-identical");
    expect(a.heap == b.heap, "heap (file data and decompressed output) byte-identical");
    std::printf("     %zu calls, %zu traced native calls, %u descriptions, %u entries, "
                "%zu heap bytes\n",
                a.rets.size(), a.trace.size(), a.descs, a.entries, a.heap.size());
    for (size_t i = 0; i < std::min(a.trace.size(), b.trace.size()); i++)
        if (a.trace[i] != b.trace[i]) {
            std::printf("     first trace difference at %zu: '%s' vs '%s'\n", i,
                        a.trace[i].c_str(), b.trace[i].c_str());
            break;
        }

    // Independent checks against the raw archive.
    auto at = [&](uint32_t guest) { return a.scratch.data() + (guest - kScratch); };
    uint32_t version;
    std::memcpy(&version, at(kBigFile + 0x14), 4);
    expect(std::memcmp(at(kHeader), "scimitar", 9) == 0 && version == 25,
           "header: magic 'scimitar', version 25");
    expect(a.descs >= 1 && a.entries >= 1, "at least one file table with entries");
    FILE* f = std::fopen(forge.c_str(), "rb");
    struct stat st;
    stat(forge.c_str(), &st);
    bool inside = true, filedata = true, unique = true;
    std::vector<uint32_t> ids;
    for (uint32_t i = 0; i < a.entries; i++) {
        uint64_t off;
        uint32_t id, size;
        std::memcpy(&off, at(kEntries + i * 0x10), 8);
        std::memcpy(&id, at(kEntries + i * 0x10 + 8), 4);
        std::memcpy(&size, at(kEntries + i * 0x10 + 12), 4);
        inside &= off + 0x1b8 + size <= (uint64_t)st.st_size;
        char tag[8] = {};
        fseeko(f, (off_t)off, SEEK_SET);
        filedata &= std::fread(tag, 1, 8, f) == 8 && std::memcmp(tag, "FILEDATA", 8) == 0;
        for (uint32_t prev : ids) unique &= prev != id;
        ids.push_back(id);
    }
    expect(inside, "every entry (440-byte FILEDATA header + data) lies inside the archive");
    expect(filedata, "every entry starts with a FILEDATA record");
    expect(unique, "entry ids are unique");

    bool read_ok = true, decomp_ok = true, matches_lzo = true;
    size_t sections = 0, out_bytes = 0;
    for (const FileResult& r : a.files) {
        // ReadFile must return exactly the stored bytes after the 440-byte header.
        uint64_t off;
        std::memcpy(&off, at(kEntries + r.index * 0x10), 8);
        std::vector<uint8_t> raw(r.stored_size);
        fseeko(f, (off_t)off + 0x1b8, SEEK_SET);
        const bool got = std::fread(raw.data(), 1, raw.size(), f) == raw.size();
        read_ok &= got && r.read_size == r.stored_size &&
                   std::memcmp(a.heap.data() + (r.read_ret - kHeap), raw.data(), raw.size()) == 0;
        // Every section: DecompressBuffer consumed all of its input and produced
        // exactly what an independent liblzo2 decode of the raw bytes gives.
        std::vector<Section> secs;
        const bool parsed = got && parse_sections(raw.data(), raw.size(), &secs) &&
                            secs.size() == r.sections.size();
        decomp_ok &= parsed;
        for (size_t k = 0; parsed && k < secs.size(); k++) {
            const SectionResult& sr = r.sections[k];
            std::vector<uint8_t> expected;
            const bool ok = decode_section(raw.data() + secs[k].offset, secs[k].length, &expected);
            decomp_ok &= ok && sr.ret == sr.in_size && sr.out_ptr &&
                         sr.out_size == expected.size();
            matches_lzo &= ok && sr.out_ptr && sr.out_size == expected.size() &&
                           std::memcmp(a.heap.data() + (sr.out_ptr - kHeap), expected.data(),
                                       expected.size()) == 0;
            sections++;
            out_bytes += sr.out_size;
        }
        std::printf("     entry %u (id 0x%x): %u stored bytes, %zu sections ->", r.index, r.id,
                    r.stored_size, r.sections.size());
        for (const SectionResult& sr : r.sections) std::printf(" %u", sr.out_size);
        std::printf(" bytes\n");
    }
    std::fclose(f);
    expect(!a.files.empty() && read_ok, "ReadFile returned exactly the stored bytes");
    expect(decomp_ok, "DecompressBuffer consumed each section and produced its full size");
    expect(matches_lzo, "decompressed bytes identical to an independent liblzo2 decode");
    std::printf("     %zu sections, %zu decompressed bytes checked\n", sections, out_bytes);
    return failures ? 1 : 0;
}
