// The shader cache and startup precompile (shader_cache.h, docs/METAL.md).
//
// Files, in $HOME/Library/Caches/scimitar-recomp/<game>/shaders (or
// SCIMITAR_SHADER_CACHE):
//   programs.bin   ARB program texts the game used: [u64 hash][u32 n][n bytes]...
//   pipelines.bin  "SCPL", u32 record size, then Record... (appended in play)
//   forge.bin      the ARB programs of the game's .forge archives (one scan;
//                  same layout as programs.bin, a record with hash 0 ends it)
//   v<translator>/ archive.bin (MTLBinaryArchive), done.bin (u64 item hashes
//                  compiled for this translator version)
// The translator version hashes arb::prelude() and the translation of a
// sample program with kFormat, so a translator change rebuilds everything.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "shader_cache.h"

#include <TargetConditionals.h>
#include <dispatch/dispatch.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <pthread.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "scimitar/arb_msl.h"
#include "scimitar/imports.h"

namespace scimitar::metal {

namespace {

constexpr uint32_t kFormat = 1;  // bump when the cache's meaning changes

struct Record {
    uint64_t vtext, ftext;
    uint32_t vopts[3];
    int32_t alpha;
    PipeDesc d;
};

double ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

bool env_off(const char* name) {
    const char* v = std::getenv(name);
    return v && v[0] == '0';
}

void mkdirs(const std::string& path) {
    for (size_t at = 1; (at = path.find('/', at)) != std::string::npos; at++) mkdir(path.substr(0, at).c_str(), 0755);
    mkdir(path.c_str(), 0755);
}

// ---- LZO1X decompression (the forge's compressed chunks) ----

// Decodes one LZO1X stream. False on malformed input.
bool lzo1x_decode(const uint8_t* in, size_t in_n, uint8_t* out, size_t out_n) {
    const uint8_t* ip = in;
    const uint8_t* const ie = in + in_n;
    uint8_t* op = out;
    uint8_t* const oe = out + out_n;
    auto lits = [&](size_t n) {
        if ((size_t)(ie - ip) < n || (size_t)(oe - op) < n) return false;
        std::memcpy(op, ip, n), op += n, ip += n;
        return true;
    };
    auto copy = [&](const uint8_t* m, size_t n) {
        if (m < out || (size_t)(oe - op) < n) return false;
        while (n--) *op++ = *m++;
        return true;
    };
    auto extend = [&](size_t base) -> size_t {  // a run of zero bytes, then one
        size_t t = 0;
        while (ip < ie && *ip == 0) t += 255, ip++;
        if (ip >= ie) return SIZE_MAX;
        return t + base + *ip++;
    };
    if (ip >= ie) return false;
    int state = 0;
    if (*ip > 17) {
        const size_t t = *ip++ - 17u;
        if (!lits(t)) return false;
        state = t < 4 ? (int)t : 4;
    }
    for (;;) {
        if (ip >= ie) return false;
        size_t t = *ip++;
        const uint8_t* m;
        size_t len, next;
        if (t < 16) {
            if (state == 0) {
                if (t == 0 && (t = extend(15)) == SIZE_MAX) return false;
                if (!lits(t + 3)) return false;
                state = 4;
                continue;
            }
            if (ip >= ie) return false;
            next = t & 3;
            if (state != 4) m = op - 1 - (t >> 2) - ((size_t)*ip++ << 2), len = 2;
            else m = op - 0x801 - (t >> 2) - ((size_t)*ip++ << 2), len = 3;
        } else if (t >= 64) {
            if (ip >= ie) return false;
            next = t & 3;
            m = op - 1 - ((t >> 2) & 7) - ((size_t)*ip++ << 3);
            len = (t >> 5) + 1;
        } else if (t >= 32) {
            len = (t & 31) + 2;
            if (len == 2 && (len = extend(31 + 2)) == SIZE_MAX) return false;
            if (ie - ip < 2) return false;
            const size_t v = ip[0] | ip[1] << 8;
            ip += 2;
            m = op - 1 - (v >> 2), next = v & 3;
        } else {
            m = op - ((t & 8) << 11);
            len = (t & 7) + 2;
            if (len == 2 && (len = extend(7 + 2)) == SIZE_MAX) return false;
            if (ie - ip < 2) return false;
            const size_t v = ip[0] | ip[1] << 8;
            ip += 2;
            m -= v >> 2, next = v & 3;
            if (m == op) return op == oe;  // end of stream
            m -= 0x4000;
        }
        if (!copy(m, len) || !lits(next)) return false;
        state = (int)next;
    }
}

const uint8_t kMarker[8] = {0x33, 0xaa, 0xfb, 0x57, 0x99, 0xfa, 0x04, 0x10};

// A compressed section: marker, u16 version, u8 codec (0 LZO1X_1, 1
// LZO1X_999: one decoder), u16 x2, u16 chunk count, per chunk u16
// uncompressed and u16 compressed size, then per chunk a u32 checksum and the
// data (stored raw when the sizes match).
bool decode_section(const uint8_t* s, size_t n, std::vector<uint8_t>* out) {
    if (n < 17 || s[10] > 1) return false;
    uint16_t chunks;
    std::memcpy(&chunks, s + 15, 2);
    size_t data = 17 + 4 * (size_t)chunks;
    if (data > n) return false;
    out->clear();
    for (uint16_t k = 0; k < chunks; k++) {
        uint16_t u, c;
        std::memcpy(&u, s + 17 + 4 * k, 2);
        std::memcpy(&c, s + 17 + 4 * k + 2, 2);
        data += 4;
        if (c > u || data + c > n) return false;
        const size_t at = out->size();
        out->resize(at + u);
        if (c < u) {
            if (!lzo1x_decode(s + data, c, out->data() + at, u)) return false;
        } else {
            std::memcpy(out->data() + at, s + data, u);
        }
        data += c;
    }
    return true;
}

// The ARB programs in one .forge file.
void scan_forge(const std::string& path, std::vector<std::string>* found) {
    const int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 64) return (void)close(fd);
    const size_t n = (size_t)st.st_size;
    auto* base = (const uint8_t*)mmap(nullptr, n, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (base == MAP_FAILED) return;
    std::vector<uint8_t> buf;
    for (const uint8_t* p = base; (p = (const uint8_t*)memmem(p, n - (p - base), kMarker, 8)); p += 8) {
        if (!decode_section(p, n - (p - base), &buf)) continue;
        const std::string_view v((const char*)buf.data(), buf.size());
        for (size_t at = 0; (at = v.find("!!ARB", at)) != std::string_view::npos; at += 5) {
            if (v.compare(at, 10, "!!ARBvp1.0") != 0 && v.compare(at, 10, "!!ARBfp1.0") != 0) continue;
            const size_t end = v.find("\nEND", at);
            if (end == std::string_view::npos || end - at > 1 << 20) continue;
            found->emplace_back(v.substr(at, end + 4 - at));
        }
    }
    munmap((void*)base, n);
}

// ---- files ----

using Texts = std::unordered_map<uint64_t, std::string>;

// Reads [u64 hash][u32 n][bytes] records; *complete: a hash-0 record ended it.
void read_texts(const std::string& path, Texts* out, bool* complete = nullptr) {
    if (complete) *complete = false;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return;
    uint64_t h;
    uint32_t n;
    while (std::fread(&h, 8, 1, f) == 1 && std::fread(&n, 4, 1, f) == 1) {
        if (h == 0) {
            if (complete) *complete = true;
            break;
        }
        std::string s(n, '\0');
        if (std::fread(s.data(), 1, n, f) != n) break;
        (*out)[h] = std::move(s);
    }
    std::fclose(f);
}

void write_text(FILE* f, uint64_t h, const std::string& s) {
    const uint32_t n = (uint32_t)s.size();
    std::fwrite(&h, 8, 1, f), std::fwrite(&n, 4, 1, f), std::fwrite(s.data(), 1, n, f);
}

// ---- background compiles ----

struct Job {
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    std::atomic<bool> done{false};
    id obj = nil;
};

struct CacheState {
    std::string dir, vdir;
    std::mutex file_lock;
    FILE* programs = nullptr;
    FILE* pipelines = nullptr;
    std::unordered_set<uint64_t> texts_known, pipes_known;
    std::unordered_map<std::string, std::shared_ptr<Job>> jobs;  // render thread only
    double budget_left = 0;
    bool async = true;
    double budget_ms = 12;  // SCIMITAR_SHADER_WAIT_MS
};

CacheState& S() {
    static CacheState s;
    return s;
}

dispatch_queue_t compile_queue() {
    static dispatch_queue_t q = dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0);
    return q;
}

id<MTLFunction> compile_function(id<MTLDevice> dev, const std::string& msl, int alpha) {
    MTLCompileOptions* o = [MTLCompileOptions new];
    o.preserveInvariance = YES;
    NSError* err = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:@(msl.c_str()) options:o error:&err];
    if (!lib) {
        std::fprintf(stderr, "metal: MSL does not compile: %s\n%s\n", err.localizedDescription.UTF8String, msl.c_str());
        return nil;
    }
    if (alpha < 0) return [lib newFunctionWithName:@"arb_main"];
    MTLFunctionConstantValues* fc = [MTLFunctionConstantValues new];
    if (alpha < 8) {
        const uint32_t a = (uint32_t)alpha;
        [fc setConstantValue:&a type:MTLDataTypeUInt atIndex:arb::kFcAlphaFunc];
    }
    id<MTLFunction> f = [lib newFunctionWithName:@"arb_main" constantValues:fc error:&err];
    if (!f) std::fprintf(stderr, "metal: fragment function: %s\n", err.localizedDescription.UTF8String);
    return f;
}

