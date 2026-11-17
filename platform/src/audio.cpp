// AudioToolbox (AUGraph), AudioUnit and CoreAudio imports: Wwise's CoreAudio
// sink builds a graph (output unit, render callback) and the host plays it.
// Graphs, nodes and units are host objects behind guest handles. The guest's
// render callback runs on the host's audio thread with a guest context of its
// own; its AudioBufferList (whose buffer pointers are 32 bits in the guest) is
// a guest copy, written back to the host buffers after the call. Property data
// is passed through: the formats Wwise sets and reads (stream descriptions,
// frame counts, device IDs) contain no pointers.
//
// iOS has no CoreAudio HAL (AudioObject*): there the device Wwise asks about is
// a fixed one (48 kHz, stereo float), and the Mac output units become RemoteIO.
#include <TargetConditionals.h>
#include <AudioToolbox/AudioToolbox.h>
#include <AudioUnit/AudioUnit.h>
#if !TARGET_OS_IPHONE
#include <CoreAudio/CoreAudio.h>
#endif

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <cmath>
#include <map>
#include <string>
#include <memory>
#include <mutex>
#include <vector>

#include "scimitar/guest_heap.h"
#include "scimitar/imports.h"
#include "scimitar/runtime.h"

namespace scimitar::imports {

namespace {

std::mutex g_lock;
std::map<uint32_t, AUGraph> g_graphs;
std::map<uint32_t, AudioUnit> g_units;
uint32_t g_next = 0x0a0d0001;

uint32_t handle_for_unit(AudioUnit u) {
    std::lock_guard<std::mutex> l(g_lock);
    for (const auto& [h, v] : g_units)
        if (v == u) return h;
    g_units[g_next] = u;
    return g_next++;
}
AUGraph graph(uint32_t h) {
    std::lock_guard<std::mutex> l(g_lock);
    auto it = g_graphs.find(h);
    if (it == g_graphs.end()) fatal("not an AUGraph handle: %08x", h);
    return it->second;
}
AudioUnit unit(uint32_t h) {
    std::lock_guard<std::mutex> l(g_lock);
    auto it = g_units.find(h);
    if (it == g_units.end()) fatal("not an AudioUnit handle: %08x", h);
    return it->second;
}

// SCIMITAR_TRACE_AUDIO=1: every call with its result, the stream formats set
// and read, and the first render callbacks.
const bool g_trace = std::getenv("SCIMITAR_TRACE_AUDIO") != nullptr;
std::string fourcc(uint32_t v) {
    std::string s;
    for (int k = 24; k >= 0; k -= 8) {
        const char ch = (char)(v >> k);
        s += ch >= 32 && ch < 127 ? ch : '?';
    }
    return s;
}
void trace_format(const char* what, const void* p, uint32_t size) {
    if (!g_trace || size < sizeof(AudioStreamBasicDescription)) return;
    const AudioStreamBasicDescription& f = *(const AudioStreamBasicDescription*)p;
    std::fprintf(stderr, "audio:   %s: %.0f Hz '%s' flags 0x%x, %u bytes/packet, %u frames/packet, %u bytes/frame, "
                 "%u channels, %u bits\n", what, f.mSampleRate, fourcc(f.mFormatID).c_str(), (unsigned)f.mFormatFlags,
                 (unsigned)f.mBytesPerPacket, (unsigned)f.mFramesPerPacket, (unsigned)f.mBytesPerFrame,
                 (unsigned)f.mChannelsPerFrame, (unsigned)f.mBitsPerChannel);
}
// Returns an OSStatus to the guest, traced.
void done(Context& c, const char* fn, OSStatus r) {
    if (g_trace) std::fprintf(stderr, "audio: %s -> %d%s\n", fn, (int)r, r ? (" '" + fourcc((uint32_t)r) + "'").c_str() : "");
    shim_return(c, (uint32_t)r);
}

// A guest render callback: proc(refCon, ioActionFlags*, const AudioTimeStamp*,
// inBusNumber, inNumberFrames, AudioBufferList* ioData).
struct Callback {
    GuestMemory* mem;
    uint32_t proc, refcon;
};

// The audio thread's guest context and scratch (flags, timestamp, buffer list
// and sample buffers), made on first use.
struct AudioThread {
    Context c;
    uint32_t stack = 0, scratch = 0, scratch_size = 0;
};

OSStatus render(void* ref, AudioUnitRenderActionFlags* flags, const AudioTimeStamp* ts, UInt32 bus,
                UInt32 frames, AudioBufferList* io) {
    Callback* cb = static_cast<Callback*>(ref);
    thread_local AudioThread t;
    GuestMemory& m = *cb->mem;
    if (!t.stack) {
        t.stack = heap::alloc(m, 1u << 20, 16);
        t.c.mem = &m;
        set_thread_context(&t.c);
    }
    // Scratch: flags (16), timestamp (64), list header + buffers, sample data.
    uint32_t data_bytes = 0;
    for (UInt32 k = 0; io && k < io->mNumberBuffers; k++) data_bytes += io->mBuffers[k].mDataByteSize;
    const uint32_t list_bytes = 8 + 12 * (io ? io->mNumberBuffers : 0);
    const uint32_t need = 16 + 64 + ((list_bytes + 15) & ~15u) + data_bytes;
    if (need > t.scratch_size) {
        heap::release(m, t.scratch);
        t.scratch = heap::alloc(m, need * 2, 16);
        t.scratch_size = need * 2;
    }
    const uint32_t gflags = t.scratch, gts = gflags + 16, glist = gts + 64;
    uint32_t gdata = glist + ((list_bytes + 15) & ~15u);
    m.write<uint32_t>(gflags, flags ? *flags : 0);
    std::memcpy(m.ptr(gts), ts, 64);
    if (io) {
        m.write<uint32_t>(glist, io->mNumberBuffers);
        for (UInt32 k = 0; k < io->mNumberBuffers; k++) {
            const uint32_t b = glist + 4 + 12 * k;
            m.write<uint32_t>(b, io->mBuffers[k].mNumberChannels);
            m.write<uint32_t>(b + 4, io->mBuffers[k].mDataByteSize);
            m.write<uint32_t>(b + 8, gdata);
            std::memset(m.ptr(gdata), 0, io->mBuffers[k].mDataByteSize);
            gdata += io->mBuffers[k].mDataByteSize;
        }
    }
    t.c.esp = ((t.stack + (1u << 20)) & ~15u) - 12;
    const OSStatus r = (OSStatus)call_guest(t.c, cb->proc, {cb->refcon, gflags, gts, bus, frames, io ? glist : 0});
    static std::atomic<uint64_t> calls{0};
    const uint64_t n = calls++;
    if (g_trace && (n < 3 || n % 500 == 0)) {
        // The loudest sample the guest wrote (float samples, as Wwise renders).
        float peak = 0;
        for (UInt32 k = 0; io && k < io->mNumberBuffers; k++) {
            const uint32_t b = glist + 4 + 12 * k;
            const float* x = (const float*)m.ptr(m.read<uint32_t>(b + 8));
            for (uint32_t i = 0; i < m.read<uint32_t>(b + 4) / 4; i++) peak = std::max(peak, std::fabs(x[i]));
        }
        std::fprintf(stderr, "audio: render #%llu: bus %u, %u frames, %u buffers (%u channels, %u bytes) -> %d, peak %.4f\n",
                     (unsigned long long)n, (unsigned)bus, (unsigned)frames, io ? (unsigned)io->mNumberBuffers : 0,
                     io && io->mNumberBuffers ? (unsigned)io->mBuffers[0].mNumberChannels : 0,
                     io && io->mNumberBuffers ? (unsigned)io->mBuffers[0].mDataByteSize : 0, (int)r, peak);
    }
    if (flags) *flags = m.read<uint32_t>(gflags);
    if (io)
        for (UInt32 k = 0; k < io->mNumberBuffers; k++) {
            const uint32_t b = glist + 4 + 12 * k;
            const uint32_t size = std::min<uint32_t>(m.read<uint32_t>(b + 4), io->mBuffers[k].mDataByteSize);
            static const bool mute = std::getenv("SCIMITAR_MUTE") != nullptr;  // tests: same work, no sound
            if (mute) std::memset(io->mBuffers[k].mData, 0, size);
            else std::memcpy(io->mBuffers[k].mData, m.ptr(m.read<uint32_t>(b + 8)), size);
            io->mBuffers[k].mDataByteSize = size;
        }
    return r;
}

std::vector<std::unique_ptr<Callback>> g_callbacks;  // kept while graphs live

}  // namespace

// OSStatus NewAUGraph(AUGraph* out)
void imp_NewAUGraph(Context& c) {
    AUGraph g = nullptr;
    const OSStatus r = NewAUGraph(&g);
    if (r == noErr) {
        std::lock_guard<std::mutex> l(g_lock);
        g_graphs[g_next] = g;
        c.mem->write<uint32_t>(arg32(c, 0), g_next++);
    }
    done(c, "NewAUGraph", (OSStatus)r);
}
// OSStatus AUGraphAddNode(AUGraph, const AudioComponentDescription*, AUNode* out)
void imp_AUGraphAddNode(Context& c) {
    AUNode n = 0;
    AudioComponentDescription d = *(const AudioComponentDescription*)c.mem->ptr(arg32(c, 1));
#if TARGET_OS_IPHONE
    constexpr OSType kHALOutput = 'ahal', kDefaultOutput = 'def ', kSystemOutput = 'sys ';
    if (d.componentType == kAudioUnitType_Output &&
        (d.componentSubType == kHALOutput || d.componentSubType == kDefaultOutput || d.componentSubType == kSystemOutput))
        d.componentSubType = kAudioUnitSubType_RemoteIO;
#endif
    const OSStatus r = AUGraphAddNode(graph(arg32(c, 0)), &d, &n);
    if (g_trace) std::fprintf(stderr, "audio:   node '%s' '%s' '%s'\n", fourcc(d.componentType).c_str(),
                              fourcc(d.componentSubType).c_str(), fourcc(d.componentManufacturer).c_str());
    c.mem->write<uint32_t>(arg32(c, 2), (uint32_t)n);
    done(c, "AUGraphAddNode", (OSStatus)r);
}
void imp_AUGraphOpen(Context& c) { done(c, "AUGraphOpen", (OSStatus)AUGraphOpen(graph(arg32(c, 0)))); }
void imp_AUGraphClose(Context& c) { done(c, "AUGraphClose", (OSStatus)AUGraphClose(graph(arg32(c, 0)))); }
void imp_AUGraphInitialize(Context& c) { done(c, "AUGraphInitialize", (OSStatus)AUGraphInitialize(graph(arg32(c, 0)))); }
void imp_AUGraphStart(Context& c) { done(c, "AUGraphStart", (OSStatus)AUGraphStart(graph(arg32(c, 0)))); }
void imp_AUGraphStop(Context& c) { done(c, "AUGraphStop", (OSStatus)AUGraphStop(graph(arg32(c, 0)))); }
void imp_DisposeAUGraph(Context& c) {
    const OSStatus r = DisposeAUGraph(graph(arg32(c, 0)));
    std::lock_guard<std::mutex> l(g_lock);
    g_graphs.erase(arg32(c, 0));
    done(c, "DisposeAUGraph", (OSStatus)r);
}
// OSStatus AUGraphNodeInfo(AUGraph, AUNode, AudioComponentDescription* out, AudioUnit* out)
void imp_AUGraphNodeInfo(Context& c) {
    AudioUnit u = nullptr;
    const OSStatus r = AUGraphNodeInfo(graph(arg32(c, 0)), (AUNode)arg32(c, 1),
                                       arg32(c, 2) ? (AudioComponentDescription*)c.mem->ptr(arg32(c, 2)) : nullptr, &u);
    if (arg32(c, 3)) c.mem->write<uint32_t>(arg32(c, 3), u ? handle_for_unit(u) : 0);
    done(c, "AUGraphNodeInfo", (OSStatus)r);
}
// OSStatus AUGraphConnectNodeInput(AUGraph, AUNode src, UInt32 srcOut, AUNode dst, UInt32 dstIn)
void imp_AUGraphConnectNodeInput(Context& c) {
    done(c, "AUGraphConnectNodeInput", (OSStatus)AUGraphConnectNodeInput(graph(arg32(c, 0)), (AUNode)arg32(c, 1), arg32(c, 2),
                                                     (AUNode)arg32(c, 3), arg32(c, 4)));
}
// OSStatus AUGraphSetNodeInputCallback(AUGraph, AUNode, UInt32 bus, const AURenderCallbackStruct*)
void imp_AUGraphSetNodeInputCallback(Context& c) {
    const uint32_t s = arg32(c, 3);
    auto cb = std::make_unique<Callback>(Callback{c.mem, c.mem->read<uint32_t>(s), c.mem->read<uint32_t>(s + 4)});
    AURenderCallbackStruct host{render, cb.get()};
    {
        std::lock_guard<std::mutex> l(g_lock);
        g_callbacks.push_back(std::move(cb));
    }
    done(c, "AUGraphSetNodeInputCallback", (OSStatus)AUGraphSetNodeInputCallback(graph(arg32(c, 0)), (AUNode)arg32(c, 1), arg32(c, 2), &host));
}

// OSStatus AudioUnitSetProperty(AudioUnit, AudioUnitPropertyID, scope, element, const void*, UInt32)
void imp_AudioUnitSetProperty(Context& c) {
    const AudioUnitPropertyID id = arg32(c, 1);
    if (g_trace) {
        std::fprintf(stderr, "audio:   set property %u, scope %u, element %u, %u bytes\n", (unsigned)id, arg32(c, 2),
                     arg32(c, 3), arg32(c, 5));
        if (id == kAudioUnitProperty_StreamFormat && arg32(c, 4)) trace_format("format set", c.mem->ptr(arg32(c, 4)), arg32(c, 5));
    }
    if (id == kAudioUnitProperty_SetRenderCallback) fatal("AudioUnitSetProperty: render callback property");
#if TARGET_OS_IPHONE
    constexpr AudioUnitPropertyID kCurrentDevice = 2000;  // kAudioOutputUnitProperty_CurrentDevice (HAL units)
    if (id == kCurrentDevice) return done(c, "AudioUnitSetProperty", 0);
#endif
    done(c, "AudioUnitSetProperty", (OSStatus)AudioUnitSetProperty(unit(arg32(c, 0)), id, arg32(c, 2), arg32(c, 3),
                                                  arg32(c, 4) ? c.mem->ptr(arg32(c, 4)) : nullptr, arg32(c, 5)));
}
// OSStatus AudioUnitGetProperty(AudioUnit, AudioUnitPropertyID, scope, element, void*, UInt32* ioSize)
void imp_AudioUnitGetProperty(Context& c) {
    UInt32 size = c.mem->read<uint32_t>(arg32(c, 5));
    const OSStatus r = AudioUnitGetProperty(unit(arg32(c, 0)), arg32(c, 1), arg32(c, 2), arg32(c, 3),
                                            c.mem->ptr(arg32(c, 4)), &size);
    c.mem->write<uint32_t>(arg32(c, 5), size);
    if (g_trace) {
        std::fprintf(stderr, "audio:   get property %u, scope %u, element %u, %u bytes\n", arg32(c, 1), arg32(c, 2),
                     arg32(c, 3), (unsigned)size);
        if (arg32(c, 1) == kAudioUnitProperty_StreamFormat && !r) trace_format("format read", c.mem->ptr(arg32(c, 4)), size);
    }
    done(c, "AudioUnitGetProperty", (OSStatus)r);
}
void imp_AudioUnitUninitialize(Context& c) { done(c, "AudioUnitUninitialize", (OSStatus)AudioUnitUninitialize(unit(arg32(c, 0)))); }
void imp_AudioOutputUnitStop(Context& c) { done(c, "AudioOutputUnitStop", (OSStatus)AudioOutputUnitStop(unit(arg32(c, 0)))); }

#if TARGET_OS_IPHONE
// OSStatus AudioObjectGetPropertyDataSize(AudioObjectID, const AudioObjectPropertyAddress*,
//                                         UInt32 qualifierSize, const void* qualifier, UInt32* outSize)
// OSStatus AudioObjectGetPropertyData(AudioObjectID, const AudioObjectPropertyAddress*,
//                                     UInt32 qualifierSize, const void* qualifier, UInt32* ioSize, void* out)
// The one output device (1) and its properties; other properties are unknown.
namespace {
constexpr uint32_t kDevice = 1;
constexpr OSStatus kUnknownProperty = 'who?';
// The value of a property as bytes, or false for one the device does not have.
bool device_property(uint32_t object, uint32_t selector, std::vector<uint8_t>* out) {
    auto put = [&](const auto& v) {
        out->assign((const uint8_t*)&v, (const uint8_t*)&v + sizeof v);
        return true;
    };
    switch (selector) {
    case 'dOut': case 'sOut': return put(kDevice);    // default (system) output device
    case 'dev#': return put(kDevice);                 // the device list
    case 'nsrt': return put(48000.0);                 // nominal sample rate (Float64)
    case 'fsiz': return put((uint32_t)512);           // buffer frame size
    case 'fsz#': {                                    // buffer frame size range
        const double r[2] = {64, 4096};
        out->assign((const uint8_t*)r, (const uint8_t*)r + sizeof r);
        return true;
    }
    case 'srnd': {  // preferred channel layout: stereo, as a Mac's built-in output answers
        // (Wwise sets up its speakers from it, and mixes silence without it)
        const uint32_t layout[13] = {0 /* use descriptions */, 0, 2, 1 /* left */, 0, 0, 0, 0, 2 /* right */, 0, 0, 0, 0};
        out->assign((const uint8_t*)layout, (const uint8_t*)layout + sizeof layout);
        return true;
    }
    case 'ltnc': case 'saft': return put((uint32_t)0);  // latency, safety offset
    case 'alv?': case 'goin': return put((uint32_t)1);   // alive, running somewhere
    }
    (void)object;
    return false;
}
void unknown_property(Context& c, const char* fn) {
    const uint32_t sel = c.mem->read<uint32_t>(arg32(c, 1));
    static const bool verbose = std::getenv("SCIMITAR_VERBOSE") != nullptr;
    if (verbose)
        std::fprintf(stderr, "audio: %s(%u, '%c%c%c%c'): unknown property\n", fn, arg32(c, 0), (char)(sel >> 24),
                     (char)(sel >> 16), (char)(sel >> 8), (char)sel);
}
}  // namespace

void imp_AudioObjectGetPropertyDataSize(Context& c) {
    std::vector<uint8_t> v;
    if (!device_property(arg32(c, 0), c.mem->read<uint32_t>(arg32(c, 1)), &v)) {
        unknown_property(c, "AudioObjectGetPropertyDataSize");
        return done(c, "AudioObjectGetPropertyDataSize", (OSStatus)kUnknownProperty);
    }
    c.mem->write<uint32_t>(arg32(c, 4), (uint32_t)v.size());
    done(c, "AudioObjectGetPropertyDataSize", 0);
}
void imp_AudioObjectGetPropertyData(Context& c) {
    if (g_trace) std::fprintf(stderr, "audio:   device property '%s'\n", fourcc(c.mem->read<uint32_t>(arg32(c, 1))).c_str());
    std::vector<uint8_t> v;
    if (!device_property(arg32(c, 0), c.mem->read<uint32_t>(arg32(c, 1)), &v)) {
        unknown_property(c, "AudioObjectGetPropertyData");
        return done(c, "AudioObjectGetPropertyData", (OSStatus)kUnknownProperty);
    }
    const uint32_t size = std::min<uint32_t>((uint32_t)v.size(), c.mem->read<uint32_t>(arg32(c, 4)));
    std::memcpy(c.mem->ptr(arg32(c, 5)), v.data(), size);
    c.mem->write<uint32_t>(arg32(c, 4), size);
    done(c, "AudioObjectGetPropertyData", 0);
}

#else
// OSStatus AudioObjectGetPropertyDataSize(AudioObjectID, const AudioObjectPropertyAddress*,
//                                         UInt32 qualifierSize, const void* qualifier, UInt32* outSize)
void imp_AudioObjectGetPropertyDataSize(Context& c) {
    UInt32 size = 0;
    const OSStatus r = AudioObjectGetPropertyDataSize(arg32(c, 0), (const AudioObjectPropertyAddress*)c.mem->ptr(arg32(c, 1)),
                                                      arg32(c, 2), arg32(c, 3) ? c.mem->ptr(arg32(c, 3)) : nullptr, &size);
    c.mem->write<uint32_t>(arg32(c, 4), size);
    done(c, "AudioObjectGetPropertyDataSize", (OSStatus)r);
}
// OSStatus AudioObjectGetPropertyData(AudioObjectID, const AudioObjectPropertyAddress*,
//                                     UInt32 qualifierSize, const void* qualifier, UInt32* ioSize, void* out)
void imp_AudioObjectGetPropertyData(Context& c) {
    const AudioObjectPropertyAddress* a = (const AudioObjectPropertyAddress*)c.mem->ptr(arg32(c, 1));
    UInt32 size = c.mem->read<uint32_t>(arg32(c, 4));
    if (a->mSelector == kAudioDevicePropertyStreamConfiguration)
        fatal("AudioObjectGetPropertyData: stream configuration (AudioBufferList) is not translated");
    const OSStatus r = AudioObjectGetPropertyData(arg32(c, 0), a, arg32(c, 2), arg32(c, 3) ? c.mem->ptr(arg32(c, 3)) : nullptr,
                                                  &size, c.mem->ptr(arg32(c, 5)));
    c.mem->write<uint32_t>(arg32(c, 4), size);
    if (g_trace) {
        std::fprintf(stderr, "audio:   device %u property '%s' scope '%s': %u bytes:", arg32(c, 0), fourcc(a->mSelector).c_str(),
                     fourcc(a->mScope).c_str(), (unsigned)size);
        for (UInt32 k = 0; k < size && k < 64; k += 4) std::fprintf(stderr, " %08x", c.mem->read<uint32_t>(arg32(c, 5) + k));
        std::fprintf(stderr, "\n");
    }
    done(c, "AudioObjectGetPropertyData", (OSStatus)r);
}

#endif

}  // namespace scimitar::imports

namespace scimitar {
namespace {
using namespace imports;
#define S(N) {"_" #N, imp_##N},
const ImportShim kAudio[] = {
    S(NewAUGraph) S(AUGraphAddNode) S(AUGraphOpen) S(AUGraphClose) S(AUGraphInitialize) S(AUGraphStart)
    S(AUGraphStop) S(DisposeAUGraph) S(AUGraphNodeInfo) S(AUGraphConnectNodeInput) S(AUGraphSetNodeInputCallback)
    S(AudioUnitSetProperty) S(AudioUnitGetProperty) S(AudioUnitUninitialize) S(AudioOutputUnitStop)
    S(AudioObjectGetPropertyDataSize) S(AudioObjectGetPropertyData)
};
#undef S
SCIMITAR_REGISTER_SHIMS(kAudio);
}  // namespace
}  // namespace scimitar
