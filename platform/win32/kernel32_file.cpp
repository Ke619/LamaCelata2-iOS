// KERNEL32, part 3: files and directories (host files under the game's
// folder, paths through host_path), standard handles, and INI files.
#include <dirent.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>

#include "win32.h"

namespace scimitar::win32 {

namespace {

struct File : Object {
    int fd = -1;
    bool console = false;
    std::string path;
    std::mutex lock;  // the file position is shared
    ~File() override {
        if (fd > 2) ::close(fd);
    }
};

struct Find : Object {
    std::vector<std::string> names;
    std::string dir;
    size_t next = 0;
};

uint32_t error_from_errno(int e) {
    switch (e) {
    case ENOENT: return 2;   // ERROR_FILE_NOT_FOUND
    case ENOTDIR: return 3;  // ERROR_PATH_NOT_FOUND
    case EACCES: case EPERM: return 5;
    case EEXIST: return 80;  // ERROR_FILE_EXISTS
    case ENOSPC: return 112;
    default: return 31;      // ERROR_GEN_FAILURE
    }
}

uint64_t filetime_of(const struct timespec& t) {
    return (uint64_t)t.tv_sec * 10000000ull + (uint64_t)t.tv_nsec / 100 + 116444736000000000ull;
}

uint32_t attributes_of(const struct stat& st, const std::string& name) {
    uint32_t a = S_ISDIR(st.st_mode) ? 0x10 : 0x20;  // DIRECTORY : ARCHIVE
    if (!(st.st_mode & S_IWUSR)) a |= 1;              // READONLY
    if (!name.empty() && name[0] == '.' && name != "." && name != "..") a |= 2;  // HIDDEN
    return a;
}

void put_find_data(uint32_t d, const std::string& dir, const std::string& name) {
    std::memset(gptr(d), 0, 320);
    struct stat st{};
    ::stat((dir + "/" + name).c_str(), &st);
    wr(d, attributes_of(st, name));
    const uint64_t ct = filetime_of(st.st_birthtimespec), at = filetime_of(st.st_atimespec),
                   mt = filetime_of(st.st_mtimespec);
    wr(d + 4, (uint32_t)ct), wr(d + 8, (uint32_t)(ct >> 32));
    wr(d + 12, (uint32_t)at), wr(d + 16, (uint32_t)(at >> 32));
    wr(d + 20, (uint32_t)mt), wr(d + 24, (uint32_t)(mt >> 32));
    wr(d + 28, (uint32_t)((uint64_t)st.st_size >> 32));
    wr(d + 32, (uint32_t)st.st_size);
    put_str(d + 44, 260, name);
}

std::shared_ptr<File> std_handle(int fd) {
    static std::shared_ptr<File> files[3];
    static std::once_flag once;
    std::call_once(once, [] {
        for (int k = 0; k < 3; k++) {
            files[k] = std::make_shared<File>();
            files[k]->fd = k;
            files[k]->console = true;
        }
    });
    return files[fd];
}

std::shared_ptr<File> file_of(uint32_t h) { return handle_as<File>(h); }

}  // namespace

}  // namespace scimitar::win32

namespace scimitar::win32 {
// Without completion ports (kernel32_acr.cpp) there is nothing to post.
[[gnu::weak]] void overlapped_done(uint32_t, uint32_t, uint32_t) {}
}  // namespace scimitar::win32

namespace scimitar::imports {
using namespace scimitar::win32;

W32(kernel32, CreateFileA) {
    const std::string name = gstr(arg(c, 0));
    const uint32_t access = arg(c, 1), disposition = arg(c, 4);
    const std::string path = host_path(name);
    int flags = (access & 0x40000000u) ? ((access & 0x80000000u) ? O_RDWR : O_WRONLY) : O_RDONLY;
    switch (disposition) {
    case 1: flags |= O_CREAT | O_EXCL; break;   // CREATE_NEW
    case 2: flags |= O_CREAT | O_TRUNC; break;  // CREATE_ALWAYS
    case 4: flags |= O_CREAT; break;            // OPEN_ALWAYS
    case 5: flags |= O_TRUNC; break;            // TRUNCATE_EXISTING
    default: break;                             // OPEN_EXISTING
    }
    const bool existed = ::access(path.c_str(), F_OK) == 0;
    struct stat st;
    if (existed && ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode) && !(arg(c, 5) & 0x02000000u)) {
        set_last_error(5);
        trace("CreateFileA(%s) = directory", name.c_str());
        return ret(c, 0xffffffffu, 7);
    }
    const int fd = ::open(path.c_str(), flags, 0644);
    trace("CreateFileA(%s, %08x, %u) = %d", name.c_str(), access, disposition, fd);
    if (fd < 0) {
        set_last_error(error_from_errno(errno));
        return ret(c, 0xffffffffu, 7);  // INVALID_HANDLE_VALUE
    }
    auto f = std::make_shared<File>();
    f->fd = fd;
    f->path = path;
    set_last_error(existed && (disposition == 2 || disposition == 4) ? 183 : 0);  // ERROR_ALREADY_EXISTS
    ret(c, add_handle(f), 7);
}

