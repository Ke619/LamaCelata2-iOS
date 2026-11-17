// The port's entry point, the same for every game: what dyld and crt1's start
// would do for the original executable, natively. Loads the game's segments
// into guest memory, runs its static initializers in order, then calls its
// main(argc, argv, envp, apple) on a guest stack, and exits with its result.
//
//   ac2 [GAME.app] [game options...]      (or acb, ...: games/<id>/game.cpp)
//
// GAME.app defaults to the game's default_app (or $<app_env>, e.g. AC2_APP); on
// iOS to the .app in the app's Documents folder, where the shell
// (shell_ios.mm) runs this on a thread of its own. Started as an app (from its
// .app bundle on the Mac, always on iOS), the port first shows the launcher
// (launcher/Launcher.swift), which sets the game's location and the port's
// environment switches from its settings. Imports the platform layer
// does not implement yet stop the run with their name, caller and a guest
// backtrace.
//
// Game options (-godmode, -world Firenze, ...; docs/AC2_DEV_OPTIONS.md) are
// parsed with the engine's own InitializeCommandLineOptions, which the retail
// Mac builds have but never call.

#include <TargetConditionals.h>
#include <crt_externs.h>
#include <unistd.h>

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

#include "game.h"
#include "scimitar/arb_store.h"
#include "scimitar/guest_heap.h"
#include "scimitar/image_loader.h"
#include "scimitar/imports.h"
#include "scimitar/orbit.h"
#include "scimitar/runtime.h"

using namespace scimitar;

namespace {

constexpr uint32_t kMainStackSize = 16u << 20;

uint32_t guest_string(GuestMemory& m, const std::string& s) {
    const uint32_t a = heap::alloc(m, (uint32_t)s.size() + 1);
    std::memcpy(m.ptr(a), s.c_str(), s.size() + 1);
    return a;
}

uint32_t guest_vector(GuestMemory& m, const std::vector<std::string>& v) {
    const uint32_t a = heap::alloc(m, 4 * ((uint32_t)v.size() + 1));
    for (size_t k = 0; k < v.size(); k++) m.write<uint32_t>(a + 4 * (uint32_t)k, guest_string(m, v[k]));
    m.write<uint32_t>(a + 4 * (uint32_t)v.size(), 0);
    return a;
}

}  // namespace

