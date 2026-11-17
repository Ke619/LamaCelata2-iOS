// Bink video imports (libBinkMachOx86.dylib), implemented with FFmpeg: the bink
// demuxer, the binkvideo and binkaudio decoders, swscale for the 32-bit BGRA
// copies the game asks for and swresample for audio, which plays through a host
// output unit: every audio track the game selected (BinkSetSoundTrack; the
// Revelations videos have music and effects on track 0 and a voice track per
// language), mixed. The game sees a BINK structure in guest memory with the fields
// it reads (Width, Height, Frames, FrameNum, FrameRate, FrameRateDiv) and
// drives playback the usual way: BinkWait / BinkDoFrame / BinkCopyToBuffer /
// BinkNextFrame. Frames are paced by the wall clock from the first frame on.
//
// Built without FFmpeg (SCIMITAR_HAVE_FFMPEG unset), BinkOpen fails as for a
// missing file and the game goes on without its videos.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "scimitar/guest_heap.h"
#include "scimitar/imports.h"
#include "scimitar/runtime.h"

#ifdef SCIMITAR_HAVE_FFMPEG
#include <AudioToolbox/AudioToolbox.h>

#include <chrono>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}
#endif

namespace scimitar::imports {

namespace {

const bool g_verbose = std::getenv("SCIMITAR_VERBOSE") != nullptr;

// BINK fields the game reads (Bink 1.x layout).
constexpr uint32_t kWidth = 0x00, kHeight = 0x04, kFrames = 0x08, kFrameNum = 0x0c, kLastFrameNum = 0x10,
                   kFrameRate = 0x14, kFrameRateDiv = 0x18, kBinkSize = 0x200;

#ifdef SCIMITAR_HAVE_FFMPEG

using Clock = std::chrono::steady_clock;

// Audio of one video: interleaved stereo float at the first track's rate,
// pulled by a host output unit from a queue per track the decoder fills, the
// tracks mixed.
struct AudioOut {
    AudioComponentInstance unit = nullptr;
    std::mutex lock;
    std::vector<std::deque<float>> tracks;
    std::vector<float> volumes;
    bool paused = false;

    static OSStatus render(void* self, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32, UInt32 frames,
                           AudioBufferList* out) {
        auto* a = static_cast<AudioOut*>(self);
        float* dst = static_cast<float*>(out->mBuffers[0].mData);
        std::lock_guard<std::mutex> l(a->lock);
        static const bool mute = std::getenv("SCIMITAR_MUTE") != nullptr;  // tests: no sound
        for (UInt32 k = 0; k < frames * 2; k++) {
            float v = 0;
            if (!a->paused)
                for (size_t t = 0; t < a->tracks.size(); t++)
                    if (!a->tracks[t].empty()) {
                        v += a->tracks[t].front() * a->volumes[t];
                        a->tracks[t].pop_front();
                    }
            dst[k] = mute ? 0.0f : std::clamp(v, -1.0f, 1.0f);
        }
        return noErr;
    }
    bool start(int rate) {
#if TARGET_OS_IPHONE
        AudioComponentDescription d{kAudioUnitType_Output, kAudioUnitSubType_RemoteIO,
#else
        AudioComponentDescription d{kAudioUnitType_Output, kAudioUnitSubType_DefaultOutput,
#endif
                                    kAudioUnitManufacturer_Apple, 0, 0};
        AudioComponent comp = AudioComponentFindNext(nullptr, &d);
        if (!comp || AudioComponentInstanceNew(comp, &unit) != noErr) return false;
        AudioStreamBasicDescription f{};
        f.mSampleRate = rate;
        f.mFormatID = kAudioFormatLinearPCM;
        f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
        f.mChannelsPerFrame = 2;
        f.mBitsPerChannel = 32;
        f.mFramesPerPacket = 1;
        f.mBytesPerFrame = f.mBytesPerPacket = 8;
        AURenderCallbackStruct cb{render, this};
        if (AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f) ||
            AudioUnitSetProperty(unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb) ||
            AudioUnitInitialize(unit) || AudioOutputUnitStart(unit))
            return false;
        return true;
    }
    ~AudioOut() {
        if (unit) {
            AudioOutputUnitStop(unit);
            AudioUnitUninitialize(unit);
            AudioComponentInstanceDispose(unit);
        }
    }
};

// One audio track that plays: its stream, decoder and conversion to the
// output's stereo float.
struct Track {
    int stream = -1;
    uint32_t id = 0;  // the Bink track id
    AVCodecContext* dec = nullptr;
    SwrContext* swr = nullptr;
};

struct Video {
    uint32_t guest = 0;  // the BINK structure
    AVFormatContext* fmt = nullptr;
    AVCodecContext* vdec = nullptr;
    int vstream = -1;
    std::vector<Track> tracks;
    AVFrame* frame = nullptr;  // the current decoded video frame
    AVFrame* scratch = nullptr;
    AVPacket* packet = nullptr;
    SwsContext* sws = nullptr;
    std::unique_ptr<AudioOut> audio;
    int width = 0, height = 0;
    uint32_t frames = 0, num = 1, den = 1;
    uint32_t decoded = 0;  // frame number in `frame` (1-based), 0 for none
    // BinkRegisterFrameBuffers: the game's Y, cR, cB and A planes of its first
    // buffer set ({address, pitch}, address 0 for none), which BinkDoFrame
    // fills with the decoded frame.
    uint32_t planes[4][2] = {};
    bool registered = false;
    bool started = false, paused = false, ended = false;
    Clock::time_point start;
    Clock::duration paused_for{};
    Clock::time_point paused_at;

