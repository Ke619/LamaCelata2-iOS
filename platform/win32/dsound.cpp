// DirectSound 8 (dsound!DirectSoundCreate8) for the game's DARE sound engine:
// secondary buffers in guest memory, mixed to a host output unit. EAX is absent (EAXDirectSoundCreate8 fails, IKsPropertySet is
// not offered), so the engine uses plain DirectSound 3D.
//
// Status: buffers, cursors and a stereo mix (volume and pan); 3D buffers are
// attenuated by distance (DirectSound's rolloff between the minimum and
// maximum distances) and panned by their direction from the listener.
// Velocities (Doppler) and cones are kept, not applied; deferred settings
// apply at once.
//
// SCIMITAR_AUDIO_DUMP=file: with SCIMITAR_MUTE, the silent mix is written to
// the file as raw 48 kHz stereo float (to check sound in unattended runs).
#include <AudioUnit/AudioUnit.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <set>
#include <thread>
#include <mutex>
#include <vector>

#include "com.h"
#include "win32.h"

namespace scimitar::win32::dsound {

using namespace scimitar::win32::com;

constexpr uint32_t DS_OK = 0, DSERR_INVALIDPARAM = 0x80070057u, DSERR_NODRIVER = 0x88780078u;

using clock = std::chrono::steady_clock;

// The mix: a host output unit pulls 48 kHz stereo and mixes every playing
// secondary buffer into it (volume and pan; 3D positions are not applied),
// advancing the buffers' play cursors as it consumes them. Without an output
// (SCIMITAR_MUTE, or no device) a timer does the same work silently, so the
// game sees the same cursors either way.
constexpr double kRate = 48000;

struct Buffer;
std::mutex g_mix_lock;
std::set<Buffer*> g_buffers;

struct Vec {
    float x = 0, y = 0, z = 0;
};
Vec operator-(Vec a, Vec b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
float dot(Vec a, Vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec cross(Vec a, Vec b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }

// A 3D buffer's settings (DS3DBUFFER) and the listener's (DS3DLISTENER).
struct Params3D {
    Vec position, velocity, cone_orientation{0, 0, 1};
    uint32_t inside = 360, outside = 360, mode = 0;  // DS3DMODE_NORMAL
    int32_t cone_volume = 0;
    float min_distance = 1, max_distance = 1e9f;
};
struct Listener {
    Vec position, velocity, front{0, 0, 1}, top{0, 1, 0};
    float distance_factor = 1, rolloff = 1, doppler = 1;
} g_listener;

struct Buffer : Native {
    bool primary = false, playing = false, looping = false;
    uint32_t flags = 0, size = 0, data = 0;
    uint32_t rate = 44100, channels = 2, bits = 16, block = 4;
    double pos = 0;  // play cursor, in frames (mixer-driven)
    int32_t volume = 0, pan = 0;
    uint32_t frequency = 0;
    bool is3d = false;  // DSBCAPS_CTRL3D
    Params3D p3d;
    Buffer() {
        std::lock_guard<std::mutex> l(g_mix_lock);
        g_buffers.insert(this);
    }
    Buffer(const Buffer& o) : Native(o) {
        primary = o.primary, flags = o.flags, size = o.size, data = o.data, rate = o.rate;
        channels = o.channels, bits = o.bits, block = o.block, volume = o.volume, pan = o.pan, frequency = o.frequency;
        is3d = o.is3d, p3d = o.p3d;
        std::lock_guard<std::mutex> l(g_mix_lock);
        g_buffers.insert(this);
    }
    ~Buffer() override {
        std::lock_guard<std::mutex> l(g_mix_lock);
        g_buffers.erase(this);
    }
    uint32_t frames() const { return size / block; }
    uint32_t position() const { return (uint32_t)pos * block % std::max(1u, size); }
};

// A 3D buffer's gain and left/right weights: DirectSound's rolloff
// (min / (min + rolloff * (d - min)), held from the maximum distance on), and
// a pan by the direction in the listener's frame (left-handed: x right).
void spatialize(const Buffer* b, float* gain, float* left, float* right) {
    *gain = 1, *left = 1, *right = 1;
    if (!b->is3d || b->p3d.mode == 2) return;  // DS3DMODE_DISABLE
    const Listener& L = g_listener;
    Vec rel = b->p3d.position;
    if (b->p3d.mode != 1) {  // world position, not DS3DMODE_HEADRELATIVE
        const Vec d = b->p3d.position - L.position;
        const Vec right_axis = cross(L.top, L.front);
        rel = {dot(d, right_axis), dot(d, L.top), dot(d, L.front)};
        const float rl = std::sqrt(dot(right_axis, right_axis)), fl = std::sqrt(dot(L.front, L.front));
        if (rl > 0) rel.x /= rl;
        if (fl > 0) rel.z /= fl;
    }
    const float dist = std::sqrt(dot(rel, rel));
    const float mn = std::max(b->p3d.min_distance, 1e-6f);
    const float d = std::min(dist, std::max(b->p3d.max_distance, mn));
    if (d > mn) *gain = mn / (mn + L.rolloff * (d - mn));
    if (dist > 1e-4f) {
        const float x = std::clamp(rel.x / dist, -1.0f, 1.0f);  // -1 left, 1 right
        *left = std::min(1.0f, 1.0f - 0.7f * x);
        *right = std::min(1.0f, 1.0f + 0.7f * x);
    }
}

// Mixes `n` frames into `out` (interleaved stereo float; null: only advance).
void mix(float* out, uint32_t n) {
    std::lock_guard<std::mutex> l(g_mix_lock);
    if (out) std::memset(out, 0, sizeof(float) * 2 * n);
    for (Buffer* b : g_buffers) {
        if (b->primary || !b->playing || !b->frames()) continue;
        const double step = (b->frequency ? b->frequency : b->rate) / kRate;
        float g3, l3, r3;
        spatialize(b, &g3, &l3, &r3);
        const float gain = b->volume <= -10000 ? 0.0f : std::pow(10.0f, b->volume / 2000.0f) * g3;
        const float pl = (b->pan > 0 ? std::pow(10.0f, -b->pan / 2000.0f) : 1.0f) * l3;
        const float pr = (b->pan < 0 ? std::pow(10.0f, b->pan / 2000.0f) : 1.0f) * r3;
        const uint8_t* base = gptr(b->data);
        const uint32_t frames = b->frames();
        for (uint32_t k = 0; k < n; k++) {
            uint32_t f = (uint32_t)b->pos;
            if (f >= frames) {
                if (!b->looping) {
                    b->playing = false;
                    b->pos = 0;
                    break;
                }
                b->pos -= frames;
                f = (uint32_t)b->pos;
            }
            if (out && gain > 0) {
                const uint8_t* p = base + f * b->block;
                float left, right;
                if (b->bits == 16) {
                    left = (int16_t)(p[0] | p[1] << 8) / 32768.0f;
                    right = b->channels > 1 ? (int16_t)(p[2] | p[3] << 8) / 32768.0f : left;
                } else {
                    left = (p[0] - 128) / 128.0f;
                    right = b->channels > 1 ? (p[1] - 128) / 128.0f : left;
                }
                out[2 * k] += left * gain * pl;
                out[2 * k + 1] += right * gain * pr;
            }
            b->pos += step;
        }
    }
}

OSStatus render(void*, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32, UInt32 frames,
                AudioBufferList* io) {
    float* out = (float*)io->mBuffers[0].mData;
    mix(out, frames);
    for (UInt32 k = 0; k < 2 * frames; k++) out[k] = std::fmax(-1.0f, std::fmin(1.0f, out[k]));
    return noErr;
}

// Starts the output unit, or the silent clock.
void start_mixer() {
    static std::once_flag once;
    std::call_once(once, [] {
        if (!std::getenv("SCIMITAR_MUTE")) {
            AudioComponentDescription desc{kAudioUnitType_Output, kAudioUnitSubType_DefaultOutput,
                                           kAudioUnitManufacturer_Apple, 0, 0};
            AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
            AudioUnit unit = nullptr;
            if (comp && AudioComponentInstanceNew(comp, &unit) == noErr) {
                AudioStreamBasicDescription f{};
                f.mSampleRate = kRate;
                f.mFormatID = kAudioFormatLinearPCM;
                f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
                f.mChannelsPerFrame = 2;
                f.mBitsPerChannel = 32;
                f.mFramesPerPacket = 1;
                f.mBytesPerFrame = f.mBytesPerPacket = 8;
                AURenderCallbackStruct cb{render, nullptr};
                if (AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f) == noErr &&
                    AudioUnitSetProperty(unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb) == noErr &&
                    AudioUnitInitialize(unit) == noErr && AudioOutputUnitStart(unit) == noErr)
                    return;
            }
            std::fprintf(stderr, "%s: no sound output; DirectSound plays silently\n", process().game_id.c_str());
        }
        std::thread([] {
            FILE* dump = std::getenv("SCIMITAR_AUDIO_DUMP") ? std::fopen(std::getenv("SCIMITAR_AUDIO_DUMP"), "wb") : nullptr;
            std::vector<float> samples(2 * (size_t)(kRate / 100));
            auto next = clock::now();
            for (;;) {
                next += std::chrono::milliseconds(10);
                std::this_thread::sleep_until(next);
                mix(dump ? samples.data() : nullptr, (uint32_t)(kRate / 100));
                if (dump) std::fwrite(samples.data(), sizeof(float), samples.size(), dump), std::fflush(dump);
            }
        }).detach();
    });
}

struct Sound : Native {};
struct Object3D : Native {
    uint32_t buffer = 0;
};

uint32_t g_vt_ds = 0, g_vt_buffer = 0, g_vt_3d = 0, g_vt_listener = 0;

Buffer* buf(Context& c) {
    Buffer* b = as<Buffer>(arg(c, 0));
    if (!b) fatal("DirectSound buffer method on %08x", arg(c, 0));
    return b;
}

void read_format(Buffer* b, uint32_t wfx) {
    if (!wfx) return;
    b->channels = rd16(wfx + 2);
    b->rate = rd(wfx + 4);
    b->block = std::max<uint32_t>(1, rd16(wfx + 12));
    b->bits = rd16(wfx + 14);
}

void ds_CreateSoundBuffer(Context& c) {
    const uint32_t desc = arg(c, 1), out = arg(c, 2);
    auto* b = new Buffer;
    b->flags = rd(desc + 4);
    b->primary = b->flags & 1;  // DSBCAPS_PRIMARYBUFFER
    b->is3d = !b->primary && (b->flags & 0x10);  // DSBCAPS_CTRL3D
    b->size = b->primary ? 65536 : rd(desc + 8);
    read_format(b, rd(desc + 16));
    b->data = galloc(b->size + 16);
    std::memset(gptr(b->data), b->bits == 8 ? 0x80 : 0, b->size);
    start_mixer();
    trace("CreateSoundBuffer(flags %08x, %u bytes, %u Hz, %u ch, %u bits)", b->flags, b->size, b->rate,
          b->channels, b->bits);
    wr(out, create(g_vt_buffer, b));
    ret(c, DS_OK, 4);
}

void ds_GetCaps(Context& c) {
    const uint32_t caps = arg(c, 1);
    const uint32_t size = rd(caps);
    std::memset(gptr(caps + 4), 0, size > 4 ? size - 4 : 0);
    wr(caps + 4, 0x00000f5f);  // primary/secondary mono, stereo, 8/16-bit, continuous rate
    wr(caps + 8, 100), wr(caps + 12, 200000);  // min/max secondary sample rate
    wr(caps + 16, 1);                          // primary buffers
    ret(c, DS_OK, 2);
}

void ds_DuplicateSoundBuffer(Context& c) {
    Buffer* src = as<Buffer>(arg(c, 1));
    if (!src) return ret(c, DSERR_INVALIDPARAM, 3);
    auto* b = new Buffer(*src);
    b->refs = 1;
    wr(arg(c, 2), create(g_vt_buffer, b));
    ret(c, DS_OK, 3);
}

void ds_SetCooperativeLevel(Context& c) { ret(c, DS_OK, 3); }
void ds_Compact(Context& c) { ret(c, DS_OK, 1); }
void ds_GetSpeakerConfig(Context& c) {
    wr(arg(c, 1), 4);  // DSSPEAKER_STEREO
    ret(c, DS_OK, 2);
}
void ds_SetSpeakerConfig(Context& c) { ret(c, DS_OK, 2); }
void ds_Initialize(Context& c) { ret(c, DS_OK, 2); }

void bf_QueryInterface(Context& c) {
    const uint32_t self = arg(c, 0), iid = arg(c, 1), out = arg(c, 2);
    const uint32_t first = rd(iid);
    if (first == 0x279afa86u || first == 0x279afa84u) {  // IID_IDirectSound3DBuffer / 3DListener
        auto* o = new Object3D;
        o->buffer = self;
        if (Buffer* b = as<Buffer>(self); b && first == 0x279afa86u && !b->primary) b->is3d = true;
        wr(out, create(first == 0x279afa86u ? g_vt_3d : g_vt_listener, o));
        return ret(c, DS_OK, 3);
    }
    if (first == 0x6825a449u || first == 0x279afa85u) return QueryInterface(c);  // IDirectSoundBuffer(8)
    trace("DirectSound buffer QueryInterface(%08x): no", first);
    wr(out, 0);
    ret(c, E_NOINTERFACE, 3);
}

void bf_GetCaps(Context& c) {
    Buffer* b = buf(c);
    const uint32_t caps = arg(c, 1);
    wr(caps + 4, b->flags), wr(caps + 8, b->size), wr(caps + 12, 0), wr(caps + 16, 0);
    ret(c, DS_OK, 2);
}

void bf_GetCurrentPosition(Context& c) {
    Buffer* b = buf(c);
    uint32_t p;
    {
        std::lock_guard<std::mutex> l(g_mix_lock);
        p = b->position();
    }
    if (arg(c, 1)) wr(arg(c, 1), p);
    // The write cursor: 15 ms ahead, where the mix has not read yet.
    if (arg(c, 2)) wr(arg(c, 2), b->size ? (p + b->block * (b->rate * 15 / 1000)) % b->size : 0);
    ret(c, DS_OK, 3);
}

void bf_GetFormat(Context& c) {
    Buffer* b = buf(c);
    const uint32_t wfx = arg(c, 1), size = arg(c, 2), written = arg(c, 3);
    if (wfx && size >= 16) {
        wr16(wfx, 1), wr16(wfx + 2, (uint16_t)b->channels), wr(wfx + 4, b->rate);
        wr(wfx + 8, b->rate * b->block), wr16(wfx + 12, (uint16_t)b->block), wr16(wfx + 14, (uint16_t)b->bits);
        if (size >= 18) wr16(wfx + 16, 0);
    }
    if (written) wr(written, 18);
    ret(c, DS_OK, 4);
}

void bf_GetVolume(Context& c) { wr(arg(c, 1), (uint32_t)buf(c)->volume); ret(c, DS_OK, 2); }
void bf_GetPan(Context& c) { wr(arg(c, 1), (uint32_t)buf(c)->pan); ret(c, DS_OK, 2); }
void bf_GetFrequency(Context& c) {
    Buffer* b = buf(c);
    wr(arg(c, 1), b->frequency ? b->frequency : b->rate);
    ret(c, DS_OK, 2);
}

void bf_GetStatus(Context& c) {
    Buffer* b = buf(c);
    std::lock_guard<std::mutex> l(g_mix_lock);
    wr(arg(c, 1), (b->playing ? 1u : 0u) | (b->playing && b->looping ? 4u : 0u));
    ret(c, DS_OK, 2);
}

void bf_Lock(Context& c) {
    // (offset, bytes, p1*, n1*, p2*, n2*, flags)
    Buffer* b = buf(c);
    uint32_t off = arg(c, 1), n = arg(c, 2);
    const uint32_t flags = arg(c, 7);
    if (flags & 1) {  // DSBLOCK_FROMWRITECURSOR
        std::lock_guard<std::mutex> l(g_mix_lock);
        off = b->position();
    }
    if (flags & 2) n = b->size;          // DSBLOCK_ENTIREBUFFER
    if (!b->size) return ret(c, DSERR_INVALIDPARAM, 8);
    off %= b->size;
    n = std::min(n, b->size);
    const uint32_t first = std::min(n, b->size - off);
    wr(arg(c, 3), b->data + off), wr(arg(c, 4), first);
    if (arg(c, 5)) wr(arg(c, 5), first < n ? b->data : 0);
    if (arg(c, 6)) wr(arg(c, 6), n - first);
    ret(c, DS_OK, 8);
}

void bf_Play(Context& c) {
    Buffer* b = buf(c);
    std::lock_guard<std::mutex> l(g_mix_lock);
    b->playing = true;
    b->looping = arg(c, 3) & 1;  // DSBPLAY_LOOPING
    ret(c, DS_OK, 4);
}

void bf_SetCurrentPosition(Context& c) {
    Buffer* b = buf(c);
    std::lock_guard<std::mutex> l(g_mix_lock);
    b->pos = b->size ? (double)(arg(c, 1) % b->size / b->block) : 0;
    ret(c, DS_OK, 2);
}

void bf_SetFormat(Context& c) {
    read_format(buf(c), arg(c, 1));
    ret(c, DS_OK, 2);
}

void bf_SetVolume(Context& c) { buf(c)->volume = (int32_t)arg(c, 1); ret(c, DS_OK, 2); }
void bf_SetPan(Context& c) { buf(c)->pan = (int32_t)arg(c, 1); ret(c, DS_OK, 2); }

void bf_SetFrequency(Context& c) {
    Buffer* b = buf(c);
    b->frequency = arg(c, 1);
    ret(c, DS_OK, 2);
}

void bf_Stop(Context& c) {
    Buffer* b = buf(c);
    std::lock_guard<std::mutex> l(g_mix_lock);
    b->playing = false;
    ret(c, DS_OK, 1);
}

void bf_Unlock(Context& c) { ret(c, DS_OK, 5); }
void bf_Restore(Context& c) { ret(c, DS_OK, 1); }
void bf_SetFX(Context& c) { ret(c, DSERR_INVALIDPARAM, 4); }

template <int N> void ok(Context& c) { ret(c, DS_OK, N); }

// 3D buffers (IDirectSound3DBuffer) and the listener (IDirectSound3DListener,
// from the primary buffer). The `apply` argument (DS3D_DEFERRED or not) is
// ignored: settings apply at once.
Params3D& p3d(Context& c) {
    Object3D* o = as<Object3D>(arg(c, 0));
    Buffer* b = o ? as<Buffer>(o->buffer) : nullptr;
    if (!b) fatal("IDirectSound3DBuffer method on %08x", arg(c, 0));
    return b->p3d;
}
Vec rd_vec(uint32_t a) {
    Vec v;
    std::memcpy(&v, gptr(a), 12);
    return v;
}
void wr_vec(uint32_t a, Vec v) { std::memcpy(gptr(a), &v, 12); }
Vec arg_vec(Context& c, int i) {
    Vec v;
    const uint32_t w[3] = {arg(c, i), arg(c, i + 1), arg(c, i + 2)};
    std::memcpy(&v, w, 12);
    return v;
}
float arg_f(Context& c, int i) {
    const uint32_t w = arg(c, i);
    float f;
    std::memcpy(&f, &w, 4);
    return f;
}
void wr_f(uint32_t a, float f) { std::memcpy(gptr(a), &f, 4); }
float rd_f(uint32_t a) {
    float f;
    std::memcpy(&f, gptr(a), 4);
    return f;
}

void b3_GetAllParameters(Context& c) {
    std::lock_guard<std::mutex> l(g_mix_lock);
    const Params3D& p = p3d(c);
    const uint32_t o = arg(c, 1);
    wr_vec(o + 4, p.position), wr_vec(o + 16, p.velocity), wr(o + 28, p.inside), wr(o + 32, p.outside);
    wr_vec(o + 36, p.cone_orientation), wr(o + 48, (uint32_t)p.cone_volume), wr_f(o + 52, p.min_distance);
    wr_f(o + 56, p.max_distance), wr(o + 60, p.mode);
    ret(c, DS_OK, 2);
}
void b3_SetAllParameters(Context& c) {
    std::lock_guard<std::mutex> l(g_mix_lock);
    Params3D& p = p3d(c);
    const uint32_t o = arg(c, 1);
    p.position = rd_vec(o + 4), p.velocity = rd_vec(o + 16), p.inside = rd(o + 28), p.outside = rd(o + 32);
    p.cone_orientation = rd_vec(o + 36), p.cone_volume = (int32_t)rd(o + 48), p.min_distance = rd_f(o + 52);
    p.max_distance = rd_f(o + 56), p.mode = rd(o + 60);
    ret(c, DS_OK, 3);
}
void b3_GetConeAngles(Context& c) {
    const Params3D& p = p3d(c);
    if (arg(c, 1)) wr(arg(c, 1), p.inside);
    if (arg(c, 2)) wr(arg(c, 2), p.outside);
    ret(c, DS_OK, 3);
}
void b3_GetConeOrientation(Context& c) { wr_vec(arg(c, 1), p3d(c).cone_orientation), ret(c, DS_OK, 2); }
void b3_GetConeOutsideVolume(Context& c) { wr(arg(c, 1), (uint32_t)p3d(c).cone_volume), ret(c, DS_OK, 2); }
void b3_GetMaxDistance(Context& c) { wr_f(arg(c, 1), p3d(c).max_distance), ret(c, DS_OK, 2); }
void b3_GetMinDistance(Context& c) { wr_f(arg(c, 1), p3d(c).min_distance), ret(c, DS_OK, 2); }
void b3_GetMode(Context& c) { wr(arg(c, 1), p3d(c).mode), ret(c, DS_OK, 2); }
void b3_GetPosition(Context& c) { wr_vec(arg(c, 1), p3d(c).position), ret(c, DS_OK, 2); }
void b3_GetVelocity(Context& c) { wr_vec(arg(c, 1), p3d(c).velocity), ret(c, DS_OK, 2); }
void b3_SetConeAngles(Context& c) {
    Params3D& p = p3d(c);
    p.inside = arg(c, 1), p.outside = arg(c, 2);
    ret(c, DS_OK, 4);
}
void b3_SetConeOrientation(Context& c) { p3d(c).cone_orientation = arg_vec(c, 1), ret(c, DS_OK, 5); }
void b3_SetConeOutsideVolume(Context& c) { p3d(c).cone_volume = (int32_t)arg(c, 1), ret(c, DS_OK, 3); }
void b3_SetMaxDistance(Context& c) { p3d(c).max_distance = arg_f(c, 1), ret(c, DS_OK, 3); }
void b3_SetMinDistance(Context& c) { p3d(c).min_distance = arg_f(c, 1), ret(c, DS_OK, 3); }
void b3_SetMode(Context& c) { p3d(c).mode = arg(c, 1), ret(c, DS_OK, 3); }
void b3_SetPosition(Context& c) {
    std::lock_guard<std::mutex> l(g_mix_lock);
    p3d(c).position = arg_vec(c, 1);
    ret(c, DS_OK, 5);
}
void b3_SetVelocity(Context& c) { p3d(c).velocity = arg_vec(c, 1), ret(c, DS_OK, 5); }

void ls_GetAllParameters(Context& c) {
    const Listener& L = g_listener;
    const uint32_t o = arg(c, 1);
    wr_vec(o + 4, L.position), wr_vec(o + 16, L.velocity), wr_vec(o + 28, L.front), wr_vec(o + 40, L.top);
    wr_f(o + 52, L.distance_factor), wr_f(o + 56, L.rolloff), wr_f(o + 60, L.doppler);
    ret(c, DS_OK, 2);
}
void ls_SetAllParameters(Context& c) {
    std::lock_guard<std::mutex> l(g_mix_lock);
    Listener& L = g_listener;
    const uint32_t o = arg(c, 1);
    L.position = rd_vec(o + 4), L.velocity = rd_vec(o + 16), L.front = rd_vec(o + 28), L.top = rd_vec(o + 40);
    L.distance_factor = rd_f(o + 52), L.rolloff = rd_f(o + 56), L.doppler = rd_f(o + 60);
    ret(c, DS_OK, 3);
}
void ls_GetDistanceFactor(Context& c) { wr_f(arg(c, 1), g_listener.distance_factor), ret(c, DS_OK, 2); }
void ls_GetDopplerFactor(Context& c) { wr_f(arg(c, 1), g_listener.doppler), ret(c, DS_OK, 2); }
void ls_GetOrientation(Context& c) {
    wr_vec(arg(c, 1), g_listener.front), wr_vec(arg(c, 2), g_listener.top);
    ret(c, DS_OK, 3);
}
void ls_GetPosition(Context& c) { wr_vec(arg(c, 1), g_listener.position), ret(c, DS_OK, 2); }
void ls_GetRolloffFactor(Context& c) { wr_f(arg(c, 1), g_listener.rolloff), ret(c, DS_OK, 2); }
void ls_GetVelocity(Context& c) { wr_vec(arg(c, 1), g_listener.velocity), ret(c, DS_OK, 2); }
void ls_SetDistanceFactor(Context& c) { g_listener.distance_factor = arg_f(c, 1), ret(c, DS_OK, 3); }
void ls_SetDopplerFactor(Context& c) { g_listener.doppler = arg_f(c, 1), ret(c, DS_OK, 3); }
void ls_SetOrientation(Context& c) {
    std::lock_guard<std::mutex> l(g_mix_lock);
    g_listener.front = arg_vec(c, 1), g_listener.top = arg_vec(c, 4);
    ret(c, DS_OK, 8);
}
void ls_SetPosition(Context& c) {
    std::lock_guard<std::mutex> l(g_mix_lock);
    g_listener.position = arg_vec(c, 1);
    ret(c, DS_OK, 5);
}
void ls_SetRolloffFactor(Context& c) { g_listener.rolloff = arg_f(c, 1), ret(c, DS_OK, 3); }
void ls_SetVelocity(Context& c) { g_listener.velocity = arg_vec(c, 1), ret(c, DS_OK, 5); }
void bf_GetObjectInPath(Context& c) {
    if (arg(c, 4)) wr(arg(c, 4), 0);
    ret(c, 0x88780163u, 5);  // DSERR_OBJECTNOTFOUND: no effects
}

void make_vtables() {
    if (g_vt_ds) return;
    g_vt_ds = make_vtable("IDirectSound8", {
        {"QueryInterface", QueryInterface}, {"AddRef", AddRef}, {"Release", Release},
        {"CreateSoundBuffer", ds_CreateSoundBuffer}, {"GetCaps", ds_GetCaps},
        {"DuplicateSoundBuffer", ds_DuplicateSoundBuffer}, {"SetCooperativeLevel", ds_SetCooperativeLevel},
        {"Compact", ds_Compact}, {"GetSpeakerConfig", ds_GetSpeakerConfig},
        {"SetSpeakerConfig", ds_SetSpeakerConfig}, {"Initialize", ds_Initialize}, {"VerifyCertification", nullptr}});
    g_vt_buffer = make_vtable("IDirectSoundBuffer8", {
        {"QueryInterface", bf_QueryInterface}, {"AddRef", AddRef}, {"Release", Release},
        {"GetCaps", bf_GetCaps}, {"GetCurrentPosition", bf_GetCurrentPosition}, {"GetFormat", bf_GetFormat},
        {"GetVolume", bf_GetVolume}, {"GetPan", bf_GetPan}, {"GetFrequency", bf_GetFrequency},
        {"GetStatus", bf_GetStatus}, {"Initialize", ok<3>}, {"Lock", bf_Lock}, {"Play", bf_Play},
        {"SetCurrentPosition", bf_SetCurrentPosition}, {"SetFormat", bf_SetFormat}, {"SetVolume", bf_SetVolume},
        {"SetPan", bf_SetPan}, {"SetFrequency", bf_SetFrequency}, {"Stop", bf_Stop}, {"Unlock", bf_Unlock},
        {"Restore", bf_Restore}, {"SetFX", bf_SetFX}, {"AcquireResources", ok<4>}, {"GetObjectInPath", bf_GetObjectInPath}});
    g_vt_3d = make_vtable("IDirectSound3DBuffer", {
        {"QueryInterface", QueryInterface}, {"AddRef", AddRef}, {"Release", Release},
        {"GetAllParameters", b3_GetAllParameters}, {"GetConeAngles", b3_GetConeAngles},
        {"GetConeOrientation", b3_GetConeOrientation}, {"GetConeOutsideVolume", b3_GetConeOutsideVolume},
        {"GetMaxDistance", b3_GetMaxDistance}, {"GetMinDistance", b3_GetMinDistance}, {"GetMode", b3_GetMode},
        {"GetPosition", b3_GetPosition}, {"GetVelocity", b3_GetVelocity},
        {"SetAllParameters", b3_SetAllParameters}, {"SetConeAngles", b3_SetConeAngles},
        {"SetConeOrientation", b3_SetConeOrientation}, {"SetConeOutsideVolume", b3_SetConeOutsideVolume},
        {"SetMaxDistance", b3_SetMaxDistance}, {"SetMinDistance", b3_SetMinDistance}, {"SetMode", b3_SetMode},
        {"SetPosition", b3_SetPosition}, {"SetVelocity", b3_SetVelocity}});
    g_vt_listener = make_vtable("IDirectSound3DListener", {
        {"QueryInterface", QueryInterface}, {"AddRef", AddRef}, {"Release", Release},
        {"GetAllParameters", ls_GetAllParameters}, {"GetDistanceFactor", ls_GetDistanceFactor},
        {"GetDopplerFactor", ls_GetDopplerFactor}, {"GetOrientation", ls_GetOrientation},
        {"GetPosition", ls_GetPosition}, {"GetRolloffFactor", ls_GetRolloffFactor},
        {"GetVelocity", ls_GetVelocity}, {"SetAllParameters", ls_SetAllParameters},
        {"SetDistanceFactor", ls_SetDistanceFactor}, {"SetDopplerFactor", ls_SetDopplerFactor},
        {"SetOrientation", ls_SetOrientation}, {"SetPosition", ls_SetPosition},
        {"SetRolloffFactor", ls_SetRolloffFactor}, {"SetVelocity", ls_SetVelocity},
        {"CommitDeferredSettings", ok<1>}});
}

}  // namespace scimitar::win32::dsound

namespace scimitar::imports {
using namespace scimitar::win32;

W32(dsound, DirectSoundCreate8) {
    // (guid, out, outer)
    dsound::make_vtables();
    trace("DirectSoundCreate8");
    wr(arg(c, 1), com::create(dsound::g_vt_ds, new dsound::Sound));
    ret(c, 0, 3);
}

W32(dsound, DirectSoundEnumerateA) {
    // (callback(guid, description, module, context), context): the primary device.
    const uint32_t cb = arg(c, 0), ctx = arg(c, 1);
    static const uint32_t desc = guest_strdup("Primary Sound Driver"), module = guest_strdup("");
    if (cb) call_guest(c, cb, {0, desc, module, ctx});
    ret(c, 0, 2);
}

W32(eax, EAXDirectSoundCreate8) {
    trace("EAXDirectSoundCreate8: no EAX");
    if (arg(c, 1)) wr(arg(c, 1), 0);
    ret(c, dsound::DSERR_NODRIVER, 3);
}

}  // namespace scimitar::imports
