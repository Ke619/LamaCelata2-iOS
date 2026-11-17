// Differential test of the translator on real game functions: each root is run
// with generated inputs twice, once recompiled and once as the original x86 in
// Unicorn, and every register and all writable guest memory must end up equal.
//
//   difftest GAME.app ROOTS_FILE [--cases N] [--seed S] [--only 0xADDR] [--verbose]
//
// ROOTS_FILE lists functions (0xADDR per line) whose whole direct-call closure
// was translated into this binary (see scimitar-analyze --difftest-candidates).
// Inputs come from the function's demangled signature where it helps: pointer
// parameters point into a scratch heap of mixed floats, pointers and small
// integers; the rest of the stack and eax/ecx/edx get the same mix.
//
// Unicorn runs first. A case is skipped when the original code faults, runs
// too long, leaves the translated closure (an indirect call to code that was
// not translated) or calls an import without a shim; otherwise the recompiled
// run must match it exactly. Needs the game installed locally.

#include <setjmp.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unicorn/unicorn.h>

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "emit.h"  // demangle
#include "flow.h"  // discover_functions
#include "scimitar/image_loader.h"
#include "scimitar/imports.h"
#include "scimitar/runtime.h"

using namespace scimitar;

namespace {

constexpr uint32_t kScratch = 0x60000000, kScratchSize = 0x100000;
constexpr uint32_t kStackLo = 0x50000000, kStackTop = 0x50100000;
constexpr uint32_t kEntryEsp = kStackTop - 0x10000;
constexpr uint32_t kSentinel = 0xdead0000;
constexpr int kArgSlots = 16;
constexpr uint64_t kMaxInsns = 2'000'000;

// ---- deterministic inputs ----

struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed) {}
    uint64_t next() {
        uint64_t z = (s += 0x9e3779b97f4a7c15ull);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }
    uint32_t below(uint32_t n) { return (uint32_t)(next() % n); }
    double unit() { return (next() >> 11) * (1.0 / 9007199254740992.0); }
};

uint32_t bits(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}

float gen_float(Rng& r) {
    const uint32_t k = r.below(100);
    if (k < 45) return (float)(r.unit() * 20 - 10);
    if (k < 65) return (float)(r.unit() * 2000 - 1000);
    if (k < 75) {
        static const float kSmall[] = {0, 1, -1, 2, 0.5f, -0.5f, 3, 10, 100, 0.25f};
        return kSmall[r.below(10)];
    }
    if (k < 87) return (float)r.unit();
    if (k < 92) return 0.0f;
    return (float)((r.unit() - 0.5) * 1e-5);
}

uint32_t gen_pointer(Rng& r) { return kScratch + (r.below(kScratchSize * 7 / 8) & ~15u); }

uint32_t gen_small(Rng& r) { return r.below(4) == 0 ? (uint32_t)r.next() : r.below(16); }

uint32_t gen_word(Rng& r) {
    const uint32_t k = r.below(100);
    if (k < 35) return bits(gen_float(r));
    if (k < 60) return gen_pointer(r);
    if (k < 78) return r.below(16);
    if (k < 90) return 0;
    return (uint32_t)r.next();
}

// Splits "ns::C::f(int, float const*) const" into parameter type strings.
std::vector<std::string> parameters(const std::string& demangled) {
    std::string s = demangled;
    for (const char* q : {" const", " volatile"})
        while (s.size() > std::strlen(q) && s.compare(s.size() - std::strlen(q), std::string::npos, q) == 0)
            s.resize(s.size() - std::strlen(q));
    if (s.empty() || s.back() != ')') return {};
    int depth = 0;
    size_t open = std::string::npos;
    for (size_t k = s.size(); k-- > 0;) {
        if (s[k] == ')') depth++;
        else if (s[k] == '(' && --depth == 0) {
            open = k;
            break;
        }
    }
    if (open == std::string::npos) return {};
    std::vector<std::string> out;
    std::string cur;
    depth = 0;
    for (size_t k = open + 1; k + 1 < s.size(); k++) {
        const char ch = s[k];
        if (ch == '<' || ch == '(' || ch == '[') depth++;
        if (ch == '>' || ch == ')' || ch == ']') depth--;
        if (ch == ',' && depth == 0) {
            out.push_back(cur);
            cur.clear();
            continue;
        }
        if (!(cur.empty() && ch == ' ')) cur += ch;
    }
    if (!cur.empty() && cur != "void") out.push_back(cur);
    return out;
}