// Starts `work` for `key` unless running; waits within the frame's budget.
id run_job(const std::string& key, id (^work)(void), bool* pending, double* waited) {
    CacheState& s = S();
    *pending = false, *waited = 0;
    auto& slot = s.jobs[key];
    if (!slot) {
        auto job = std::make_shared<Job>();
        slot = job;
        dispatch_async(compile_queue(), ^{
            @autoreleasepool {
                job->obj = work();
            }
            job->done = true;
            dispatch_semaphore_signal(job->sem);
        });
    }
    std::shared_ptr<Job> job = slot;
    if (!job->done) {
        const auto t0 = std::chrono::steady_clock::now();
        const double budget = s.async ? s.budget_left : 1e12;
        if (budget > 0)
            dispatch_semaphore_wait(job->sem, s.async ? dispatch_time(DISPATCH_TIME_NOW, (int64_t)(budget * 1e6))
                                                      : DISPATCH_TIME_FOREVER);
        *waited = ms_since(t0);
        s.budget_left -= *waited;
        if (!job->done) {
            *pending = true;
            return nil;
        }
    }
    id obj = job->obj;
    s.jobs.erase(key);
    return obj;
}

uint64_t translator_version() {
    static const uint64_t v = [] {
        std::string k = arb::prelude();
        k += arb::translate("!!ARBfp1.0\nTEMP r0;\nTEX r0, fragment.texcoord[0], texture[0], 2D;\n"
                            "MUL result.color, r0, program.env[0];\nEND\n").msl;
        k += arb::translate("!!ARBvp1.0\nDP4 result.position.x, program.env[0], vertex.position;\n"
                            "MOV result.texcoord[0], vertex.attrib[8];\nEND\n").msl;
        k += std::to_string(kFormat) + "/" + std::to_string(sizeof(Record));
        return text_hash(k.data(), k.size());
    }();
    return v;
}

}  // namespace