void game::log(const char* fmt, ...) {
    char line[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    std::fprintf(stderr, "%s: %s", kInfo.id, line);
#if TARGET_OS_IPHONE
    // On a phone with no Xcode attached the lines go nowhere: they are also written to <id>-log.txt in the
    // app's Documents folder (Files, On My iPhone, the port's folder), a new file for each run.
    static std::mutex lock;
    static FILE* const file = [] {
        const char* home = std::getenv("HOME");
        return home ? std::fopen((std::string(home) + "/Documents/" + kInfo.id + "-log.txt").c_str(), "w") : nullptr;
    }();
    static size_t written = 0;
    std::lock_guard<std::mutex> hold(lock);
    if (file && written < (1u << 20)) {  // about a megabyte at most
        written += std::fprintf(file, "%s", line);
        std::fflush(file);
    }
#endif
}

namespace {

// Gear's allocator, for the ARB program store: shrinks a block in place, as
// MemAllocDl283::Realloc does when it can (and with the allocator's lock).
bool gear_shrink(Context& c, uint32_t ptr, uint32_t size) {
    static const uint32_t marker = guest_symbol_or_0("__ZN4Gear13MemPageMarker4pRefE"),
                          owner = guest_symbol_or_0("__ZNK4Gear13MemPageMarker20GetAllocatorFromDataEPKv"),
                          dl_realloc = guest_symbol_or_0("__ZN4Gear13MemAllocDl28315InternalReallocEPvj"),
                          inplace = guest_symbol_or_0("__ZN4Gear13MemAllocDl28324internal_inplace_reallocEPNS0_12malloc_stateEPvm");
    if (!marker || !owner || !dl_realloc || !inplace || !c.mem->read<uint32_t>(marker)) return false;
    const uint32_t allocator = call_guest(c, owner, {c.mem->read<uint32_t>(marker), ptr});
    // A dlmalloc allocator (MemAllocDl283 or one built on it, by its Realloc).
    if (!allocator || c.mem->read<uint32_t>(c.mem->read<uint32_t>(allocator) + 16) != dl_realloc) return false;
    return call_guest(c, inplace, {allocator, allocator + 0x30, ptr, size}) == ptr;  // its malloc_state at +0x30
}

}  // namespace

#if TARGET_OS_IPHONE
int game::game_main(int argc, char** argv) {
    const std::string default_app = std::string(std::getenv("HOME")) + "/Documents/" + kInfo.app_name;
#else
#ifdef SCIMITAR_LAUNCHER
#include "launcher/launch.h"
#endif

int main(int argc, char** argv) {
#ifdef SCIMITAR_LAUNCHER
    if (!game::run_launcher(argc, argv, SCIMITAR_LAUNCHER_DEFAULT_APP)) return 0;
#endif
    return game::game_main(argc, argv);
}

int game::game_main(int argc, char** argv) {
    const std::string default_app = kInfo.default_app;
#endif
    using game::kInfo;
    std::string app = std::getenv(kInfo.app_env) ? std::getenv(kInfo.app_env) : default_app;
    int first_option = 1;
    if (argc > 1 && std::string(argv[1]).find(".app") != std::string::npos) app = argv[1], first_option = 2;
    const std::string binary = app + "/Contents/MacOS/scimitar_sf";
    if (access(binary.c_str(), R_OK) != 0) {
        game::log("cannot read %s (pass the game's .app or set %s)\n", binary.c_str(), kInfo.app_env);
        return 2;
    }
    // Each game's saves in a folder of its own (SCIMITAR_SAVE_DIR wins).
    if (!std::getenv("SCIMITAR_SAVE_DIR") && std::getenv("HOME"))
        orbit::set_save_directory(std::string(std::getenv("HOME")) +
                                  "/Library/Application Support/scimitar-recomp/" + kInfo.saves);

    GuestMemory mem;
    LoadedImage image = load_image(mem, binary);
    bind_imports(mem, image);
    arb_store::set_shrink(gear_shrink);

    // The process as the game sees it.
    std::vector<std::string> args = {binary};
    for (int k = first_option; k < argc; k++) args.push_back(argv[k]);
    // SCIMITAR_GAME_OPTIONS="/a /b:c": more game options (the launcher's), split at spaces.
    if (const char* more = std::getenv("SCIMITAR_GAME_OPTIONS"))
        for (const char* p = more; *p;) {
            while (*p == ' ') p++;
            const char* e = p;
            while (*e && *e != ' ') e++;
            if (e > p) args.emplace_back(p, e);
            p = e;
        }
    set_process_arguments(args);
    std::vector<std::string> env;
    for (char** e = *_NSGetEnviron(); *e; e++) env.push_back(*e);
    const uint32_t g_argv = guest_vector(mem, args), g_envp = guest_vector(mem, env);
    const uint32_t g_apple = guest_vector(mem, {"executable_path=" + binary});

    // The main thread (about 20 KB of its stack used in Florence: 1 MB in a small window).
    const uint32_t main_stack = guest_layout() == GuestLayout::Window ? 1u << 20 : kMainStackSize;
    const uint32_t stack = heap::alloc(mem, main_stack, 16);
    Context c;
    c.mem = &mem;
    c.esp = ((stack + main_stack) & ~15u) - 12;
    set_thread_context(&c);

    if (kInfo.start) kInfo.start();
    game::install_video_fit();

    // Static initializers (__DATA,__mod_init_func), in order, as dyld runs them.
    const recomp::Image& img = *image.image;
    size_t inits = 0;
    for (const recomp::Section& s : img.sections())
        if (s.type() == 9)  // S_MOD_INIT_FUNC_POINTERS
            for (uint32_t a = s.addr; a < s.addr + s.size; a += 4) {
                call_guest(c, mem.read<uint32_t>(a), {(uint32_t)args.size(), g_argv, g_envp, g_apple});
                inits++;
            }
    game::log("%zu static initializers ran\n", inits);

    // The engine's option parser takes the options as one string.
    if (args.size() > 1) {
        std::string line;
        for (size_t k = 1; k < args.size(); k++) line += (k > 1 ? " " : "") + args[k];
        // void scimitar::InitializeCommandLineOptions(char*)
        call_guest(c, guest_symbol("__ZN8scimitar28InitializeCommandLineOptionsEPc"), {guest_string(mem, line)});
        game::log("game options: %s\n", line.c_str());
    }

    const uint32_t main_fn = img.symbol("_main");
    if (!main_fn) fatal("no _main in %s", binary.c_str());
    const uint32_t r = call_guest(c, main_fn, {(uint32_t)args.size(), g_argv, g_envp, g_apple});
    game::log("main returned %d\n", (int)r);
    return (int)r;
}
