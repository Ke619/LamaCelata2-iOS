// Prince of Persia (2008), the Windows release recompiled (docs/POP_WINDOWS.md):
// the Windows executable inside the Mac release (TransGaming Cider wraps it),
// with the Windows platform layer of the AC1 and Revelations ports
// (platform/win32). Loads its sections into guest memory, fills its import
// address table, sets up the main thread's TEB and static TLS on a guest
// stack, and runs the entry point (the C runtime's WinMainCRTStartup, which
// ends the process through ExitProcess).
//
//   pop [PATH/Prince of Persia.exe] [game options...]
//
// The executable defaults to $POP_EXE (the .exe, its folder, or the Mac
// release's .app holding it), else the Cider bundle next to the repository
// (game.cpp). Started from its app (PoP.app), the launcher shows first and
// sets them. The working directory becomes the game's folder.
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <pthread.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "launcher/launch.h"
#include "scimitar/guest_heap.h"
#include "scimitar/image_loader.h"
#include "scimitar/runtime.h"
#include "win32.h"

using namespace scimitar;

int main(int argc, char** argv) {
    using game::kInfo;
#ifdef SCIMITAR_LAUNCHER_DEFAULT_APP
    // Started from its app: the launcher first, which sets $POP_EXE and the
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
        std::fprintf(stderr, "pop: cannot read the game at %s (pass the executable, or set %s to it or its folder)\n",
                     exe.empty() ? where : exe.c_str(), kInfo.app_env);
        return 2;
    }
    char real[4096];
    if (realpath(exe.c_str(), real)) exe = real;

    GuestMemory mem;
    LoadedImage image = load_image(mem, exe);
    if (!image.image->is_pe()) fatal("%s is not a Windows executable", exe.c_str());
    std::vector<std::string> args{exe};
    for (int k = first_option; k < argc; k++) args.push_back(argv[k]);
    win32::load_process(mem, *image.image, exe, args, "pop");
    if (chdir(win32::process().host_dir.c_str()) != 0)
        fatal("cannot enter %s", win32::process().host_dir.c_str());

    // The game starts at 800x600 unless its options (kept in the registry)
    // say otherwise: the main display's size instead, as Direct3D lists it.
    const std::string engine = "HKCU\\Software\\Ubisoft\\Prince of Persia\\1.0\\Engine";
    win32::registry_default_dword(engine, "ScreenResolutionWidth", (uint32_t)CGDisplayPixelsWide(CGMainDisplayID()));
    win32::registry_default_dword(engine, "ScreenResolutionHeight", (uint32_t)CGDisplayPixelsHigh(CGMainDisplayID()));

    // The game creates its window and pumps its messages on an engine thread
    // while its main thread waits, and AppKit only works on the host main
    // thread. So the guest main thread runs on a host thread of its own (with
    // a large host stack for the translated frames), and the host main thread
    // runs the run loop, which serves the AppKit work USER32 sends it
    // (platform/win32/user32.mm, on_main). ExitProcess ends the process.
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
        // 16 MB of guest stack (the executable asks for 1 MB).
        constexpr uint32_t kStack = 16u << 20;
        const uint32_t stack = heap::alloc(*start.mem, kStack, 16);
        Context c;
        c.esp = stack + kStack - 64;
        c.fpu.cw = 0x027f;  // as Windows starts a thread: 53-bit precision
        win32::attach_thread(c, stack + kStack, stack);
        call_guest(c, start.entry, {});
        std::fprintf(stderr, "pop: the entry point returned\n");
        std::exit(0);
    }, nullptr);
    for (;;) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1.0, false);
}