// Stack argument words for one case.
std::vector<uint32_t> gen_args(const std::string& mangled, Rng& r) {
    std::vector<uint32_t> a;
    const std::string d = recomp::demangle(mangled);
    if (mangled.rfind("__ZN", 0) == 0 || mangled.rfind("__ZNK", 0) == 0) a.push_back(gen_pointer(r));
    for (const std::string& p : parameters(d)) {
        if (p.find('*') != std::string::npos || p.find('&') != std::string::npos) {
            a.push_back(gen_pointer(r));
        } else if (p == "float") {
            a.push_back(bits(gen_float(r)));
        } else if (p == "double") {
            const double v = gen_float(r);
            uint64_t u;
            std::memcpy(&u, &v, 8);
            a.push_back((uint32_t)u);
            a.push_back((uint32_t)(u >> 32));
        } else if (p == "long long" || p == "unsigned long long") {
            a.push_back(gen_small(r));
            a.push_back(r.below(4) == 0 ? (uint32_t)r.next() : 0);
        } else if (p == "bool") {
            a.push_back(r.below(2));
        } else if (p == "...") {
            for (int k = 0; k < 4; k++) a.push_back(gen_word(r));
        } else {
            a.push_back(gen_small(r));  // integers, enums; structs get the words after
        }
    }
    while (a.size() < kArgSlots) a.push_back(gen_word(r));
    a.resize(kArgSlots);
    return a;
}

// ---- guarded execution ----

sigjmp_buf g_jmp;
volatile sig_atomic_t g_guarded = 0;
char g_fatal_msg[1024];

void on_signal(int sig) {
    if (!g_guarded) {
        signal(sig, SIG_DFL);
        raise(sig);
        return;
    }
    siglongjmp(g_jmp, sig);
}

void on_fatal(const char* msg) {
    if (!g_guarded) return;  // fatal() prints and aborts
    std::snprintf(g_fatal_msg, sizeof g_fatal_msg, "%s", msg);
    siglongjmp(g_jmp, 1000);
}

void install_guards() {
    static std::vector<uint8_t> alt(1 << 20);
    stack_t ss{};
    ss.ss_sp = alt.data();
    ss.ss_size = alt.size();
    sigaltstack(&ss, nullptr);
    struct sigaction sa {};
    sa.sa_handler = on_signal;
    sa.sa_flags = SA_ONSTACK | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    for (int s : {SIGSEGV, SIGBUS, SIGALRM, SIGFPE, SIGILL}) sigaction(s, &sa, nullptr);
    set_fatal_hook(on_fatal);
}

void arm_watchdog(bool on) {
    itimerval t{};
    if (on) t.it_value.tv_sec = 3;
    setitimer(ITIMER_REAL, &t, nullptr);
}

// ---- machine state ----

struct State {
    uint32_t gpr[8] = {};  // eax ecx edx ebx esp ebp esi edi
    Context::Xmm xmm[8] = {};
    X87 fpu;  // values of empty registers are not compared
};

const char* kGprNames[8] = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"};

class Engine {
public:
    explicit Engine(const std::string& binary) {
        image_ = load_image(mem_, binary);
        recomp::discover_functions(*image_.image);  // the same function set as scimitar-recomp
        mem_.commit(kScratch, kScratchSize);
        mem_.commit(kStackLo, kStackTop - kStackLo);
        for (const auto& s : image().segments())
            if (s.name == "__TEXT") text_ = {s.vmaddr, s.vmsize};
        // Everything committed above the code: __TEXT (and __PAGEZERO below it,
        // which shares its first host page) is read-only.
        const uint64_t t1 = (uint64_t)text_.first + text_.second;
        for (auto [a, n] : mem_.committed()) {
            const uint64_t lo = std::max<uint64_t>(a, t1), hi = (uint64_t)a + n;
            if (hi > lo) writable_.push_back({(uint32_t)lo, (uint32_t)(hi - lo)});
        }
        for (auto [a, n] : writable_) pristine_.emplace_back(mem_.ptr(a), mem_.ptr(a) + n);
        // Code is read-only, as in a real process: a wild write into it faults.
        if (mprotect(mem_.ptr(0), t1, PROT_READ) != 0) fatal("mprotect __TEXT");
    }
    virtual ~Engine() = default;

