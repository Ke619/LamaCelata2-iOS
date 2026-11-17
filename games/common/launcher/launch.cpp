// Starting a port as an app (launch.h).
#include "launch.h"

#include <mach-o/dyld.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "launcher.h"

namespace game {

bool run_launcher(int argc, char** argv, const char* default_app) {
    char path[4096];
    uint32_t size = sizeof path;
    if (argc > 1 || _NSGetExecutablePath(path, &size) != 0 || !std::strstr(path, ".app/Contents/MacOS/")) return true;
    if (std::getenv("SCIMITAR_NO_LAUNCHER") || std::getenv("SCIMITAR_HIDDEN")) return true;
    const int play = scimitar_launcher_run(kInfo.id, kInfo.title, kInfo.app_env, kInfo.app_name, default_app,
                                           kInfo.game_file, kInfo.launcher_settings ? kInfo.launcher_settings : "");
    if (!play) return false;
    if (play == 2) {
        // The launcher's window was up: the game starts in a fresh image of
        // this process (its choices are in the environment), so that nothing
        // the launcher set up in AppKit (its application, window and modal
        // session) is in the game's way. After it, the game's full-screen
        // window stayed black until the application was activated again.
        setenv("SCIMITAR_NO_LAUNCHER", "1", 1);
        execv(path, argv);
        std::fprintf(stderr, "%s: cannot restart for the game (%s); starting it here\n", kInfo.id,
                     std::strerror(errno));
    }
    return true;
}

std::string find_game_file(const std::string& location) {
    struct stat st;
    if (stat(location.c_str(), &st) != 0) return "";
    if (!S_ISDIR(st.st_mode)) return location;
    for (const char* p = kInfo.game_file; *p;) {
        const char* e = std::strchr(p, '|');
        if (!e) e = p + std::strlen(p);
        const std::string file = location + "/" + std::string(p, e);
        if (access(file.c_str(), R_OK) == 0) return file;
        p = *e ? e + 1 : e;
    }
    return "";
}

}  // namespace game
