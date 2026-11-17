// binkw32.dll: the Windows Bink exports are the Mac ones (platform/src/bink.cpp,
// FFmpeg) in stdcall: each wrapper runs the Mac shim, which pops only the
// return address, then pops the arguments too.
#include "win32.h"

namespace scimitar::imports {
void imp_BinkWait(Context&);
void imp_BinkSetSoundTrack(Context&);
void imp_BinkShouldSkip(Context&);
void imp_BinkNextFrame(Context&);
void imp_BinkDoFrame(Context&);
void imp_BinkOpen(Context&);
void imp_BinkSetVolume(Context&);
void imp_BinkPause(Context&);
void imp_BinkClose(Context&);
void imp_BinkSetMemory(Context&);
void imp_BinkGetFrameBuffersInfo(Context&);
void imp_BinkCopyToBuffer(Context&);
void imp_BinkGoto(Context&);
void imp_BinkRegisterFrameBuffers(Context&);

using namespace scimitar::win32;

#define BINK(name, bytes)                     \
    W32(binkw32, _##name##_##bytes) {         \
        imp_##name(c);                        \
        c.esp += bytes;                       \
    }

BINK(BinkWait, 4)
BINK(BinkSetSoundTrack, 8)
BINK(BinkShouldSkip, 4)
BINK(BinkNextFrame, 4)
BINK(BinkDoFrame, 4)
BINK(BinkSetVolume, 12)
BINK(BinkPause, 8)
BINK(BinkClose, 4)
BINK(BinkSetMemory, 8)
BINK(BinkCopyToBuffer, 28)
BINK(BinkGoto, 12)
BINK(BinkRegisterFrameBuffers, 8)

// void BinkGetFrameBuffersInfo(HBINK, BINKFRAMEBUFFERS*): one buffer set (the
// Mac shim's header), whose Y, cR and cB planes the game is to allocate
// (Prince of Persia makes a texture for each plane flagged and registers them).
W32(binkw32, _BinkGetFrameBuffersInfo_8) {
    const uint32_t p = arg(c, 1);
    imp_BinkGetFrameBuffersInfo(c);
    c.esp += 8;
    if (!p || !rd(p + 4)) return;
    std::memset(gptr(p + 0x18), 0, 2 * 48);
    for (int k = 0; k < 3; k++) wr(p + 0x18 + 12 * k, 1);  // Allocate Y, cR, cB; no alpha
}

// void BinkSetMixBinVolumes(HBINK, U32 track, U32* bins, S32* volumes, U32 total):
// speaker routing, which the stereo mix ignores.
W32(binkw32, _BinkSetMixBinVolumes_20) { ret(c, 0, 5); }

// HBINK BinkOpen(const char* name, U32 flags): the name as a host path.
W32(binkw32, _BinkOpen_8) {
    const std::string path = host_path(gstr(arg(c, 0)));
    trace("BinkOpen(%s)", path.c_str());
    const uint32_t s = guest_strdup(path), saved = arg(c, 0);
    wr(c.esp + 4, s);
    imp_BinkOpen(c);
    wr(c.esp + 0, saved);  // the argument slot, now at esp after the pop
    gfree(s);
    c.esp += 8;
}

// The sound system: Bink plays through its own host output, so the
// DirectSound hookup only has to succeed.
W32(binkw32, _BinkOpenDirectSound_4) { ret(c, 0x10060, 1); }
W32(binkw32, _BinkSetSoundSystem_8) { ret(c, 1, 2); }

}  // namespace scimitar::imports