    void reset(uint64_t seed) {
        for (size_t k = 0; k < writable_.size(); k++)
            std::memcpy(mem_.ptr(writable_[k].first), pristine_[k].data(), pristine_[k].size());
        Rng r(seed);
        for (uint32_t a = kScratch; a < kScratch + kScratchSize; a += 4)
            mem_.write<uint32_t>(a, gen_word(r));
    }

    GuestMemory& mem() { return mem_; }
    const recomp::Image& image() const { return *image_.image; }
    const std::vector<std::pair<uint32_t, uint32_t>>& writable() const { return writable_; }

protected:
    GuestMemory mem_;
    LoadedImage image_;
    std::pair<uint32_t, uint32_t> text_{0, 0};
    std::vector<std::pair<uint32_t, uint32_t>> writable_;
    std::vector<std::vector<uint8_t>> pristine_;
};

void setup_stack(GuestMemory& m, const std::vector<uint32_t>& args) {
    m.write<uint32_t>(kEntryEsp, kSentinel);
    for (size_t i = 0; i < args.size(); i++) m.write<uint32_t>(kEntryEsp + 4 + 4 * (uint32_t)i, args[i]);
}

State entry_state(Rng& r) {
    State s;
    s.gpr[0] = gen_word(r), s.gpr[1] = gen_word(r), s.gpr[2] = gen_word(r);
    s.gpr[3] = 0x11111111, s.gpr[4] = kEntryEsp, s.gpr[5] = 0x44444444;
    s.gpr[6] = 0x22222222, s.gpr[7] = 0x33333333;
    for (auto& x : s.xmm)
        for (int k = 0; k < 4; k++) {
            const uint32_t w = bits(gen_float(r));
            std::memcpy(x.b + 4 * k, &w, 4);
        }
    return s;
}

class Recompiled : public Engine {
public:
    using Engine::Engine;

    // 0 on success, else the signal number or 1000 for fatal().
    int run(uint32_t fn, const State& in, State* out) {
        const FuncEntry* f = find_function(fn);
        if (!f) fatal("%08x was not recompiled", fn);
        Context c;
        c.mem = &mem_;
        uint32_t* g[8] = {&c.eax, &c.ecx, &c.edx, &c.ebx, &c.esp, &c.ebp, &c.esi, &c.edi};
        for (int k = 0; k < 8; k++) *g[k] = in.gpr[k];
        std::memcpy(c.xmm, in.xmm, sizeof c.xmm);
        c.fpu = in.fpu;
        g_fatal_msg[0] = 0;
        const int sig = sigsetjmp(g_jmp, 1);
        if (sig == 0) {
            g_guarded = 1;
            arm_watchdog(true);
            f->fn(c);
        }
        arm_watchdog(false);
        g_guarded = 0;
        for (int k = 0; k < 8; k++) out->gpr[k] = *g[k];
        std::memcpy(out->xmm, c.xmm, sizeof c.xmm);
        out->fpu = c.fpu;
        return sig;
    }
};

class Emulated : public Engine {
public:
    enum Outcome { kDone, kFault, kLimit, kLeft, kNoShim, kHostSignal };

    explicit Emulated(const std::string& binary) : Engine(binary) {
        check(uc_open(UC_ARCH_X86, UC_MODE_32, &uc_), "open");
        // SSE needs CR4.OSFXSR/OSXMMEXCPT and CR0.EM clear (CR0.MP set).
        uint64_t cr0 = 0, cr4 = 0;
        uc_reg_read(uc_, UC_X86_REG_CR0, &cr0);
        uc_reg_read(uc_, UC_X86_REG_CR4, &cr4);
        cr0 = (cr0 & ~4ull) | 2ull;
        cr4 |= (1u << 9) | (1u << 10);
        uc_reg_write(uc_, UC_X86_REG_CR0, &cr0);
        uc_reg_write(uc_, UC_X86_REG_CR4, &cr4);
        std::vector<std::pair<uint64_t, uint64_t>> r;
        for (auto [a, n] : mem_.committed()) r.emplace_back(a, a + n);
        std::sort(r.begin(), r.end());
        std::vector<std::pair<uint64_t, uint64_t>> merged;
        for (auto x : r) {
            if (!merged.empty() && x.first <= merged.back().second)
                merged.back().second = std::max(merged.back().second, x.second);
            else merged.push_back(x);
        }
        // __TEXT (and the page below it) read and execute only, like the host side.
        const uint64_t t1 = (uint64_t)text_.first + text_.second;
        for (auto [a, e] : merged) {
            if (a < t1) {
                check(uc_mem_map_ptr(uc_, a, std::min(e, t1) - a, UC_PROT_READ | UC_PROT_EXEC,
                                     mem_.ptr((uint32_t)a)), "map text");
                if (e > t1) check(uc_mem_map_ptr(uc_, t1, e - t1, UC_PROT_ALL, mem_.ptr((uint32_t)t1)), "map");
            } else {
                check(uc_mem_map_ptr(uc_, a, e - a, UC_PROT_ALL, mem_.ptr((uint32_t)a)), "map");
            }
        }
        uc_hook h;
        for (const auto& sec : image().sections())
            if (sec.type() == 8) {
                stubs_ = {sec.addr, sec.addr + sec.size};
                check(uc_hook_add(uc_, &h, UC_HOOK_CODE, (void*)&on_stub, this, sec.addr,
                                  sec.addr + sec.size - 1),
                      "hook stubs");
            }
        check(uc_hook_add(uc_, &h, UC_HOOK_BLOCK, (void*)&on_block, this, 1, 0), "hook blocks");
        const recomp::Section* t = image().text();
        textsec_ = {t->addr, t->addr + t->size};
    }
    ~Emulated() override { uc_close(uc_); }

