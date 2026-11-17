// Assassin's Creed Revelations, the Windows release recompiled
// (docs/ACR_WINDOWS.md): what the Windows loader would do for ACRSP.exe,
// natively, with the Windows platform layer AC1 brought (platform/win32).
// Loads its sections into guest memory, fills its import address table, sets
// up the main thread's TEB and static TLS on a guest stack, and runs the entry
// point (the C runtime's WinMainCRTStartup, which ends the process through
// ExitProcess).
//
// Revelations creates its window and pumps its messages on an engine thread,
// not on its main thread, and AppKit only works on the host main thread. So
// the guest main thread runs on a host thread of its own, and the host main
// thread runs the run loop, which serves the AppKit work USER32 sends it
// (platform/win32/user32.mm, on_main).
//
//   acr [PATH/ACRSP.exe] [game options...]
//
// The executable defaults to $ACR_EXE (the .exe or its folder), else the Steam
// folder next to the repository (game.cpp). Started from its app (ACR.app),
// the launcher shows first and sets them. The working directory becomes the
// game's folder.
#include <CoreFoundation/CoreFoundation.h>
#include <pthread.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "launcher/launch.h"
#include "scimitar/guest_heap.h"
#include "scimitar/image_loader.h"
#include "scimitar/orbit.h"
#include "scimitar/runtime.h"
#include "win32.h"

using namespace scimitar;

int main(int argc, char** argv) {
    using game::kInfo;
#ifdef SCIMITAR_LAUNCHER_DEFAULT_APP
    // Started from its app: the launcher first, which sets $ACR_EXE and the
    // switches from its settings (games/common/launcher).
    if (!game::run_launcher(argc, argv, SCIMITAR_LAUNCHER_DEFAULT_APP)) return 0;
#endif
    const char* where = std::getenv(kInfo.app_env) ? std::getenv(kInfo.app_env) : kInfo.default_app;
    std::string exe = game::find_game_file(where);
    int first_option = 1;
    if (argc > 1 && std::string(argv[1]).size() > 4 &&
        std::string(argv[1]).compare(std::string(argv[1]).size() - 4, 4, ".exe") == 0)
        exe = argv[1], first_option = 2;
    if (exe.empty() || access(exe.c_str(), R_OK) != 0) {
        std::fprintf(stderr, "acr: cannot read the game at %s (pass the executable, or set %s to it or its folder)\n",
                     exe.empty() ? where : exe.c_str(), kInfo.app_env);
        return 2;
    }
    char real[4096];
    if (realpath(exe.c_str(), real)) exe = real;

    // Saves (through the Orbit client) beside the Mac ports' own, as they do.
    if (!std::getenv("SCIMITAR_SAVE_DIR") && std::getenv("HOME"))
        orbit::set_save_directory(std::string(std::getenv("HOME")) +
                                  "/Library/Application Support/scimitar-recomp/acr/saves");

    // One processor (GetSystemInfo, GetProcessAffinityMask). With two or more
    // the menu background blinks: the game's frame data alternates between two
    // projections (docs/ACR_WINDOWS.md). With one it is steady, and gameplay
    // runs at about the same frame rate. SCIMITAR_WIN32_CPUS=4 restores four.
    setenv("SCIMITAR_WIN32_CPUS", "1", 0);

    GuestMemory mem;
    LoadedImage image = load_image(mem, exe);
    if (!image.image->is_pe()) fatal("%s is not a Windows executable", exe.c_str());
    std::vector<std::string> args{exe};
    for (int k = first_option; k < argc; k++) args.push_back(argv[k]);
    win32::load_process(mem, *image.image, exe, args, "acr");
    if (chdir(win32::process().host_dir.c_str()) != 0)
        fatal("cannot enter %s", win32::process().host_dir.c_str());

    // The guest main thread: 16 MB of guest stack (the executable asks for
    // 1 MB), and a large host stack for the translated frames.
    struct Start {
        GuestMemory* mem;
        uint32_t entry;
    };
    static Start start{&mem, image.image->entry()};
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 512u << 20);
    pthread_t thread;
    pthread_create(&thread, &attr, [](void*) -> void* {
        constexpr uint32_t kStack = 16u << 20;
        const uint32_t stack = heap::alloc(*start.mem, kStack, 16);
        Context c;
        c.esp = stack + kStack - 64;
        c.fpu.cw = 0x027f;  // as Windows starts a thread: 53-bit precision
        win32::attach_thread(c, stack + kStack, stack);
        call_guest(c, start.entry, {});
        std::fprintf(stderr, "acr: the entry point returned\n");
        std::exit(0);
    }, nullptr);
    // ExitProcess ends the process from the game's threads.
    for (;;) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1.0, false);
}