    ~Video() {
        audio.reset();
        sws_freeContext(sws);
        for (Track& t : tracks) swr_free(&t.swr), avcodec_free_context(&t.dec);
        av_frame_free(&frame);
        av_frame_free(&scratch);
        av_packet_free(&packet);
        avcodec_free_context(&vdec);
        avformat_close_input(&fmt);
    }

    // Seconds since the first frame, not counting pauses.
    double clock() const {
        if (!started) return 0;
        const auto now = paused ? paused_at : Clock::now();
        return std::chrono::duration<double>(now - start - paused_for).count();
    }
    double frame_time(uint32_t n) const { return (double)(n - 1) * den / num; }

    void queue_audio(size_t t, AVFrame* a) {
        if (!audio || !tracks[t].swr) return;
        const int cap = swr_get_out_samples(tracks[t].swr, a->nb_samples) + 256;
        std::vector<float> out((size_t)cap * 2);
        uint8_t* o = reinterpret_cast<uint8_t*>(out.data());
        const int n = swr_convert(tracks[t].swr, &o, cap, (const uint8_t**)a->extended_data, a->nb_samples);
        if (n <= 0) return;
        std::lock_guard<std::mutex> l(audio->lock);
        audio->tracks[t].insert(audio->tracks[t].end(), out.begin(), out.begin() + (size_t)n * 2);
    }

    // Decodes up to video frame `n` (1-based) into `frame`. Audio packets met
    // on the way are decoded and queued.
    void decode_to(uint32_t n) {
        while (decoded < n && !ended) {
            if (av_read_frame(fmt, packet) < 0) {
                ended = true;
                break;
            }
            if (packet->stream_index == vstream) {
                if (avcodec_send_packet(vdec, packet) == 0)
                    while (avcodec_receive_frame(vdec, scratch) == 0) {
                        std::swap(frame, scratch);
                        av_frame_unref(scratch);
                        decoded++;
                    }
            } else {
                for (size_t t = 0; t < tracks.size(); t++)
                    if (packet->stream_index == tracks[t].stream && avcodec_send_packet(tracks[t].dec, packet) == 0)
                        while (avcodec_receive_frame(tracks[t].dec, scratch) == 0) {
                            queue_audio(t, scratch);
                            av_frame_unref(scratch);
                        }
            }
            av_packet_unref(packet);
        }
    }

    // Copies the current frame's planes into the registered buffers.
    void fill_planes(GuestMemory& m) {
        if (!registered || !decoded || !frame->data[0]) return;
        const int cw = (width + 1) / 2, ch = (height + 1) / 2;
        // Bink's cR is V (FFmpeg's plane 2), cB is U (plane 1).
        const int src[4] = {0, 2, 1, 3}, pw[4] = {width, cw, cw, width}, ph[4] = {height, ch, ch, height};
        for (int k = 0; k < 4; k++) {
            const uint32_t dst = planes[k][0], pitch = planes[k][1];
            if (!dst || !pitch || !frame->data[src[k]]) continue;
            const int w = std::min<int>(pw[k], (int)pitch);
            for (int y = 0; y < ph[k]; y++)
                std::memcpy(m.ptr(dst + (uint32_t)y * pitch), frame->data[src[k]] + (size_t)y * frame->linesize[src[k]],
                            (size_t)w);
        }
    }