W32(kernel32, ReadFile) {
    const uint32_t h = arg(c, 0), buf = arg(c, 1), n = arg(c, 2), done = arg(c, 3), ov = arg(c, 4);
    auto f = file_of(h);
    if (!f) {
        set_last_error(6);  // ERROR_INVALID_HANDLE
        return ret(c, 0, 5);
    }
    ssize_t r;
    if (ov) {
        // Overlapped: done at once, with the result in the OVERLAPPED.
        const off_t off = (off_t)rd(ov + 8) | (off_t)rd(ov + 12) << 32;
        r = ::pread(f->fd, gptr(buf), n, off);
        wr(ov, r < 0 ? 0xc0000001u : 0);
        wr(ov + 4, r < 0 ? 0 : (uint32_t)r);
        if (rd(ov + 16)) signal_event(rd(ov + 16));  // as a completed request does
        if (r >= 0) overlapped_done(h, ov, (uint32_t)r);
    } else {
        std::lock_guard<std::mutex> l(f->lock);
        r = ::read(f->fd, gptr(buf), n);
    }
    if (done) wr(done, r < 0 ? 0 : (uint32_t)r);
    if (r < 0) {
        set_last_error(error_from_errno(errno));
        return ret(c, 0, 5);
    }
    if (ov && r == 0 && n) {
        set_last_error(38);  // ERROR_HANDLE_EOF
        return ret(c, 0, 5);
    }
    ret(c, 1, 5);
}

W32(kernel32, WriteFile) {
    const uint32_t h = arg(c, 0), buf = arg(c, 1), n = arg(c, 2), done = arg(c, 3), ov = arg(c, 4);
    auto f = h == 0xfffffff5u || h == 0xfffffff4u ? std_handle(h == 0xfffffff5u ? 1 : 2) : file_of(h);
    if (!f) {
        set_last_error(6);
        return ret(c, 0, 5);
    }
    ssize_t r;
    if (ov) {
        const off_t off = (off_t)rd(ov + 8) | (off_t)rd(ov + 12) << 32;
        r = ::pwrite(f->fd, gptr(buf), n, off);
        wr(ov, 0);
        wr(ov + 4, r < 0 ? 0 : (uint32_t)r);
        if (r >= 0) overlapped_done(h, ov, (uint32_t)r);
    } else {
        std::lock_guard<std::mutex> l(f->lock);
        r = ::write(f->fd, gptr(buf), n);
    }
    if (done) wr(done, r < 0 ? 0 : (uint32_t)r);
    if (r < 0) {
        set_last_error(error_from_errno(errno));
        return ret(c, 0, 5);
    }
    ret(c, 1, 5);
}

W32(kernel32, GetOverlappedResult) {
    const uint32_t ov = arg(c, 1), done = arg(c, 2);
    if (done) wr(done, rd(ov + 4));
    ret(c, rd(ov) == 0 ? 1 : 0, 4);
}

W32(kernel32, SetFilePointer) {
    auto f = file_of(arg(c, 0));
    if (!f) return ret(c, 0xffffffffu, 4);
    const uint32_t hi_ptr = arg(c, 2);
    off_t off = hi_ptr ? (off_t)((uint64_t)rd(hi_ptr) << 32 | arg(c, 1)) : (off_t)(int32_t)arg(c, 1);
    std::lock_guard<std::mutex> l(f->lock);
    const off_t r = ::lseek(f->fd, off, (int)arg(c, 3));
    if (r < 0) {
        set_last_error(131);  // ERROR_NEGATIVE_SEEK
        return ret(c, 0xffffffffu, 4);
    }
    if (hi_ptr) wr(hi_ptr, (uint32_t)((uint64_t)r >> 32));
    set_last_error(0);
    ret(c, (uint32_t)r, 4);
}