uint64_t text_hash(const void* p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t k = 0; k < n; k++) h = (h ^ ((const uint8_t*)p)[k]) * 1099511628211ull;
    return h ? h : 1;  // 0 ends a texts file
}

ShaderCache& shader_cache() {
    static ShaderCache c;
    return c;
}

void ShaderCache::new_frame() {
    CacheState& s = S();
    if (s.budget_left <= 0 && !s.jobs.empty()) n_skipped_frames++;
    s.budget_left = s.budget_ms;
    static uint64_t frames, told;
    if (++frames % 1000 == 0 && n_background != told) {
        told = n_background;
        std::fprintf(stderr, "shaders: %llu background compiles, %llu frames waited out the budget\n",
                     (unsigned long long)n_background, (unsigned long long)n_skipped_frames);
    }
}

id<MTLFunction> ShaderCache::function(id<MTLDevice> dev, const std::string& msl, int alpha, bool* pending,
                                      double* waited) {
    std::string key = "F";
    key += (char)alpha;
    key += msl;
    std::string copy = msl;
    if (!S().jobs.count(key)) n_background++;
    return run_job(key, ^id { return compile_function(dev, copy, alpha); }, pending, waited);
}

id<MTLRenderPipelineState> ShaderCache::pipeline(id<MTLDevice> dev, const std::string& key,
                                                 MTLRenderPipelineDescriptor* pd, bool* pending, double* waited) {
    if (!S().jobs.count("P" + key)) n_background++;
    return run_job("P" + key, ^id {
        NSError* err = nil;
        id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:pd error:&err];
        if (!ps) std::fprintf(stderr, "metal: pipeline: %s\n", err.localizedDescription.UTF8String);
        return ps;
    }, pending, waited);
}