    Outcome run(uint32_t fn, const State& in, State* out, std::string* why) {
        static const int kRegs[8] = {UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX,
                                     UC_X86_REG_EBX, UC_X86_REG_ESP, UC_X86_REG_EBP,
                                     UC_X86_REG_ESI, UC_X86_REG_EDI};
        for (int k = 0; k < 8; k++) set(kRegs[k], in.gpr[k]);
        for (int k = 0; k < 8; k++) uc_reg_write(uc_, UC_X86_REG_XMM0 + k, in.xmm[k].b);
        set(UC_X86_REG_EFLAGS, 0x202);
        write_fpu(in.fpu);
        stop_ = kDone;
        why_.clear();
        uc_err e = UC_ERR_OK;
        const int sig = sigsetjmp(g_jmp, 1);
        if (sig == 0) {
            g_guarded = 1;
            e = uc_emu_start(uc_, fn, kSentinel, 0, kMaxInsns);
        }
        g_guarded = 0;
        for (int k = 0; k < 8; k++) out->gpr[k] = get(kRegs[k]);
        for (int k = 0; k < 8; k++) uc_reg_read(uc_, UC_X86_REG_XMM0 + k, out->xmm[k].b);
        out->fpu = read_fpu();
        if (sig) {
            *why = "native code raised signal " + std::to_string(sig) + " " + g_fatal_msg;
            return kHostSignal;
        }
        *why = why_;
        if (stop_ != kDone) return stop_;
        if (e != UC_ERR_OK) {
            *why = uc_strerror(e);
            return kFault;
        }
        if (get(UC_X86_REG_EIP) != kSentinel) return kLimit;
        return kDone;
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

    // x87 state: physical registers FP0-7, TOP in the status word, tags (3 = empty).
    void write_fpu(const X87& f) {
        uint32_t cw = f.cw, sw = (f.top & 7) << 11, tag = 0;
        for (int p = 0; p < 8; p++) {
            if (!(f.valid >> p & 1)) tag |= 3u << (2 * p);
            uint8_t buf[16] = {};
            std::memcpy(buf, &f.r[p].mant, 8);
            std::memcpy(buf + 8, &f.r[p].se, 2);
            uc_reg_write(uc_, UC_X86_REG_FP0 + p, buf);
        }
        uc_reg_write(uc_, UC_X86_REG_FPCW, &cw);
        uc_reg_write(uc_, UC_X86_REG_FPSW, &sw);
        uc_reg_write(uc_, UC_X86_REG_FPTAG, &tag);
    }
    X87 read_fpu() {
        X87 f;
        uint32_t cw = 0, sw = 0, tag = 0;
        uc_reg_read(uc_, UC_X86_REG_FPCW, &cw);
        uc_reg_read(uc_, UC_X86_REG_FPSW, &sw);
        uc_reg_read(uc_, UC_X86_REG_FPTAG, &tag);
        f.cw = (uint16_t)cw;
        f.top = sw >> 11 & 7;
        f.valid = 0;
        for (int p = 0; p < 8; p++) {
            if ((tag >> (2 * p) & 3) != 3) f.valid |= 1u << p;
            uint8_t buf[16] = {};
            uc_reg_read(uc_, UC_X86_REG_FP0 + p, buf);
            std::memcpy(&f.r[p].mant, buf, 8);
            std::memcpy(&f.r[p].se, buf + 8, 2);
        }
        return f;
    }

    void halt(Outcome o, std::string why) {
        if (stop_ == kDone) {
            stop_ = o;
            why_ = std::move(why);
        }
        uc_emu_stop(uc_);
    }

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
        if (it == stubs.end()) return;
        if (it->second.internal) {
            self->set(UC_X86_REG_EIP, it->second.internal);
            return;
        }
        const ImportShim* shim = find_import_shim(it->second.name);
        if (!shim) return self->halt(kNoShim, it->second.name);
        self->run_native(shim->fn);
    }