    // Converts the current frame into 32-bit BGRA rows (alpha 255) at `dst`.
    void copy(uint8_t* dst, int pitch, int dst_h, int dx, int dy, int sx, int sy, int w, int h) {
        if (!decoded || !frame->data[0]) return;
        w = std::min(w, width - sx), h = std::min(h, height - sy);
        h = std::min(h, dst_h - dy);
        if (w <= 0 || h <= 0) return;
        sws = sws_getCachedContext(sws, width, height, (AVPixelFormat)frame->format, width, height, AV_PIX_FMT_BGRA,
                                   SWS_POINT, nullptr, nullptr, nullptr);
        if (!sws) return;
        // Converts the whole frame into a scratch image, then copies the rectangle.
        std::vector<uint8_t> rgba((size_t)width * height * 4);
        uint8_t* planes[4] = {rgba.data(), nullptr, nullptr, nullptr};
        int strides[4] = {width * 4, 0, 0, 0};
        sws_scale(sws, frame->data, frame->linesize, 0, height, planes, strides);
        for (int y = 0; y < h; y++)
            std::memcpy(dst + (size_t)(dy + y) * pitch + (size_t)dx * 4,
                        rgba.data() + ((size_t)(sy + y) * width + sx) * 4, (size_t)w * 4);
    }
};

std::mutex g_lock;
std::map<uint32_t, std::unique_ptr<Video>> g_videos;
std::vector<uint32_t> g_tracks;  // from BinkSetSoundTrack, for the next BinkOpen

Video* video(uint32_t b) {
    std::lock_guard<std::mutex> l(g_lock);
    auto it = g_videos.find(b);
    return it == g_videos.end() ? nullptr : it->second.get();
}

AVCodecContext* open_decoder(AVStream* st) {
    const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) return nullptr;
    AVCodecContext* d = avcodec_alloc_context3(codec);
    if (avcodec_parameters_to_context(d, st->codecpar) < 0 || avcodec_open2(d, codec, nullptr) < 0)
        avcodec_free_context(&d);
    return d;
}

std::unique_ptr<Video> open_video(const std::string& path) {
    auto v = std::make_unique<Video>();
    if (avformat_open_input(&v->fmt, path.c_str(), nullptr, nullptr) < 0) return nullptr;
    if (avformat_find_stream_info(v->fmt, nullptr) < 0) return nullptr;
    for (unsigned k = 0; k < v->fmt->nb_streams; k++) {
        AVStream* st = v->fmt->streams[k];
        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && v->vstream < 0) v->vstream = (int)k;
    }
    if (v->vstream < 0) return nullptr;
    // The audio tracks: those the game selected that the file has, else the
    // first track (Bink track ids are the streams' ids).
    std::vector<int> audio_streams;
    for (uint32_t want : g_tracks)
        for (unsigned k = 0; k < v->fmt->nb_streams; k++)
            if (v->fmt->streams[k]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO &&
                (uint32_t)v->fmt->streams[k]->id == want &&
                std::find(audio_streams.begin(), audio_streams.end(), (int)k) == audio_streams.end())
                audio_streams.push_back((int)k);
    for (unsigned k = 0; k < v->fmt->nb_streams && audio_streams.empty(); k++)
        if (v->fmt->streams[k]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) audio_streams.push_back((int)k);

    AVStream* vs = v->fmt->streams[v->vstream];
    if (!(v->vdec = open_decoder(vs))) return nullptr;
    v->width = vs->codecpar->width, v->height = vs->codecpar->height;
    v->num = vs->r_frame_rate.num > 0 ? vs->r_frame_rate.num : 30;
    v->den = vs->r_frame_rate.den > 0 ? vs->r_frame_rate.den : 1;
    v->frames = vs->duration > 0 ? (uint32_t)vs->duration : (uint32_t)avformat_index_get_entries_count(vs);
    if (!v->frames) v->frames = 1;
    v->frame = av_frame_alloc(), v->scratch = av_frame_alloc(), v->packet = av_packet_alloc();

