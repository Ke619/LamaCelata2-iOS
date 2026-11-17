// scimitar-analyze: call closure of one or more functions in the game binary.
//
//   scimitar-analyze BINARY ROOT_SYMBOL... [--stop SYMBOL_OR_PREFIX]... [--list]
//   scimitar-analyze BINARY --disasm SYMBOL_OR_0xADDR...
//   scimitar-analyze BINARY --callers SYMBOL...
//   scimitar-analyze BINARY --coverage [--list-failures | --list-indirect]
//   scimitar-analyze BINARY --difftest-candidates [--max N] [--max-closure K]
//                    [--allow-import DYLD_NAME]... [--out FILE]
//
// Follows direct calls and tail calls (including stubs bound to the image's own
// weak definitions) and reports what the translator would face: function and
// instruction counts, imports used, indirect calls/jumps, unsupported instructions.
//
// --coverage translates every function of the image on its own and reports how
// many succeed, by instructions too, and what stops the others.
// --difftest-candidates prints (as 0xADDR lines) functions whose whole direct
// call closure translates and only calls the allowed imports, chosen to cover
// as many instruction kinds as possible, for tests/difftest.

#include <cxxabi.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "emit.h"
#include "flow.h"
#include "macho.h"

using namespace recomp;

namespace {

// Result of translating one function on its own.
struct Translation {
    bool ok = false;
    std::string error;
    size_t insns = 0, tables = 0, indirect_jumps = 0;
    std::set<uint32_t> callees;
    std::set<std::string> imports;
    std::set<std::string> features;  // instruction kinds used, for test selection
};

std::string feature_of(const Insn& i) {
    const auto m = i.in.mnemonic;
    if (i.xfer == Xfer::JumpIndirect) return "indirect-jump";
    if (i.xfer == Xfer::JumpTable) return "jump-table";
    for (unsigned k = 0; k < i.in.operand_count_visible; k++) {
        const auto& op = i.ops[k];
        if (op.type == ZYDIS_OPERAND_TYPE_REGISTER) {
            const auto cls = ZydisRegisterGetClass(op.reg.value);
            if (cls == ZYDIS_REGCLASS_XMM) return std::string("sse:") + ZydisMnemonicGetString(m);
            if (cls == ZYDIS_REGCLASS_X87) return std::string("x87:") + ZydisMnemonicGetString(m);
            if (cls == ZYDIS_REGCLASS_MMX) return std::string("mmx:") + ZydisMnemonicGetString(m);
        }
    }
    if (i.in.meta.isa_set == ZYDIS_ISA_SET_X87 || i.in.meta.category == ZYDIS_CATEGORY_X87_ALU)
        return std::string("x87:") + ZydisMnemonicGetString(m);
    switch (m) {
    case ZYDIS_MNEMONIC_MUL: case ZYDIS_MNEMONIC_IMUL: case ZYDIS_MNEMONIC_DIV:
    case ZYDIS_MNEMONIC_IDIV:
        return std::string("muldiv:") + ZydisMnemonicGetString(m) +
               (i.in.operand_count_visible == 1 && i.ops[0].size < 32 ? "8/16" : "");
    // Results depend on the host or stop the program: not comparable.
    case ZYDIS_MNEMONIC_RDTSC: case ZYDIS_MNEMONIC_CPUID: case ZYDIS_MNEMONIC_INT3:
    case ZYDIS_MNEMONIC_HLT: case ZYDIS_MNEMONIC_UD2:
        return "nondeterministic";
    case ZYDIS_MNEMONIC_ROL: case ZYDIS_MNEMONIC_ROR: case ZYDIS_MNEMONIC_SHLD:
    case ZYDIS_MNEMONIC_SHRD: case ZYDIS_MNEMONIC_CWDE: case ZYDIS_MNEMONIC_CBW:
    case ZYDIS_MNEMONIC_CDQ: case ZYDIS_MNEMONIC_CWD: case ZYDIS_MNEMONIC_XCHG:
    case ZYDIS_MNEMONIC_XADD: case ZYDIS_MNEMONIC_BSWAP: case ZYDIS_MNEMONIC_BSF:
    case ZYDIS_MNEMONIC_BSR: case ZYDIS_MNEMONIC_CLD: case ZYDIS_MNEMONIC_STD:
    case ZYDIS_MNEMONIC_MOVSB: case ZYDIS_MNEMONIC_MOVSW: case ZYDIS_MNEMONIC_STOSB:
    case ZYDIS_MNEMONIC_STOSW: case ZYDIS_MNEMONIC_STOSD: case ZYDIS_MNEMONIC_LODSB:
    case ZYDIS_MNEMONIC_LODSW: case ZYDIS_MNEMONIC_LODSD: case ZYDIS_MNEMONIC_CMPSB:
    case ZYDIS_MNEMONIC_CMPSW: case ZYDIS_MNEMONIC_SCASB: case ZYDIS_MNEMONIC_SCASW:
    case ZYDIS_MNEMONIC_SCASD: case ZYDIS_MNEMONIC_MOVSD: case ZYDIS_MNEMONIC_CMPSD:
        return std::string("int:") + ZydisMnemonicGetString(m) +
               (i.in.attributes & ZYDIS_ATTRIB_HAS_LOCK ? "-lock" : "") +
               (i.in.attributes & (ZYDIS_ATTRIB_HAS_REP | ZYDIS_ATTRIB_HAS_REPE |
                                   ZYDIS_ATTRIB_HAS_REPNE) ? "-rep" : "");
    default:
        break;
    }
    if (i.in.attributes & ZYDIS_ATTRIB_HAS_LOCK) return std::string("int:lock-") + ZydisMnemonicGetString(m);
    if (m >= ZYDIS_MNEMONIC_CMOVB && m <= ZYDIS_MNEMONIC_CMOVZ) return "cmov";
    return "";
}

Translation translate(const Image& image, const Function& f) {
    Translation t;
    DecodedFunction fn = decode_image_function(image, f);
    t.insns = fn.insns.size();
    for (const auto& [a, i] : fn.insns) {
        t.tables += i.xfer == Xfer::JumpTable;
        t.indirect_jumps += i.xfer == Xfer::JumpIndirect;
        std::string ft = feature_of(i);
        if (!ft.empty()) t.features.insert(ft);
    }
    try {
        EmitResult r = emit_function(fn, func_symbol(f.addr), &image, "");
        t.ok = true;
        t.callees = std::move(r.callees);
        t.imports = std::move(r.imports);
    } catch (const EmitError& e) {
        t.error = e.what();
        // "f_xxxxxxxx: unsupported instruction at 0012abcd: mulss %xmm1, %xmm0"
        // -> "unsupported instruction: mulss"
        const size_t at = t.error.find(" at ");
        const size_t colon = t.error.find(": ", at == std::string::npos ? 0 : at);
        std::string head = t.error.rfind("f_", 0) == 0 ? t.error.substr(t.error.find(": ") + 2) : t.error;
        head = head.substr(0, head.find(" at "));
        if (head.rfind("unsupported instruction", 0) == 0 && colon != std::string::npos) {
            std::string mn = t.error.substr(colon + 2);
            head += ": " + mn.substr(0, mn.find(' '));
        }
        t.error = head;
    }
    return t;
}

std::vector<Translation> translate_all(const Image& image) {
    std::vector<Translation> out;
    out.reserve(image.functions().size());
    for (const Function& f : image.functions()) out.push_back(translate(image, f));
    return out;
}

// Indirect jumps not recognised as jump tables, unless they follow an epilogue
// (`leave`/`pop`: a tail call through a pointer).
void list_indirect(const Image& image) {
    for (const Function& f : image.functions()) {
        DecodedFunction fn =
            decode_image_function(image, f);
        const Insn* prev = nullptr;
        for (const auto& [a, i] : fn.insns) {
            if (i.xfer == Xfer::JumpIndirect && prev &&
                prev->in.mnemonic != ZYDIS_MNEMONIC_LEAVE && prev->in.mnemonic != ZYDIS_MNEMONIC_POP)
                std::printf("%08x %-40s after %-30s in %s\n", a, i.text.c_str(),
                            prev->text.c_str(), demangle(f.name).c_str());
            prev = &i;
        }
    }
}

int coverage(const Image& image, bool list_failures) {
    const auto all = translate_all(image);
    size_t ok = 0, insns = 0, ok_insns = 0, tables = 0, indirect = 0;
    std::map<std::string, std::pair<size_t, size_t>> why;  // reason -> functions, insns
    for (size_t k = 0; k < all.size(); k++) {
        insns += all[k].insns;
        tables += all[k].tables;
        indirect += all[k].indirect_jumps;
        if (all[k].ok) {
            ok++;
            ok_insns += all[k].insns;
        } else {
            auto& w = why[all[k].error];
            w.first++;
            w.second += all[k].insns;
            if (list_failures)
                std::printf("FAIL %08x %s: %s\n", image.functions()[k].addr,
                            demangle(image.functions()[k].name).c_str(), all[k].error.c_str());
        }
    }
    std::printf("translatable: %zu of %zu functions (%.1f%%), %zu of %zu instructions (%.1f%%)\n",
                ok, all.size(), 100.0 * ok / all.size(), ok_insns, insns,
                100.0 * ok_insns / insns);
    std::printf("jump tables resolved: %zu; other indirect jumps (tail calls through a "
                "pointer): %zu\n", tables, indirect);
    std::vector<std::pair<std::pair<size_t, size_t>, std::string>> v;
    for (const auto& [r, n] : why) v.push_back({n, r});
    std::sort(v.rbegin(), v.rend());
    std::printf("first blocker (functions, instructions in them):\n");
    for (size_t k = 0; k < v.size() && k < 40; k++)
        std::printf("  %7zu %9zu  %s\n", v[k].first.first, v[k].first.second, v[k].second.c_str());
    return 0;
}

int difftest_candidates(const Image& image, size_t max, size_t max_closure,
                        const std::set<std::string>& allowed, const std::string& out_path) {
    const auto all = translate_all(image);
    std::map<uint32_t, size_t> index;
    for (size_t k = 0; k < all.size(); k++) index[image.functions()[k].addr] = k;

    // A function qualifies when everything it reaches by direct calls translates
    // and calls only allowed imports. Features are those of the whole closure.
    struct Candidate {
        uint32_t addr;
        size_t insns;
        std::set<std::string> features;
    };
    std::vector<Candidate> cands;
    for (size_t k = 0; k < all.size(); k++) {
        if (!all[k].ok) continue;
        std::set<uint32_t> seen;
        std::vector<uint32_t> work{image.functions()[k].addr};
        bool good = true;
        Candidate c{image.functions()[k].addr, 0, {}};
        while (!work.empty() && good) {
            const uint32_t a = work.back();
            work.pop_back();
            if (!seen.insert(a).second) continue;
            if (seen.size() > max_closure) { good = false; break; }
            const Translation& t = all[index.at(a)];
            if (!t.ok) { good = false; break; }
            for (const auto& imp : t.imports)
                if (!allowed.count(imp)) good = false;
            if (t.features.count("nondeterministic")) { good = false; break; }
            c.insns += t.insns;
            c.features.insert(t.features.begin(), t.features.end());
            for (uint32_t x : t.callees) work.push_back(x);
        }
        if (good) cands.push_back(std::move(c));
    }

    // Greedy selection: each round, every feature not yet covered often enough
    // picks its smallest remaining candidate; then fill with plain functions
    // spread over the image.
    std::map<std::string, size_t> covered;
    std::set<uint32_t> chosen;
    std::vector<const Candidate*> by_size;
    for (const auto& c : cands) by_size.push_back(&c);
    std::sort(by_size.begin(), by_size.end(), [](const Candidate* a, const Candidate* b) {
        return a->insns != b->insns ? a->insns < b->insns : a->addr < b->addr;
    });
    std::set<std::string> features;
    for (const auto& c : cands) features.insert(c.features.begin(), c.features.end());
    constexpr size_t kPerFeature = 12;
    for (bool progress = true; progress && chosen.size() < max;) {
        progress = false;
        for (const auto& ft : features) {
            if (covered[ft] >= kPerFeature || chosen.size() >= max) continue;
            for (const Candidate* c : by_size)
                if (!chosen.count(c->addr) && c->features.count(ft)) {
                    chosen.insert(c->addr);
                    for (const auto& f2 : c->features) covered[f2]++;
                    progress = true;
                    break;
                }
        }
    }
    const size_t plain = cands.size();
    for (size_t k = 0; k < plain && chosen.size() < max; k += std::max<size_t>(1, plain / max))
        chosen.insert(cands[k].addr);

    std::fprintf(stderr, "%zu of %zu functions qualify; chose %zu; features covered:", cands.size(),
                 all.size(), chosen.size());
    for (const auto& [ft, n] : covered) std::fprintf(stderr, " %s(%zu)", ft.c_str(), n);
    std::fprintf(stderr, "\n");
    FILE* out = out_path.empty() ? stdout : std::fopen(out_path.c_str(), "w");
    if (!out) {
        std::fprintf(stderr, "cannot write %s\n", out_path.c_str());
        return 1;
    }
    for (uint32_t a : chosen) std::fprintf(out, "0x%08x\n", a);
    if (out != stdout) std::fclose(out);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s BINARY ROOT... [--stop NAME_OR_PREFIX]... [--list]\n",
                     argv[0]);
        return 2;
    }
    Image image(argv[1]);
    const size_t hidden = discover_functions(image);
    if (std::getenv("SCIMITAR_VERBOSE"))
        std::fprintf(stderr, "%zu functions without a symbol found from call targets\n", hidden);
    if (std::string(argv[2]) == "--coverage") {
        if (argc > 3 && std::string(argv[3]) == "--list-indirect") {
            list_indirect(image);
            return 0;
        }
        return coverage(image, argc > 3 && std::string(argv[3]) == "--list-failures");
    }
    if (std::string(argv[2]) == "--difftest-candidates") {
        size_t max = 2000, max_closure = 64;
        std::set<std::string> allowed;
        std::string out;
        for (int k = 3; k + 1 < argc; k += 2) {
            const std::string a = argv[k];
            if (a == "--max") max = std::stoul(argv[k + 1]);
            else if (a == "--max-closure") max_closure = std::stoul(argv[k + 1]);
            else if (a == "--allow-import") allowed.insert(argv[k + 1]);
            else if (a == "--out") out = argv[k + 1];
            else {
                std::fprintf(stderr, "unknown option %s\n", a.c_str());
                return 2;
            }
        }
        return difftest_candidates(image, max, max_closure, allowed, out);
    }
    if (std::string(argv[2]) == "--disasm") {
        // One function by address or symbol, with call targets named.
        for (int k = 3; k < argc; k++) {
            const std::string a = argv[k];
            const Function* f = a.rfind("0x", 0) == 0
                                    ? image.function_containing((uint32_t)std::stoul(a, nullptr, 16))
                                    : image.function_named(a);
            if (!f) {
                std::fprintf(stderr, "no function at/named %s\n", a.c_str());
                return 1;
            }
            DecodedFunction fn =
                decode_image_function(image, *f);
            std::printf("%08x-%08x %s\n", f->addr, f->end, demangle(f->name).c_str());
            for (const auto& e : fn.errors) std::printf("  ! %s\n", e.c_str());
            for (const auto& [ia, i] : fn.insns) {
                std::string note;
                if (i.xfer == Xfer::Call || i.xfer == Xfer::TailCall || i.xfer == Xfer::CondTailCall)
                    note = demangle(image.function_at(i.target)->name);
                else if (i.xfer == Xfer::CallImport || i.xfer == Xfer::TailImport) {
                    const Import& imp = image.stubs().at(i.target);
                    note = "stub " + demangle(imp.name);
                }
                std::printf("  %s%08x  %-48s %s\n", fn.labels.count(ia) ? ">" : " ", ia,
                            i.text.c_str(), note.c_str());
            }
        }
        return 0;
    }
    if (std::string(argv[2]) == "--callers") {
        // Reverse lookup: every function that calls or tail-calls the named ones.
        std::set<uint32_t> targets;
        for (int k = 3; k < argc; k++) {
            const Function* f = image.function_named(argv[k]);
            if (!f) {
                std::fprintf(stderr, "no function named %s\n", argv[k]);
                return 1;
            }
            targets.insert(f->addr);
        }
        for (const Function& f : image.functions()) {
            DecodedFunction fn =
                decode_image_function(image, f);
            for (const auto& [a, i] : fn.insns) {
                uint32_t t = i.target;
                if (i.xfer == Xfer::CallImport || i.xfer == Xfer::TailImport)
                    t = image.stubs().at(i.target).internal;
                if ((i.xfer == Xfer::Call || i.xfer == Xfer::TailCall || i.xfer == Xfer::CondTailCall ||
                     i.xfer == Xfer::CallImport || i.xfer == Xfer::TailImport) &&
                    targets.count(t)) {
                    std::printf("%08x %s -> %s\n", a, demangle(f.name).c_str(),
                                demangle(image.function_at(t)->name).c_str());
                }
            }
        }
        return 0;
    }
    std::vector<std::string> roots, stops;
    bool list = false;
    for (int k = 2; k < argc; k++) {
        std::string a = argv[k];
        if (a == "--stop" && k + 1 < argc) stops.push_back(argv[++k]);
        else if (a == "--list") list = true;
        else roots.push_back(a);
    }
    auto stopped = [&](const Function& f) {
        const std::string d = demangle(f.name);
        for (const auto& s : stops)
            if (f.name == s || d.rfind(s, 0) == 0) return true;
        return false;
    };

    std::set<uint32_t> seen, stopped_hit;
    std::vector<uint32_t> work;
    for (const auto& r : roots) {
        const Function* f = image.function_named(r);
        if (!f) {
            std::fprintf(stderr, "no function named %s\n", r.c_str());
            return 1;
        }
        work.push_back(f->addr);
    }

    size_t total_insns = 0;
    std::map<std::string, int> imports, unsupported;
    std::map<uint32_t, int> indirect_calls, indirect_jumps;
    std::map<uint32_t, size_t> sizes;
    int errors = 0;
    while (!work.empty()) {
        const uint32_t a = work.back();
        work.pop_back();
        if (!seen.insert(a).second) continue;
        const Function* f = image.function_at(a);
        DecodedFunction fn = decode_image_function(image, *f);
        total_insns += fn.insns.size();
        sizes[a] = fn.insns.size();
        for (const auto& e : fn.errors) {
            if (e.rfind("unsupported instruction", 0) == 0) {
                const std::string mn = e.substr(e.find(": ") + 2);
                unsupported[mn.substr(0, mn.find(' '))]++;
            } else {
                if (errors++ < 10)
                    std::printf("  %s: %s\n", demangle(f->name).c_str(), e.c_str());
            }
        }
        for (const auto& [ia, i] : fn.insns) {
            auto follow = [&](uint32_t t) {
                const Function* g = image.function_at(t);
                if (!g) return;
                if (stopped(*g)) stopped_hit.insert(t);
                else work.push_back(t);
            };
            switch (i.xfer) {
            case Xfer::Call:
            case Xfer::TailCall:
            case Xfer::CondTailCall:
                follow(i.target);
                break;
            case Xfer::CallImport:
            case Xfer::TailImport: {
                const Import& imp = image.stubs().at(i.target);
                if (imp.internal) follow(imp.internal);
                else imports[imp.name]++;
                break;
            }
            case Xfer::CallIndirect:
                indirect_calls[a]++;
                break;
            case Xfer::JumpIndirect:
            case Xfer::JumpTable:
                indirect_jumps[a]++;
                break;
            default:
                break;
            }
        }
    }

    std::printf("closure: %zu functions, %zu instructions, %zu stop points reached\n",
                seen.size(), total_insns, stopped_hit.size());
    std::printf("indirect calls: %zu sites in %zu functions; indirect jumps: %zu functions\n",
                [&] { size_t n = 0; for (auto& [k, v] : indirect_calls) n += v; return n; }(),
                indirect_calls.size(), indirect_jumps.size());
    std::printf("imports (%zu):", imports.size());
    for (const auto& [n, c] : imports) std::printf(" %s", n.c_str());
    std::printf("\nunsupported mnemonics:");
    std::vector<std::pair<int, std::string>> un;
    for (const auto& [n, c] : unsupported) un.push_back({c, n});
    std::sort(un.rbegin(), un.rend());
    for (const auto& [c, n] : un) std::printf(" %s(%d)", n.c_str(), c);
    std::printf("\n");
    if (!indirect_jumps.empty()) {
        std::printf("functions with indirect jumps:\n");
        for (const auto& [a, c] : indirect_jumps)
            std::printf("  %08x %s\n", a, demangle(image.function_at(a)->name).c_str());
    }
    if (!stopped_hit.empty()) {
        std::printf("stop points reached:\n");
        for (uint32_t a : stopped_hit)
            std::printf("  %08x %s\n", a, demangle(image.function_at(a)->name).c_str());
    }
    if (list) {
        std::printf("functions:\n");
        for (uint32_t a : seen)
            std::printf("  %08x %5zu %s%s\n", a, sizes[a],
                        demangle(image.function_at(a)->name).c_str(),
                        indirect_calls.count(a) ? "  [indirect calls]" : "");
    }
    return 0;
}