    // Execution must stay inside translated functions (or reach the sentinel).
    static void on_block(uc_engine*, uint64_t addr64, uint32_t, void* user) {
        auto* self = static_cast<Emulated*>(user);
        const uint32_t addr = (uint32_t)addr64;
        if (addr == kSentinel || (addr >= self->stubs_.first && addr < self->stubs_.second)) return;
        if (addr < self->textsec_.first || addr >= self->textsec_.second)
            return self->halt(kLeft, "execution outside __text");
        const recomp::Function* f = self->image().function_containing(addr);
        if (!f || !find_function(f->addr))
            return self->halt(kLeft, "entered untranslated " + recomp::demangle(f ? f->name : "?"));
    }

    uc_engine* uc_ = nullptr;
    std::pair<uint32_t, uint32_t> stubs_{0, 0}, textsec_{0, 0};
    Outcome stop_ = kDone;
    std::string why_;
};

// ---- comparison ----

bool compare(const char* fname, int case_no, Engine& a, const State& sa, Engine& b,
             const State& sb, std::string* report) {
    char buf[512];
    for (int k = 0; k < 8; k++)
        if (sa.gpr[k] != sb.gpr[k]) {
            std::snprintf(buf, sizeof buf, "%s case %d: %s emulated %08x recompiled %08x", fname,
                          case_no, kGprNames[k], sa.gpr[k], sb.gpr[k]);
            *report = buf;
            return false;
        }
    for (int k = 0; k < 8; k++)
        if (std::memcmp(sa.xmm[k].b, sb.xmm[k].b, 16) != 0) {
            uint32_t x[4], y[4];
            std::memcpy(x, sa.xmm[k].b, 16), std::memcpy(y, sb.xmm[k].b, 16);
            std::snprintf(buf, sizeof buf,
                          "%s case %d: xmm%d emulated %08x %08x %08x %08x recompiled %08x %08x "
                          "%08x %08x",
                          fname, case_no, k, x[3], x[2], x[1], x[0], y[3], y[2], y[1], y[0]);
            *report = buf;
            return false;
        }
    const X87 &fa = sa.fpu, &fb = sb.fpu;
    if (fa.top != fb.top || fa.valid != fb.valid || fa.cw != fb.cw) {
        std::snprintf(buf, sizeof buf,
                      "%s case %d: x87 top/valid/cw emulated %u/%02x/%04x recompiled %u/%02x/%04x",
                      fname, case_no, fa.top, fa.valid, fa.cw, fb.top, fb.valid, fb.cw);
        *report = buf;
        return false;
    }
    for (int p = 0; p < 8; p++)
        if (fa.valid >> p & 1 && (fa.r[p].mant != fb.r[p].mant || fa.r[p].se != fb.r[p].se)) {
            std::snprintf(buf, sizeof buf,
                          "%s case %d: x87 r%d emulated %04x:%016llx recompiled %04x:%016llx",
                          fname, case_no, p, fa.r[p].se, (unsigned long long)fa.r[p].mant,
                          fb.r[p].se, (unsigned long long)fb.r[p].mant);
            *report = buf;
            return false;
        }
    for (auto [addr, n] : a.writable()) {
        const uint8_t* p = a.mem().ptr(addr);
        const uint8_t* q = b.mem().ptr(addr);
        if (std::memcmp(p, q, n) == 0) continue;
        uint32_t off = 0;
        while (p[off] == q[off]) off++;
        off &= ~3u;
        uint32_t x, y;
        std::memcpy(&x, p + off, 4), std::memcpy(&y, q + off, 4);
        std::snprintf(buf, sizeof buf, "%s case %d: memory at %08x emulated %08x recompiled %08x",
                      fname, case_no, addr + off, x, y);
        *report = buf;
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s GAME.app ROOTS_FILE [--cases N] [--seed S] [--only 0xADDR] "
                             "[--verbose]\n", argv[0]);
        return 2;
    }
    const std::string binary = std::string(argv[1]) + "/Contents/MacOS/scimitar_sf";
    int cases = 8;
    uint64_t seed = 1;
    uint32_t only = 0;
    bool verbose = false;
    for (int k = 3; k < argc; k++) {
        const std::string a = argv[k];
        if (a == "--cases" && k + 1 < argc) cases = std::stoi(argv[++k]);
        else if (a == "--seed" && k + 1 < argc) seed = std::stoull(argv[++k]);
        else if (a == "--only" && k + 1 < argc) only = (uint32_t)std::stoul(argv[++k], nullptr, 16);
        else if (a == "--verbose") verbose = true;
    }
    std::vector<uint32_t> roots;
    {
        std::ifstream f(argv[2]);
        for (std::string l; std::getline(f, l);)
            if (!l.empty() && l[0] != '#') roots.push_back((uint32_t)std::stoul(l, nullptr, 16));
    }
    if (only) roots = {only};
    install_guards();
    x87::set_qemu_compat(true);  // compare against Unicorn's x87 exactly

    Emulated emu(binary);
    Recompiled rec(binary);
    struct Totals {
        int run = 0, same = 0, mismatch = 0, failed = 0;
        std::map<std::string, int> skipped;
    } t;
    std::map<std::string, int> failures;  // recompiled fatal/fault reasons
    int functions_ok = 0, functions_bad = 0, functions_untested = 0;
    std::vector<std::string> reports;

    for (uint32_t fn : roots) {
        const recomp::Function* f = emu.image().function_at(fn);
        if (!f) {
            std::fprintf(stderr, "no function at %08x\n", fn);
            return 2;
        }
        const std::string name = recomp::demangle(f->name);
        int ran = 0, bad = 0;
        for (int k = 0; k < cases; k++) {
            const uint64_t cs = seed * 0x100000001b3ull ^ ((uint64_t)fn << 16) ^ (uint64_t)k;
            Rng r(cs);
            const std::vector<uint32_t> args = gen_args(f->name, r);
            const State in = entry_state(r);
            emu.reset(cs);
            setup_stack(emu.mem(), args);
            State se, sr;
            std::string why;
            const auto o = emu.run(fn, in, &se, &why);
            if (o != Emulated::kDone) {
                static const char* kWhy[] = {"done", "fault", "limit", "left closure", "no shim",
                                             "host signal"};
                t.skipped[kWhy[o]]++;
                if (verbose)
                    std::printf("  skip %s case %d: %s %s\n", name.c_str(), k, kWhy[o], why.c_str());
                continue;
            }
            rec.reset(cs);
            setup_stack(rec.mem(), args);
            const int sig = rec.run(fn, in, &sr);
            t.run++, ran++;
            if (sig) {
                t.failed++, bad++;
                const std::string msg = sig == 1000 ? std::string("fatal: ") + g_fatal_msg
                                                    : "signal " + std::to_string(sig);
                failures[msg.substr(0, msg.find(" at "))]++;
                if (reports.size() < 40) reports.push_back(name + " case " + std::to_string(k) + ": " + msg);
                continue;
            }
            std::string rep;
            if (compare(name.c_str(), k, emu, se, rec, sr, &rep)) {
                t.same++;
            } else {
                t.mismatch++, bad++;
                if (reports.size() < 40) reports.push_back(rep);
            }
        }
        if (!ran) functions_untested++;
        else if (bad) functions_bad++;
        else functions_ok++;
        if (verbose) std::printf("%s %08x %s (%d run)\n", bad ? "BAD " : "ok  ", fn, name.c_str(), ran);
    }

    for (const auto& r : reports) std::printf("%s\n", r.c_str());
    std::printf("difftest: %zu functions: %d identical in every case run, %d with differences, "
                "%d with no runnable case\n",
                roots.size(), functions_ok, functions_bad, functions_untested);
    std::printf("cases: %d run, %d identical, %d different, %d failed in recompiled code; skipped:",
                t.run, t.same, t.mismatch, t.failed);
    for (const auto& [w, n] : t.skipped) std::printf(" %s %d,", w.c_str(), n);
    std::printf("\n");
    for (const auto& [w, n] : failures) std::printf("  recompiled failure x%d: %s\n", n, w.c_str());
    return t.mismatch || t.failed ? 1 : 0;
}