W32(kernel32, GetFileSize) {
    auto f = file_of(arg(c, 0));
    struct stat st;
    if (!f || ::fstat(f->fd, &st) != 0) return ret(c, 0xffffffffu, 2);
    if (arg(c, 1)) wr(arg(c, 1), (uint32_t)((uint64_t)st.st_size >> 32));
    ret(c, (uint32_t)st.st_size, 2);
}

W32(kernel32, GetFileSizeEx) {
    auto f = file_of(arg(c, 0));
    struct stat st;
    if (!f || ::fstat(f->fd, &st) != 0) return ret(c, 0, 2);
    wr(arg(c, 1), (uint32_t)st.st_size);
    wr(arg(c, 1) + 4, (uint32_t)((uint64_t)st.st_size >> 32));
    ret(c, 1, 2);
}

W32(kernel32, GetFileType) {
    const uint32_t h = arg(c, 0);
    if (h >= 0xfffffff4u) return ret(c, 2, 1);  // the standard handles: FILE_TYPE_CHAR
    auto f = file_of(h);
    ret(c, f ? (f->console ? 2 : 1) : 0, 1);
}

W32(kernel32, GetStdHandle) {
    // STD_INPUT_HANDLE -10, OUTPUT -11, ERROR -12: their own values work as handles.
    ret(c, arg(c, 0), 1);
}

W32(kernel32, SetStdHandle) { ret(c, 1, 2); }
W32(kernel32, SetHandleCount) { ret(c, arg(c, 0), 1); }

W32(kernel32, FlushFileBuffers) { ret(c, 1, 1); }

W32(kernel32, SetEndOfFile) {
    auto f = file_of(arg(c, 0));
    if (!f) return ret(c, 0, 1);
    const off_t pos = ::lseek(f->fd, 0, SEEK_CUR);
    ret(c, ::ftruncate(f->fd, pos) == 0, 1);
}

W32(kernel32, LockFile) { ret(c, 1, 5); }
W32(kernel32, LockFileEx) { ret(c, 1, 6); }
W32(kernel32, UnlockFile) { ret(c, 1, 5); }

W32(kernel32, GetFileTime) {
    auto f = file_of(arg(c, 0));
    struct stat st;
    if (!f || ::fstat(f->fd, &st) != 0) return ret(c, 0, 4);
    const uint64_t t[3] = {filetime_of(st.st_birthtimespec), filetime_of(st.st_atimespec),
                           filetime_of(st.st_mtimespec)};
    for (int k = 0; k < 3; k++)
        if (arg(c, 1 + k)) wr(arg(c, 1 + k), (uint32_t)t[k]), wr(arg(c, 1 + k) + 4, (uint32_t)(t[k] >> 32));
    ret(c, 1, 4);
}

W32(kernel32, SetFileTime) { ret(c, 1, 4); }

W32(kernel32, GetFileAttributesA) {
    const std::string name = gstr(arg(c, 0));
    struct stat st;
    if (::stat(host_path(name).c_str(), &st) != 0) {
        set_last_error(error_from_errno(errno));
        trace("GetFileAttributesA(%s): none", name.c_str());
        return ret(c, 0xffffffffu, 1);
    }
    ret(c, attributes_of(st, ""), 1);
}

W32(kernel32, SetFileAttributesA) { ret(c, 1, 2); }

W32(kernel32, CreateDirectoryA) {
    const std::string path = host_path(gstr(arg(c, 0)));
    if (::mkdir(path.c_str(), 0755) != 0) {
        set_last_error(errno == EEXIST ? 183 : error_from_errno(errno));
        return ret(c, 0, 2);
    }
    ret(c, 1, 2);
}

W32(kernel32, RemoveDirectoryA) { ret(c, ::rmdir(host_path(gstr(arg(c, 0))).c_str()) == 0, 1); }

W32(kernel32, DeleteFileA) {
    if (::unlink(host_path(gstr(arg(c, 0))).c_str()) != 0) {
        set_last_error(error_from_errno(errno));
        return ret(c, 0, 1);
    }
    ret(c, 1, 1);
}

W32(kernel32, MoveFileA) {
    ret(c, ::rename(host_path(gstr(arg(c, 0))).c_str(), host_path(gstr(arg(c, 1))).c_str()) == 0, 2);
}