void ShaderCache::record(const std::string& vtext, const std::string& ftext, const uint32_t vopts[3], int alpha,
                         const PipeDesc& d) {
    CacheState& s = S();
    if (!s.pipelines) return;
    Record r;
    std::memset(&r, 0, sizeof r);
    r.vtext = text_hash(vtext.data(), vtext.size());
    r.ftext = text_hash(ftext.data(), ftext.size());
    std::memcpy(r.vopts, vopts, sizeof r.vopts);
    r.alpha = alpha;
    r.d = d;
    if (!s.pipes_known.insert(text_hash(&r, sizeof r)).second) return;
    std::lock_guard<std::mutex> l(s.file_lock);
    for (const auto* t : {&vtext, &ftext}) {
        const uint64_t h = text_hash(t->data(), t->size());
        if (s.texts_known.insert(h).second) write_text(s.programs, h, *t);
    }
    std::fflush(s.programs);
    std::fwrite(&r, sizeof r, 1, s.pipelines);
    std::fflush(s.pipelines);
}

namespace {
// The startup precompile's work and results (outlives startup() while the
// archives' programs compile in the background).
struct Precompile {
    struct Item {
        uint64_t id;
        const std::string* text;  // a forge program
        const Record* rec;        // a recorded pipeline
    };
    std::vector<Item> items;
    Texts forge, texts;
    std::string done_path;
    std::vector<Record> records;
    std::mutex lock;
    std::vector<uint64_t> done_ids;
    std::atomic<size_t> finished{0}, archived{0};
    void flush() {  // under lock
        if (done_ids.empty()) return;
        if (FILE* f = std::fopen(done_path.c_str(), "ab")) {
            std::fwrite(done_ids.data(), 8, done_ids.size(), f);
            std::fclose(f);
        }
        done_ids.clear();
    }
};
}  // namespace