    int rate = 0;  // the output's: the first track's
    for (int k : audio_streams) {
        Track t;
        t.stream = k, t.id = (uint32_t)v->fmt->streams[k]->id;
        if (!(t.dec = open_decoder(v->fmt->streams[k]))) continue;
        if (!rate) rate = t.dec->sample_rate;
        AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
        if (swr_alloc_set_opts2(&t.swr, &stereo, AV_SAMPLE_FMT_FLT, rate, &t.dec->ch_layout, t.dec->sample_fmt,
                                t.dec->sample_rate, 0, nullptr) != 0 ||
            swr_init(t.swr) != 0) {
            swr_free(&t.swr);
            avcodec_free_context(&t.dec);
            continue;
        }
        v->tracks.push_back(t);
    }
    if (!v->tracks.empty()) {
        v->audio = std::make_unique<AudioOut>();
        v->audio->tracks.resize(v->tracks.size());
        v->audio->volumes.assign(v->tracks.size(), 1.0f);
        if (!v->audio->start(rate)) v->audio.reset();
    }
    return v;
}

void publish(Context& c, Video& v) {
    c.mem->write<uint32_t>(v.guest + kFrameNum, v.decoded ? v.decoded : 1);
}

#endif  // SCIMITAR_HAVE_FFMPEG

}  // namespace

void imp_BinkSetMemory(Context& c) { shim_return(c, 0); }

// void BinkSetSoundTrack(U32 total_tracks, U32* track_ids): for the next BinkOpen
void imp_BinkSetSoundTrack(Context& c) {
#ifdef SCIMITAR_HAVE_FFMPEG
    std::lock_guard<std::mutex> l(g_lock);
    g_tracks.clear();
    for (uint32_t k = 0; k < arg32(c, 0) && arg32(c, 1); k++) g_tracks.push_back(c.mem->read<uint32_t>(arg32(c, 1) + 4 * k));
#endif
    shim_return(c, 0);
}

// HBINK BinkOpen(const char* name, U32 flags)
void imp_BinkOpen(Context& c) {
    std::string path = (const char*)c.mem->ptr(arg32(c, 0));
    for (char& ch : path)
        if (ch == '\\') ch = '/';
    // As the game spells it on a Mac's case-insensitive disk (Brotherhood's
    // "UBI_LOGO.bik"): on iOS resolved ignoring case. A failed open leaves the
    // game's music muted for the video (SoundManager::OnPlayVideo) until a
    // level loads.
    path = host_path(path.c_str());
#ifdef SCIMITAR_HAVE_FFMPEG
    std::unique_ptr<Video> v = open_video(path);
    if (g_verbose) {
        if (!v) std::fprintf(stderr, "BinkOpen(%s): failed\n", path.c_str());
        else
            std::fprintf(stderr, "BinkOpen(%s): %dx%d, %u frames at %u/%u, audio %s (%zu tracks, first %d, %d Hz)\n",
                         path.c_str(), v->width, v->height, v->frames, v->num, v->den, v->audio ? "on" : "off",
                         v->tracks.size(), v->tracks.empty() ? -1 : (int)v->tracks[0].id,
                         v->tracks.empty() ? 0 : v->tracks[0].dec->sample_rate);
    }
    if (!v) return shim_return(c, 0);
    const uint32_t b = heap::alloc(*c.mem, kBinkSize);
    std::memset(c.mem->ptr(b), 0, kBinkSize);
    v->guest = b;
    c.mem->write<uint32_t>(b + kWidth, (uint32_t)v->width);
    c.mem->write<uint32_t>(b + kHeight, (uint32_t)v->height);
    c.mem->write<uint32_t>(b + kFrames, v->frames);
    c.mem->write<uint32_t>(b + kFrameNum, 1);
    c.mem->write<uint32_t>(b + kFrameRate, v->num);
    c.mem->write<uint32_t>(b + kFrameRateDiv, v->den);
    {
        std::lock_guard<std::mutex> l(g_lock);
        g_videos[b] = std::move(v);
    }
    shim_return(c, b);
#else
    if (g_verbose) std::fprintf(stderr, "BinkOpen(%s): built without FFmpeg, no videos\n", path.c_str());
    shim_return(c, 0);
#endif
}

void imp_BinkClose(Context& c) {
#ifdef SCIMITAR_HAVE_FFMPEG
    std::unique_ptr<Video> v;
    {
        std::lock_guard<std::mutex> l(g_lock);
        auto it = g_videos.find(arg32(c, 0));
        if (it != g_videos.end()) v = std::move(it->second), g_videos.erase(it);
    }
    if (v) heap::release(*c.mem, v->guest);
#endif
    shim_return(c, 0);
}