W32(kernel32, CopyFileA) {
    const std::string from = host_path(gstr(arg(c, 0))), to = host_path(gstr(arg(c, 1)));
    if (arg(c, 2) && ::access(to.c_str(), F_OK) == 0) return ret(c, 0, 3);
    std::ifstream in(from, std::ios::binary);
    std::ofstream out(to, std::ios::binary);
    if (!in || !out) return ret(c, 0, 3);
    out << in.rdbuf();
    ret(c, 1, 3);
}

W32(kernel32, FindFirstFileA) {
    const std::string pattern = host_path(gstr(arg(c, 0)));
    const size_t slash = pattern.rfind('/');
    auto f = std::make_shared<Find>();
    f->dir = slash == std::string::npos ? "." : pattern.substr(0, slash);
    const std::string glob = slash == std::string::npos ? pattern : pattern.substr(slash + 1);
    if (DIR* d = ::opendir(f->dir.c_str())) {
        while (struct dirent* e = ::readdir(d)) {
            const std::string n = e->d_name;
            if (glob == "*.*" || fnmatch(glob.c_str(), n.c_str(), FNM_CASEFOLD) == 0) f->names.push_back(n);
        }
        ::closedir(d);
    }
    trace("FindFirstFileA(%s): %zu", gstr(arg(c, 0)).c_str(), f->names.size());
    if (f->names.empty()) {
        set_last_error(2);
        return ret(c, 0xffffffffu, 2);
    }
    put_find_data(arg(c, 1), f->dir, f->names[f->next++]);
    ret(c, add_handle(f), 2);
}

W32(kernel32, FindNextFileA) {
    auto f = handle_as<Find>(arg(c, 0));
    if (!f || f->next >= f->names.size()) {
        set_last_error(18);  // ERROR_NO_MORE_FILES
        return ret(c, 0, 2);
    }
    put_find_data(arg(c, 1), f->dir, f->names[f->next++]);
    ret(c, 1, 2);
}

W32(kernel32, FindClose) { ret(c, close_handle(arg(c, 0)), 1); }

W32(kernel32, GetCurrentDirectoryA) {
    const std::string& dir = process().game_dir;
    const uint32_t size = arg(c, 0);
    if (size <= dir.size()) return ret(c, (uint32_t)dir.size() + 1, 2);
    ret(c, put_str(arg(c, 1), size, dir), 2);
}

W32(kernel32, SetCurrentDirectoryA) {
    trace("SetCurrentDirectoryA(%s)", gstr(arg(c, 0)).c_str());
    const std::string path = host_path(gstr(arg(c, 0)));
    ret(c, ::chdir(path.c_str()) == 0, 1);
}

W32(kernel32, GetFullPathNameA) {
    std::string name = gstr(arg(c, 0));
    for (char& ch : name)
        if (ch == '/') ch = '\\';
    if (!(name.size() >= 2 && name[1] == ':') && !(name.size() >= 1 && name[0] == '\\'))
        name = process().game_dir + "\\" + name;
    const uint32_t size = arg(c, 1), buf = arg(c, 2), file_part = arg(c, 3);
    if (size <= name.size()) return ret(c, (uint32_t)name.size() + 1, 4);
    const uint32_t n = put_str(buf, size, name);
    if (file_part) {
        const size_t s = name.rfind('\\');
        wr(file_part, s == std::string::npos ? buf : buf + (uint32_t)s + 1);
    }
    ret(c, n, 4);
}

W32(kernel32, GetTempPathA) {
    const std::string dir = process().game_dir + "\\temp\\";
    ::mkdir((process().host_dir + "/temp").c_str(), 0755);
    ret(c, put_str(arg(c, 1), arg(c, 0), dir), 2);
}

// ---- INI files ----

namespace {

struct Ini {
    std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>> sections;
};

std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

bool iequal(const std::string& a, const std::string& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower((unsigned char)x) == std::tolower((unsigned char)y);
           });
}

std::string ini_path(uint32_t file) {
    std::string f = gstr(file);
    if (f.find('\\') == std::string::npos && f.find('/') == std::string::npos) f = "C:\\WINDOWS\\" + f;
    return host_path(f);
}

Ini read_ini(const std::string& path) {
    Ini ini;
    std::ifstream in(path);
    for (std::string line; std::getline(in, line);) {
        line = trim(line);
        if (line.empty() || line[0] == ';') continue;
        if (line[0] == '[') {
            ini.sections.push_back({trim(line.substr(1, line.find(']') - 1)), {}});
        } else if (!ini.sections.empty()) {
            const size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            ini.sections.back().second.push_back({trim(line.substr(0, eq)), trim(line.substr(eq + 1))});
        }
    }
    return ini;
}

