// Carbon File Manager fork I/O, used by the engine's streaming (Gear::AsynchDevice):
// FSOpenFork, FSGetForkSize, FSCloseFork, FSGetDataForkName and PBReadForkAsync.
// Forks are host file descriptors behind 16-bit reference numbers. Async reads
// run on a worker thread, which fills the parameter block and then calls the
// guest's completion routine with it, as Carbon did from its own thread.
//
// FSForkIOParam (i386, packed to 2 bytes; offsets checked against the game's
// LLRead): ioCompletion +12, ioResult +16 (SInt16), forkRefNum +24 (SInt16),
// buffer +32, requestCount +36, actualCount +40, positionMode +44 (UInt16),
// positionOffset +46 (SInt64).
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include "scimitar/guest_heap.h"
#include "scimitar/imports.h"
#include "scimitar/runtime.h"

namespace scimitar::imports {

std::string fsref_path(Context& c, uint32_t ref);  // cf.cpp

namespace {

constexpr int16_t kNoErr = 0, kEofErr = -39, kFnfErr = -43, kRfNumErr = -51, kIoErr = -36;

std::mutex g_lock;
std::map<int16_t, int> g_forks;  // refnum -> fd
int16_t g_next_ref = 100;

int fd_of(int16_t ref) {
    std::lock_guard<std::mutex> l(g_lock);
    auto it = g_forks.find(ref);
    return it == g_forks.end() ? -1 : it->second;
}

// The async reader: one worker with its own guest context for completions.
struct Reader {
    std::mutex m;
    std::condition_variable cv;
    std::deque<uint32_t> queue;  // parameter blocks
    GuestMemory* mem = nullptr;
    bool started = false;
};
Reader g_reader;

void reader_thread() {
    Context c;
    c.mem = g_reader.mem;
    set_thread_context(&c);
    const uint32_t stack = heap::alloc(*c.mem, 1u << 20, 16);
    GuestMemory& m = *c.mem;
    for (;;) {
        uint32_t pb;
        {
            std::unique_lock<std::mutex> l(g_reader.m);
            g_reader.cv.wait(l, [] { return !g_reader.queue.empty(); });
            pb = g_reader.queue.front();
            g_reader.queue.pop_front();
        }
        const int16_t ref = (int16_t)m.read<uint16_t>(pb + 24);
        const uint32_t buf = m.read<uint32_t>(pb + 32), want = m.read<uint32_t>(pb + 36);
        const uint16_t mode = m.read<uint16_t>(pb + 44);
        const int64_t off = (int64_t)m.read<uint64_t>(pb + 46);
        const int fd = fd_of(ref);
        int16_t err = kNoErr;
        ssize_t got = 0;
        if (fd < 0) {
            err = kRfNumErr;
        } else {
            off_t at;
            switch (mode & 3) {
            case 1: at = off; break;                                        // fsFromStart
            case 2: at = ::lseek(fd, 0, SEEK_END) + off; break;              // fsFromLEOF
            default: at = ::lseek(fd, 0, SEEK_CUR) + ((mode & 3) == 3 ? off : 0); break;  // mark
            }
            got = ::pread(fd, m.ptr(buf), want, at);
            if (got < 0) err = kIoErr, got = 0;
            else {
                ::lseek(fd, at + got, SEEK_SET);
                if ((uint32_t)got < want) err = kEofErr;
            }
        }
        m.write<uint32_t>(pb + 40, (uint32_t)got);
        m.write<uint16_t>(pb + 16, (uint16_t)err);
        if (const uint32_t done = m.read<uint32_t>(pb + 12)) {
            c.esp = ((stack + (1u << 20)) & ~15u) - 12;
            call_guest(c, done, {pb});
        }
    }
}

}  // namespace

// OSErr FSGetDataForkName(HFSUniStr255* name): the data fork's name is empty.
void imp_FSGetDataForkName(Context& c) {
    c.mem->write<uint16_t>(arg32(c, 0), 0);
    shim_return(c, 0);
}
// OSErr FSOpenFork(const FSRef*, UniCharCount nameLength, const UniChar* name,
//                  SInt8 permissions, FSIORefNum* refNum)
void imp_FSOpenFork(Context& c) {
    const std::string path = fsref_path(c, arg32(c, 0));
    const int perm = (int8_t)(arg32(c, 3) & 0xff);
    const int flags = (perm & 2) ? ((perm & 1) ? O_RDWR : O_WRONLY) : O_RDONLY;  // fsRdPerm 1, fsWrPerm 2
    const int fd = ::open(path.c_str(), flags);
    static const bool trace = std::getenv("SCIMITAR_TRACE_FILES") != nullptr;
    if (trace) std::fprintf(stderr, "FSOpenFork(%s, %d) = %d\n", path.c_str(), perm, fd);
    if (fd < 0) return shim_return(c, (uint32_t)(uint16_t)kFnfErr);
    int16_t ref;
    {
        std::lock_guard<std::mutex> l(g_lock);
        ref = g_next_ref++;
        g_forks[ref] = fd;
    }
    c.mem->write<uint16_t>(arg32(c, 4), (uint16_t)ref);
    shim_return(c, 0);
}
// OSErr FSGetForkSize(FSIORefNum, SInt64* size)
void imp_FSGetForkSize(Context& c) {
    const int fd = fd_of((int16_t)(arg32(c, 0) & 0xffff));
    struct stat st;
    if (fd < 0 || ::fstat(fd, &st) != 0) return shim_return(c, (uint32_t)(uint16_t)kRfNumErr);
    c.mem->write<uint64_t>(arg32(c, 1), (uint64_t)st.st_size);
    shim_return(c, 0);
}
// OSErr FSCloseFork(FSIORefNum)
void imp_FSCloseFork(Context& c) {
    int fd = -1;
    {
        std::lock_guard<std::mutex> l(g_lock);
        auto it = g_forks.find((int16_t)(arg32(c, 0) & 0xffff));
        if (it != g_forks.end()) fd = it->second, g_forks.erase(it);
    }
    if (fd >= 0) ::close(fd);
    shim_return(c, fd >= 0 ? 0 : (uint32_t)(uint16_t)kRfNumErr);
}
// void PBReadForkAsync(FSForkIOParam*): ioResult is 1 while pending.
void imp_PBReadForkAsync(Context& c) {
    const uint32_t pb = arg32(c, 0);
    c.mem->write<uint16_t>(pb + 16, 1);
    {
        std::lock_guard<std::mutex> l(g_reader.m);
        if (!g_reader.started) {
            g_reader.mem = c.mem;
            g_reader.started = true;
            std::thread(reader_thread).detach();
        }
        g_reader.queue.push_back(pb);
    }
    g_reader.cv.notify_one();
    shim_return(c, 0);
}

}  // namespace scimitar::imports

namespace scimitar {
namespace {
using namespace imports;
#define S(N) {"_" #N, imp_##N},
const ImportShim kCarbonFork[] = {
    S(FSGetDataForkName) S(FSOpenFork) S(FSGetForkSize) S(FSCloseFork) S(PBReadForkAsync)
};
#undef S
SCIMITAR_REGISTER_SHIMS(kCarbonFork);
}  // namespace
}  // namespace scimitar