#ifdef SCIMITAR_HAVE_FFMPEG
// S32 BinkDoFrame(HBINK): decodes the frame FrameNum names.
void imp_BinkDoFrame(Context& c) {
    if (Video* v = video(arg32(c, 0))) {
        const uint32_t want = c.mem->read<uint32_t>(v->guest + kFrameNum);
        v->decode_to(want);
        v->fill_planes(*c.mem);
        if (!v->started) v->started = true, v->start = Clock::now();
        c.mem->write<uint32_t>(v->guest + kLastFrameNum, v->decoded);
    }
    shim_return(c, 0);
}
// void BinkNextFrame(HBINK): on to the next frame (decoded by the next BinkDoFrame).
void imp_BinkNextFrame(Context& c) {
    if (Video* v = video(arg32(c, 0))) {
        const uint32_t n = c.mem->read<uint32_t>(v->guest + kFrameNum);
        if (n < v->frames) c.mem->write<uint32_t>(v->guest + kFrameNum, n + 1);
    }
    shim_return(c, 0);
}
// S32 BinkWait(HBINK): nonzero while it is too early for the frame FrameNum names.
void imp_BinkWait(Context& c) {
    Video* v = video(arg32(c, 0));
    if (!v || !v->started) return shim_return(c, 0);
    if (v->paused) return shim_return(c, 1);
    shim_return(c, v->clock() < v->frame_time(c.mem->read<uint32_t>(v->guest + kFrameNum)) ? 1 : 0);
}
// S32 BinkShouldSkip(HBINK): nonzero when more than a frame behind.
void imp_BinkShouldSkip(Context& c) {
    Video* v = video(arg32(c, 0));
    if (!v || !v->started || v->paused) return shim_return(c, 0);
    const uint32_t n = c.mem->read<uint32_t>(v->guest + kFrameNum);
    shim_return(c, v->clock() > v->frame_time(n + 1) && n < v->frames ? 1 : 0);
}
// S32 BinkCopyToBuffer(HBINK, void* dest, S32 pitch, U32 height, U32 x, U32 y, U32 flags)
void imp_BinkCopyToBuffer(Context& c) {
    if (Video* v = video(arg32(c, 0)))
        v->copy((uint8_t*)c.mem->ptr(arg32(c, 1)), (int)arg32(c, 2), (int)arg32(c, 3), (int)arg32(c, 4),
                (int)arg32(c, 5), 0, 0, v->width, v->height);
    shim_return(c, 0);
}
// S32 BinkCopyToBufferRect(HBINK, void* dest, S32 pitch, U32 height, U32 x, U32 y,
//                          U32 srcx, U32 srcy, U32 srcw, U32 srch, U32 flags)
void imp_BinkCopyToBufferRect(Context& c) {
    if (Video* v = video(arg32(c, 0)))
        v->copy((uint8_t*)c.mem->ptr(arg32(c, 1)), (int)arg32(c, 2), (int)arg32(c, 3), (int)arg32(c, 4),
                (int)arg32(c, 5), (int)arg32(c, 6), (int)arg32(c, 7), (int)arg32(c, 8), (int)arg32(c, 9));
    shim_return(c, 0);
}
// void BinkGoto(HBINK, U32 frame, S32 flags): reopened and decoded forward when
// going back (rare: the game uses it to restore a paused video).
void imp_BinkGoto(Context& c) {
    const uint32_t b = arg32(c, 0), target = std::max<uint32_t>(arg32(c, 1), 1);
    Video* v = video(b);
    if (!v) return shim_return(c, 0);
    if (target < v->decoded) {
        av_seek_frame(v->fmt, v->vstream, 0, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(v->vdec);
        for (Track& t : v->tracks) avcodec_flush_buffers(t.dec);
        v->decoded = 0, v->ended = false;
    }
    if (v->audio) {
        std::lock_guard<std::mutex> l(v->audio->lock);
        for (auto& q : v->audio->tracks) q.clear();
    }
    v->decode_to(target - 1);
    c.mem->write<uint32_t>(b + kFrameNum, target);
    v->started = true;
    v->start = Clock::now() - std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(v->frame_time(target)));
    v->paused_for = {};
    shim_return(c, 0);
}
// S32 BinkPause(HBINK, S32 pause)
void imp_BinkPause(Context& c) {
    Video* v = video(arg32(c, 0));
    if (!v) return shim_return(c, 0);
    const bool pause = arg32(c, 1) != 0;
    if (pause && !v->paused) v->paused_at = Clock::now();
    if (!pause && v->paused) v->paused_for += Clock::now() - v->paused_at;
    v->paused = pause;
    if (v->audio) {
        std::lock_guard<std::mutex> l(v->audio->lock);
        v->audio->paused = pause;
    }
    shim_return(c, pause ? 1 : 0);
}
// void BinkSetVolume(HBINK, U32 track, S32 volume): 32768 is full volume. A
// track id the video does not play sets every track's.
void imp_BinkSetVolume(Context& c) {
    if (Video* v = video(arg32(c, 0)))
        if (v->audio) {
            const float vol = (float)(int32_t)arg32(c, 2) / 32768.0f;
            std::lock_guard<std::mutex> l(v->audio->lock);
            bool found = false;
            for (size_t t = 0; t < v->tracks.size(); t++)
                if (v->tracks[t].id == arg32(c, 1)) v->audio->volumes[t] = vol, found = true;
            if (!found) std::fill(v->audio->volumes.begin(), v->audio->volumes.end(), vol);
        }
    shim_return(c, 0);
}
// void BinkGetFrameBuffersInfo(HBINK, BINKFRAMEBUFFERS*): one frame buffer set of
// the video's size ({TotalFrames, YA width, YA height, cRcB width, cRcB height,
// FrameNum, ...}); the game makes one texture per set.
void imp_BinkGetFrameBuffersInfo(Context& c) {
    if (Video* v = video(arg32(c, 0))) {
        const uint32_t p = arg32(c, 1);
        c.mem->write<uint32_t>(p + 0x00, 1);
        c.mem->write<uint32_t>(p + 0x04, (uint32_t)v->width);
        c.mem->write<uint32_t>(p + 0x08, (uint32_t)v->height);
        c.mem->write<uint32_t>(p + 0x0c, (uint32_t)v->width / 2);
        c.mem->write<uint32_t>(p + 0x10, (uint32_t)v->height / 2);
        c.mem->write<uint32_t>(p + 0x14, 0);
    }
    shim_return(c, 0);
}
// void BinkRegisterFrameBuffers(HBINK, BINKFRAMEBUFFERS*): the game's own
// planes to decode into. BINKFRAMEBUFFERS is the header above, then the buffer
// sets at +0x18, each four planes {Allocate, Buffer, BufferPitch} (Y, cR, cB,
// A); only the first set is used (BinkGetFrameBuffersInfo asks for one).
void imp_BinkRegisterFrameBuffers(Context& c) {
    if (Video* v = video(arg32(c, 0))) {
        const uint32_t p = arg32(c, 1);
        for (int k = 0; k < 4; k++) {
            v->planes[k][0] = c.mem->read<uint32_t>(p + 0x18 + 12 * k + 4);
            v->planes[k][1] = c.mem->read<uint32_t>(p + 0x18 + 12 * k + 8);
        }
        v->registered = true;
        c.mem->write<uint32_t>(p + 0x14, 0);  // FrameNum: the current frame is in set 0
        v->fill_planes(*c.mem);
    }
    shim_return(c, 0);
}
#else
void imp_BinkSetVolume(Context& c) { shim_return(c, 0); }
#endif

}  // namespace scimitar::imports

namespace scimitar {
namespace {
using namespace imports;
#define S(N) {"_" #N, imp_##N},
const ImportShim kBink[] = {
    S(BinkSetMemory) S(BinkOpen) S(BinkClose) S(BinkSetVolume) S(BinkSetSoundTrack)
#ifdef SCIMITAR_HAVE_FFMPEG
    S(BinkDoFrame) S(BinkNextFrame) S(BinkWait) S(BinkShouldSkip) S(BinkCopyToBuffer) S(BinkCopyToBufferRect)
    S(BinkGoto) S(BinkPause) S(BinkGetFrameBuffersInfo) S(BinkRegisterFrameBuffers)
#endif
};
#undef S
SCIMITAR_REGISTER_SHIMS(kBink);
}  // namespace
}  // namespace scimitar