void write_ini(const std::string& path, const Ini& ini) {
    std::ofstream out(path);
    for (const auto& [name, keys] : ini.sections) {
        out << "[" << name << "]\r\n";
        for (const auto& [k, v] : keys) out << k << "=" << v << "\r\n";
    }
}

// A double-null-terminated list into a buffer of `cap` chars; returns the
// length without the final null (cap - 2 when it does not fit).
uint32_t put_list(uint32_t buf, uint32_t cap, const std::vector<std::string>& items) {
    std::string s;
    for (const auto& i : items) s += i + '\0';
    if (s.empty()) s += '\0';
    if (s.size() + 1 > cap) s.resize(cap >= 2 ? cap - 2 : 0), s += std::string(2, '\0');
    else s += '\0';
    std::memcpy(gptr(buf), s.data(), s.size());
    return (uint32_t)s.size() - 1;
}

}  // namespace

W32(kernel32, GetPrivateProfileStringA) {
    const uint32_t section = arg(c, 0), key = arg(c, 1), def = arg(c, 2), buf = arg(c, 3), size = arg(c, 4);
    const Ini ini = read_ini(ini_path(arg(c, 5)));
    if (!section) {
        std::vector<std::string> names;
        for (const auto& s : ini.sections) names.push_back(s.first);
        return ret(c, put_list(buf, size, names), 6);
    }
    for (const auto& [name, keys] : ini.sections) {
        if (!iequal(name, gstr(section))) continue;
        if (!key) {
            std::vector<std::string> names;
            for (const auto& kv : keys) names.push_back(kv.first);
            return ret(c, put_list(buf, size, names), 6);
        }
        for (const auto& [k, v] : keys)
            if (iequal(k, gstr(key))) return ret(c, put_str(buf, size, v), 6);
    }
    ret(c, put_str(buf, size, trim(gstr(def))), 6);
}

W32(kernel32, GetPrivateProfileIntA) {
    const Ini ini = read_ini(ini_path(arg(c, 3)));
    for (const auto& [name, keys] : ini.sections)
        if (iequal(name, gstr(arg(c, 0))))
            for (const auto& [k, v] : keys)
                if (iequal(k, gstr(arg(c, 1)))) return ret(c, (uint32_t)std::strtol(v.c_str(), nullptr, 0), 4);
    ret(c, arg(c, 2), 4);
}

W32(kernel32, GetPrivateProfileSectionA) {
    const Ini ini = read_ini(ini_path(arg(c, 3)));
    for (const auto& [name, keys] : ini.sections)
        if (iequal(name, gstr(arg(c, 0)))) {
            std::vector<std::string> lines;
            for (const auto& [k, v] : keys) lines.push_back(k + "=" + v);
            return ret(c, put_list(arg(c, 1), arg(c, 2), lines), 4);
        }
    ret(c, put_list(arg(c, 1), arg(c, 2), {}), 4);
}

W32(kernel32, GetPrivateProfileSectionNamesA) {
    const Ini ini = read_ini(ini_path(arg(c, 2)));
    std::vector<std::string> names;
    for (const auto& s : ini.sections) names.push_back(s.first);
    ret(c, put_list(arg(c, 0), arg(c, 1), names), 3);
}

W32(kernel32, WritePrivateProfileStringA) {
    const std::string path = ini_path(arg(c, 3));
    Ini ini = read_ini(path);
    const std::string section = gstr(arg(c, 0));
    auto it = std::find_if(ini.sections.begin(), ini.sections.end(),
                           [&](const auto& s) { return iequal(s.first, section); });
    if (it == ini.sections.end()) {
        ini.sections.push_back({section, {}});
        it = std::prev(ini.sections.end());
    }
    if (!arg(c, 1)) {
        ini.sections.erase(it);
    } else {
        auto& keys = it->second;
        auto k = std::find_if(keys.begin(), keys.end(), [&](const auto& kv) { return iequal(kv.first, gstr(arg(c, 1))); });
        if (!arg(c, 2)) {
            if (k != keys.end()) keys.erase(k);
        } else if (k != keys.end()) {
            k->second = gstr(arg(c, 2));
        } else {
            keys.push_back({gstr(arg(c, 1)), gstr(arg(c, 2))});
        }
    }
    write_ini(path, ini);
    ret(c, 1, 4);
}

}  // namespace scimitar::imports