void ShaderCache::startup(id<MTLDevice> dev, void (*draw)(size_t, size_t, void*), void* ctx, const char* resources) {
    CacheState& s = S();
    // Again in one process (the launcher's precompile, then the game's): start over.
    if (s.programs) std::fclose(s.programs), s.programs = nullptr;
    if (s.pipelines) std::fclose(s.pipelines), s.pipelines = nullptr;
    s.texts_known.clear(), s.pipes_known.clear();
    s.async = !env_off("SCIMITAR_SHADER_ASYNC");
    if (const char* v = std::getenv("SCIMITAR_SHADER_WAIT_MS")) s.budget_ms = std::atof(v);
    s.budget_left = s.budget_ms;
    if (env_off("SCIMITAR_SHADER_CACHE_ON")) return;
    if (const char* v = std::getenv("SCIMITAR_SHADER_CACHE")) s.dir = v;
    else if (const char* h = std::getenv("HOME"))
        s.dir = std::string(h) + "/Library/Caches/scimitar-recomp/" + getprogname() + "/shaders";
    else return;
    char vname[32];
    std::snprintf(vname, sizeof vname, "/v%016llx", (unsigned long long)translator_version());
    s.vdir = s.dir + vname;
    mkdirs(s.vdir);
    const auto t0 = std::chrono::steady_clock::now();

    // What the game used before.
    Texts texts;
    read_texts(s.dir + "/programs.bin", &texts);
    for (auto& [h, t] : texts) s.texts_known.insert(h);
    std::vector<Record> records;
    {
        const std::string path = s.dir + "/pipelines.bin";
        bool fresh = true;
        if (FILE* f = std::fopen(path.c_str(), "rb")) {
            char magic[4];
            uint32_t size = 0;
            if (std::fread(magic, 4, 1, f) == 1 && std::fread(&size, 4, 1, f) == 1 && !std::memcmp(magic, "SCPL", 4) &&
                size == sizeof(Record)) {
                fresh = false;
                Record r;
                while (std::fread(&r, sizeof r, 1, f) == 1)
                    if (s.pipes_known.insert(text_hash(&r, sizeof r)).second) records.push_back(r);
            }
            std::fclose(f);
        }
        s.pipelines = std::fopen(path.c_str(), fresh ? "wb" : "ab");
        if (s.pipelines && fresh) {
            const uint32_t size = sizeof(Record);
            std::fwrite("SCPL", 4, 1, s.pipelines), std::fwrite(&size, 4, 1, s.pipelines);
        }
        s.programs = std::fopen((s.dir + "/programs.bin").c_str(), "ab");
    }

    // The game's own programs, scanned once from its .forge archives.
    Texts forge;
#if TARGET_OS_IPHONE
    const bool scan = std::getenv("SCIMITAR_PRECOMPILE") && !env_off("SCIMITAR_PRECOMPILE_FORGE");
#else
    const bool scan = !env_off("SCIMITAR_PRECOMPILE_FORGE");
#endif
    if (scan) {
        bool complete = false;
        read_texts(s.dir + "/forge.bin", &forge, &complete);
        if (!complete && (resources || !process_arguments().empty())) {
            forge.clear();
            std::string res;
            if (resources) {
                res = resources;
            } else {
                res = process_arguments()[0];
                res = res.substr(0, res.rfind('/'));
                res = res.substr(0, res.rfind('/')) + "/Resources";
            }
            std::vector<std::string> files;
            // A relative app path no longer holds once the game has changed
            // directory (to its Resources): then the current directory.
            char cwd[4096];
            for (const std::string& dir : {res, std::string(getcwd(cwd, sizeof cwd) ? cwd : ".")}) {
                if (NSArray* names = [NSFileManager.defaultManager contentsOfDirectoryAtPath:@(dir.c_str()) error:nil])
                    for (NSString* n in names)
                        if ([n hasSuffix:@".forge"]) files.push_back(dir + "/" + n.UTF8String);
                if (!files.empty()) break;
            }
            std::vector<std::vector<std::string>> found(files.size());
            const std::string* fp = files.data();
            std::vector<std::string>* outp = found.data();
            dispatch_apply(files.size(), compile_queue(), ^(size_t k) { scan_forge(fp[k], &outp[k]); });
            for (auto& v : found)
                for (auto& t : v) forge.emplace(text_hash(t.data(), t.size()), std::move(t));
            if (FILE* f = files.empty() ? nullptr : std::fopen((s.dir + "/forge.bin").c_str(), "wb")) {
                for (auto& [h, t] : forge) write_text(f, h, t);
                write_text(f, 0, "");
                std::fclose(f);
            }
            std::fprintf(stderr, "shaders: %zu programs in %zu .forge files (%.0f ms)\n", forge.size(), files.size(),
                         ms_since(t0));
        }
    }

    // What this translator version has compiled already.
    std::unordered_set<uint64_t> done;
    const std::string done_path = s.vdir + "/done.bin";
    if (FILE* f = std::fopen(done_path.c_str(), "rb")) {
        uint64_t h;
        while (std::fread(&h, 8, 1, f) == 1) done.insert(h);
        std::fclose(f);
    }

    // The binary archive.
    const std::string archive_path = s.vdir + "/archive.bin";
    NSURL* archive_url = [NSURL fileURLWithPath:@(archive_path.c_str())];
    {
        MTLBinaryArchiveDescriptor* ad = [MTLBinaryArchiveDescriptor new];
        if (access(archive_path.c_str(), R_OK) == 0) ad.url = archive_url;
        NSError* err = nil;
        archive_ = [dev newBinaryArchiveWithDescriptor:ad error:&err];
        if (!archive_ && ad.url) {  // unreadable: start over
            unlink(archive_path.c_str()), unlink(done_path.c_str()), done.clear();
            ad.url = nil;
            archive_ = [dev newBinaryArchiveWithDescriptor:ad error:&err];
        }
    }

    using Item = Precompile::Item;
    std::vector<Item> items;
    for (const Record& r : records) {
        const uint64_t id = text_hash(&r, sizeof r);
        if (!done.count(id) && texts.count(r.vtext) && texts.count(r.ftext)) items.push_back({id, nullptr, &r});
    }
    // Recorded pipelines first: they warm the programs the game is known to use.
    for (auto& [h, t] : forge)
        if (!done.count(h ^ 0x5f0f5f0f5f0f5f0full)) items.push_back({h ^ 0x5f0f5f0f5f0f5f0full, &t, nullptr});
    if (env_off("SCIMITAR_PRECOMPILE") || items.empty()) {
        std::fprintf(stderr, "shaders: %zu recorded pipelines, nothing to precompile\n", records.size());
        return;
    }
    // Before the first frame, behind the progress bar. Work done is saved as
    // it goes: a launch quit midway resumes where it stopped.
    // SCIMITAR_PRECOMPILE: unset or "full", everything behind the bar;
    // "background", the archives' programs after the first frame on two
    // low-priority threads (measured: it still costs the game frames, see
    // docs/METAL.md); "recorded", the archives' programs not at all.
    const char* mode = std::getenv("SCIMITAR_PRECOMPILE");
#if TARGET_OS_IPHONE
    // On iOS, recorded pipelines only by default: the archives' 25-30k
    // programs would hold a phone's launch for many minutes (not measured).
    if (!mode) mode = "recorded";
#endif
    const bool background = mode && !std::strcmp(mode, "background");
    if (mode && !std::strcmp(mode, "recorded"))
        while (!items.empty() && items.back().text) items.pop_back();
    size_t blocking = 0;
    while (blocking < items.size() && (!background || items[blocking].rec)) blocking++;
    std::fprintf(stderr, "shaders: precompiling %zu items now (%zu recorded pipelines), %zu forge programs in the background\n",
                 blocking, records.size(), items.size() - blocking);

    auto* shared = new Precompile{std::move(items), std::move(forge), std::move(texts), done_path, std::move(records)};
    // (moving keeps the elements in place: the items' pointers stay valid)
    Precompile* pc = shared;
    id<MTLBinaryArchive> archive = archive_;
    // Compiles items [from, to) on all cores at `qos`; returns when done.
    // Compiles items [from, to) at `qos` on `width` threads (0: all cores).
    auto run = [pc, dev, archive](size_t from, size_t to, dispatch_qos_class_t qos, size_t width = 0) {
        const size_t lanes = width ? width : to - from;
        dispatch_apply(lanes, dispatch_get_global_queue(qos, 0), ^(size_t lane) {
          for (size_t i = lane; i < to - from; i += lanes) {
            @autoreleasepool {
                Precompile::Item& item = pc->items[from + i];
                bool good = false;
                if (item.text) {
                    const arb::Program p = arb::translate(*item.text);
                    good = !p.ok || compile_function(dev, p.msl, p.stage == arb::Stage::Vertex ? -1 : 8) != nil;
                } else {
                    const Record& r = *item.rec;
                    arb::Options o;
                    o.extra_varyings = r.vopts[0], o.uint_attribs = r.vopts[1], o.int_attribs = r.vopts[2];
                    const arb::Program v = arb::translate(pc->texts.at(r.vtext), o);
                    const arb::Program f = arb::translate(pc->texts.at(r.ftext));
                    id<MTLFunction> vfn = v.ok ? compile_function(dev, v.msl, -1) : nil;
                    id<MTLFunction> ffn = f.ok ? compile_function(dev, f.msl, r.alpha) : nil;
                    if (vfn && ffn) {
                        MTLRenderPipelineDescriptor* pd = pipeline_descriptor(vfn, ffn, r.d, f.color_outputs);
                        NSError* err = nil;
                        // One at a time: the archive is not documented as thread-safe.
                        std::lock_guard<std::mutex> l(pc->lock);
                        good = [archive addRenderPipelineFunctionsWithDescriptor:pd error:&err];
                        if (good) pc->archived++;
                    }
                }
                if (good) {
                    std::lock_guard<std::mutex> l(pc->lock);
                    pc->done_ids.push_back(item.id);
                    if (pc->done_ids.size() >= 256) pc->flush();
                }
                pc->finished++;
            }
          }
        });
        std::lock_guard<std::mutex> l(pc->lock);
        pc->flush();
    };

    if (blocking) {
        std::atomic<bool> all_done_v{false};
        std::atomic<bool>* all_done = &all_done_v;
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            run(0, blocking, QOS_CLASS_USER_INITIATED);
            *all_done = true;
        });
        size_t last = SIZE_MAX;
        auto told = std::chrono::steady_clock::now();
        const bool main_thread = pthread_main_np();
        while (!*all_done) {
            const size_t now = pc->finished.load();
            if (now != last) {
                if (draw) draw(now, blocking, ctx);
                last = now;
            }
            // The main thread keeps its run loop turning (the window draws);
            // another thread just waits.
            if (main_thread) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.016, true);
            else usleep(16000);
            if (ms_since(told) > 2000) {
                std::fprintf(stderr, "shaders: %zu/%zu\n", pc->finished.load(), blocking);
                told = std::chrono::steady_clock::now();
            }
        }
        if (draw) draw(blocking, blocking, ctx);
        if (pc->archived) {
            NSError* err = nil;
            if (![archive_ serializeToURL:archive_url error:&err])
                std::fprintf(stderr, "shaders: archive not saved: %s\n", err.localizedDescription.UTF8String);
        }
        std::fprintf(stderr, "shaders: precompiled %zu items in %.1f s (%zu into the archive)\n", blocking,
                     ms_since(t0) / 1000, pc->archived.load());
    }
    if (blocking < pc->items.size()) {
        const size_t n = pc->items.size();
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
            const auto t1 = std::chrono::steady_clock::now();
            // Two threads: the game keeps the other cores (on all of them it stutters).
            static const size_t width = std::getenv("SCIMITAR_PRECOMPILE_THREADS") ? std::atoi(std::getenv("SCIMITAR_PRECOMPILE_THREADS")) : 2;
            run(blocking, n, QOS_CLASS_BACKGROUND, width);
            std::fprintf(stderr, "shaders: %zu forge programs compiled in the background in %.1f s\n", n - blocking,
                         ms_since(t1) / 1000);
        });  // `pc` stays: the background work outlives this call
    } else {
        delete pc;
    }
}

}  // namespace scimitar::metal

// The launcher's "Compiling shaders" step (launcher.h): the precompile with a
// device of its own, before the game starts. Blocks; progress from the calling thread.
extern "C" __attribute__((visibility("default"))) long scimitar_shader_precompile(
    const char* game_app, void (*progress)(long done, long total, void* ctx), void* ctx) {
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev) return -1;
        struct Relay {
            void (*progress)(long, long, void*);
            void* ctx;
        } relay = {progress, ctx};
        const std::string res = game_app ? std::string(game_app) + "/Contents/Resources" : std::string();
        scimitar::metal::shader_cache().startup(
            dev,
            [](size_t done, size_t total, void* r) {
                auto* x = static_cast<Relay*>(r);
                if (x->progress) x->progress((long)done, (long)total, x->ctx);
            },
            &relay, game_app ? res.c_str() : nullptr);
        return 0;
    }
}
