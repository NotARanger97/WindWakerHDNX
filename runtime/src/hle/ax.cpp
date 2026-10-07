// snd_core (AX): voices, the 3 ms audio frame, mixing and output.
//
// Every frame (96 samples at 32 kHz) each playing voice is decoded (ADPCM/PCM16/PCM8),
// resampled, run through its volume envelope and filters and mixed into the TV buses
// (main + 3 aux). The game's aux effects (reverb...) and final-mix callbacks run on the
// result, which is upsampled to 48 kHz and sent to the host as stereo.
// Behaviour follows Cemu's snd_core (ax_mix.cpp, ax_ist.cpp, ax_aux.cpp).
#include "../platform/host.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <thread>
#include <vector>

#include "../audio_out.h"
#include "../runtime.h"
#include "ax_simd.h"

namespace interp { const char* phase_name(); }

namespace {

constexpr int kMaxVoices = 96;
constexpr int kSamples = 96;       // per frame at 32 kHz
constexpr int kSamples48 = 144;    // per frame at 48 kHz
constexpr int kTvChannels = 6, kDrcChannels = 4, kBuses = 4, kAuxBuses = 3;
constexpr auto kFramePeriod = std::chrono::microseconds(3000);

// AXVPB (guest) offsets
constexpr uint32_t kVpbIndex = 0x00, kVpbState = 0x04, kVpbPriority = 0x1C, kVpbCallback = 0x20, kVpbUser = 0x24,
                   kVpbOffsets = 0x34, kVpbCallbackEx = 0x48;
constexpr uint32_t kVpbSize = 0x58;

enum Format : uint16_t { ADPCM = 0, PCM16 = 0x0A, PCM8 = 0x19 };
enum FilterMode : uint16_t { FILTER_TAP = 0, FILTER_LINEAR = 1, FILTER_NONE = 2 };

struct ChMix {
    uint16_t vol = 0;
    int16_t delta = 0;
};

struct Voice {
    bool acquired = false;
    uint32_t vpb = 0;
    uint32_t state = 0;  // 1 = playing
    uint16_t type = 0;   // 0 normal, 1 stream (keeps ADPCM history across loops)
    // offsets, absolute in format units (bytes / halfwords / nibbles) like the DSP sees them
    uint16_t format = 0, loop = 0;
    uint32_t loop_abs = 0, end_abs = 0, cur_abs = 0, samples = 0;
    uint32_t loop_count = 0;
    // ADPCM decoder
    int16_t coef[16] = {};
    uint16_t scale = 0;
    int16_t yn1 = 0, yn2 = 0;
    uint16_t loop_scale = 0;
    int16_t loop_yn1 = 0, loop_yn2 = 0;
    // sample rate conversion (16.16 ratio)
    uint16_t filter = FILTER_TAP;
    uint32_t ratio = 0x10000, frac = 0;
    int32_t prev = 0, cur = 0;
    // volume envelope
    uint16_t ve_vol = 0x8000;
    int16_t ve_delta = 0;
    // filters
    uint16_t lpf_on = 0;
    int16_t lpf_yn1 = 0, lpf_a0 = 0, lpf_b0 = 0;
    uint16_t bq_on = 0;
    int16_t bq_b0 = 0, bq_b1 = 0, bq_b2 = 0, bq_a1 = 0, bq_a2 = 0;
    float bq_xn1 = 0, bq_xn2 = 0, bq_yn1 = 0, bq_yn2 = 0;
    // TV device mix: [channel][bus]
    ChMix tv[kTvChannels][kBuses];
};

std::mutex g_ax_mutex;
Voice g_voices[kMaxVoices];
uint32_t g_vpb_base = 0;
uint32_t g_app_frame_cb[64] = {};
uint32_t g_final_mix_cb[3] = {};
uint32_t g_aux_cb[kAuxBuses] = {}, g_aux_user[kAuxBuses] = {};
uint16_t g_aux_return[kAuxBuses] = {0x8000, 0x8000, 0x8000};
uint32_t g_upsample_stage[3] = {};  // 0 = before final mix (final mix sees 48 kHz)
std::atomic<bool> g_running{false};

// ------------------------------------------------------------------ offsets
uint32_t base_units(uint16_t fmt, uint32_t samples_ptr) {
    switch (fmt) {
    case ADPCM: return samples_ptr * 2;
    case PCM16: return samples_ptr / 2;
    default: return samples_ptr;
    }
}

// mirror the playback position into the guest VPB (offsets relative to the sample buffer)
void write_offsets(const Voice& v) {
    uint32_t o = v.vpb + kVpbOffsets, b = base_units(v.format, v.samples);
    st16(o + 0, v.format);
    st16(o + 2, v.loop);
    st32(o + 4, v.loop_abs - b);
    st32(o + 8, v.end_abs - b);
    st32(o + 0xC, v.cur_abs - b);
    st32(o + 0x10, v.samples);
}

// ------------------------------------------------------------------ decoding
void voice_end(Voice& v) {
    if (v.loop) {
        v.cur_abs = v.loop_abs;
        v.loop_count++;
        if (v.format == ADPCM) {
            v.scale = v.loop_scale;
            if (v.type == 0) { v.yn1 = v.loop_yn1; v.yn2 = v.loop_yn2; }
        }
    } else {
        v.state = 0;
    }
}

// next source sample (s16 range); 0 once the voice has stopped
int32_t next_sample(Voice& v) {
    if (!v.state) return 0;
    int32_t s;
    switch (v.format) {
    case ADPCM: {
        uint32_t off = v.cur_abs;
        if ((off & 15) < 2) {  // frame header: predictor/scale byte
            off &= ~15u;
            v.scale = *mem::ptr(off >> 1);
            off += 2;
        }
        uint8_t byte = *mem::ptr(off >> 1);
        int32_t nib = (off & 1) ? (int8_t)(byte << 4) >> 4 : (int8_t)byte >> 4;
        int ci = (v.scale >> 4) & 7;
        int32_t val = ((nib << 11) * (1 << (v.scale & 15)) + v.coef[ci * 2] * v.yn1 + v.coef[ci * 2 + 1] * v.yn2 + 0x400) >> 11;
        s = std::clamp(val, -32768, 32767);
        v.yn2 = v.yn1;
        v.yn1 = (int16_t)s;
        uint32_t end = v.end_abs;
        if ((end & 15) < 2) end += 2 - (end & 15);  // unreachable header position (see Cemu)
        if (off == end) voice_end(v);
        else v.cur_abs = off + 1;
        return s;
    }
    case PCM16: s = (int16_t)ld16(v.cur_abs * 2); break;
    case PCM8: s = (int8_t)*mem::ptr(v.cur_abs) << 8; break;
    default: v.state = 0; return 0;
    }
    if (v.cur_abs == v.end_abs) voice_end(v);
    else v.cur_abs++;
    return s;
}

// decode one frame into out (samples scaled by 256, as the DSP mixes)
void decode(Voice& v, float* out) {
    if (v.filter == FILTER_NONE) {
        for (int i = 0; i < kSamples; i++) out[i] = (float)(next_sample(v) * 256);
        return;
    }
    uint32_t ratio = std::min<uint32_t>(v.ratio, 0x80000);
    for (int i = 0; i < kSamples; i++) {
        v.frac += ratio;
        while (v.frac >= 0x10000) {
            v.prev = v.cur;
            v.cur = next_sample(v);
            v.frac -= 0x10000;
        }
        float f = v.frac / 65536.0f;
        out[i] = ((float)v.prev + (float)(v.cur - v.prev) * f) * 256.0f;
    }
}

void apply_envelope(Voice& v, float* s) {
    if (v.ve_vol == 0x8000 && v.ve_delta == 0) return;
    float vol = v.ve_vol / 32768.0f, d = v.ve_delta / 32768.0f;
#if defined(__SWITCH__) && defined(__aarch64__)
    axsimd::envelope(s, kSamples, vol, d);
#else
    for (int i = 0; i < kSamples; i++) {
        vol += d;
        s[i] *= vol;
    }
#endif
    if (v.ve_delta) v.ve_vol = (uint16_t)std::clamp<int32_t>((int32_t)v.ve_vol + v.ve_delta * kSamples, 0, 0xFFFF);
}

void apply_biquad(Voice& v, float* s) {
    if (!v.bq_on) return;
    float b0 = v.bq_b0 / 16384.0f, b1 = v.bq_b1 / 16384.0f, b2 = v.bq_b2 / 16384.0f;
    float a1 = v.bq_a1 / 16384.0f, a2 = v.bq_a2 / 16384.0f;
    for (int i = 0; i < kSamples; i++) {
        float x = s[i] / 256.0f;
        float y = b0 * x + b1 * v.bq_xn1 + b2 * v.bq_xn2 + a1 * v.bq_yn1 + a2 * v.bq_yn2;
        s[i] = y * 256.0f;
        y = std::clamp(y, -32768.0f, 32767.0f);
        v.bq_yn2 = v.bq_yn1;
        v.bq_xn2 = v.bq_xn1;
        v.bq_yn1 = y;
        v.bq_xn1 = x;
    }
}

void apply_lpf(Voice& v, float* s) {
    if (!v.lpf_on) return;
    float a0 = v.lpf_a0 / 32767.0f, b0 = v.lpf_b0 / 32767.0f;
    float prev = v.lpf_yn1 * 256.0f / 32767.0f;
    for (int i = 0; i < kSamples; i++) {
        s[i] = a0 * s[i] - b0 * prev;
        prev = s[i];
    }
    v.lpf_yn1 = (int16_t)std::clamp(prev / 256.0f * 32767.0f, -32768.0f, 32767.0f);
}

// ------------------------------------------------------------------ mixing
float g_tv_bus[kBuses][kTvChannels][kSamples];
double g_sfx_energy = 0;   // stats: output of non-stream voices
uint32_t g_sfx_started = 0;

void mix_into(const float* in, float* out, ChMix& m) {
    float vol = m.vol / 32768.0f;
#if defined(__SWITCH__) && defined(__aarch64__)
    vol = axsimd::mix(in, out, kSamples, vol, m.delta / 32768.0f);
    if (m.delta) m.vol = (uint16_t)std::clamp(vol * 32768.0f, 0.0f, 65535.0f);
#else
    if (m.delta) {
        float d = m.delta / 32768.0f;
        for (int i = 0; i < kSamples; i++) {
            vol += d;
            out[i] += in[i] * vol;
        }
        m.vol = (uint16_t)std::clamp(vol * 32768.0f, 0.0f, 65535.0f);
    } else {
        for (int i = 0; i < kSamples; i++) out[i] += in[i] * vol;
    }
#endif
}

// debug: WWHD_AX_STATS=1 logs the active voices about once a second
static bool ax_stats() { static const bool on = getenv("WWHD_AX_STATS") != nullptr; return on; }
void log_stats() {
    static int frame = 0;
    if (!ax_stats() || ++frame % 333) return;
    int n = 0, fmt[3] = {}, filt[3] = {}, loops = 0, streams = 0;
    for (Voice& v : g_voices) {
        if (!v.acquired || v.state != 1) continue;
        n++;
        fmt[v.format == ADPCM ? 0 : v.format == PCM16 ? 1 : 2]++;
        filt[v.filter]++;
        loops += v.loop != 0;
        streams += v.type == 1;
    }
    uint64_t ur, dr;
    audio::stats(ur, dr);
    LOG("[ax] device queue %d frames, underrun %llu, dropped %llu", audio::buffered_frames(), (unsigned long long)ur,
        (unsigned long long)dr);
    LOG("[ax] sfx started %u, sfx mean level %.1f", g_sfx_started, g_sfx_energy / (333.0 * kSamples));
    g_sfx_energy = 0;
    LOG("[ax] %d playing: adpcm %d pcm16 %d pcm8 %d | tap %d linear %d none %d | looping %d stream %d | aux cb %d%d%d final mix %d%d",
        n, fmt[0], fmt[1], fmt[2], filt[0], filt[1], filt[2], loops, streams, !!g_aux_cb[0], !!g_aux_cb[1], !!g_aux_cb[2],
        !!g_final_mix_cb[0], !!g_final_mix_cb[1]);
}

void process_voices() {
    log_stats();
    const bool stats = ax_stats();
    memset(g_tv_bus, 0, sizeof g_tv_bus);
    float buf[kSamples];
    for (Voice& v : g_voices) {
        if (!v.acquired || v.state != 1) continue;
        decode(v, buf);
        apply_envelope(v, buf);
        apply_biquad(v, buf);
        apply_lpf(v, buf);
        if (stats && v.type == 0) {
            float tv = 0;
            for (int ch = 0; ch < 2; ch++) tv += v.tv[ch][0].vol / 32768.0f;
            for (int i = 0; i < kSamples; i++) g_sfx_energy += std::fabs(buf[i] / 256.0f) * tv;
        }
        for (int c = 0; c < kTvChannels; c++)
            for (int b = 0; b < kBuses; b++)
                if (v.tv[c][b].vol || v.tv[c][b].delta) mix_into(buf, g_tv_bus[b][c], v.tv[c][b]);
        if (!v.state) st32(v.vpb + kVpbState, 0);
        st32(v.vpb + kVpbOffsets + 0xC, v.cur_abs - base_units(v.format, v.samples));
    }
}

// ------------------------------------------------------------------ aux, final mix, output (guest buffers)
struct GuestBuffers {
    uint32_t aux[2][kAuxBuses];  // per aux frame and bus: 6 channels x 96 int32
    uint32_t aux_ptrs = 0, aux_info = 0;
    uint32_t tv48 = 0, tv_ptrs = 0, tv_param = 0;     // 6 channels x 144 int32
    uint32_t drc48 = 0, drc_ptrs = 0, drc_param = 0;  // 8 channels x 144 int32
} G;
int g_aux_frame = 0;
float g_up_hist[kTvChannels] = {};

void init_buffers() {
    for (auto& f : G.aux)
        for (auto& b : f) {
            b = mem::runtime_alloc(4 * kTvChannels * kSamples48, 64);
            memset(mem::ptr(b), 0, 4 * kTvChannels * kSamples48);
        }
    G.aux_ptrs = mem::runtime_alloc(4 * kTvChannels, 32);
    G.aux_info = mem::runtime_alloc(8, 32);
    G.tv48 = mem::runtime_alloc(4 * kTvChannels * kSamples48, 64);
    G.tv_ptrs = mem::runtime_alloc(4 * kTvChannels, 32);
    G.tv_param = mem::runtime_alloc(0x10, 32);
    G.drc48 = mem::runtime_alloc(4 * kDrcChannels * 2 * kSamples48, 64);
    G.drc_ptrs = mem::runtime_alloc(4 * kDrcChannels * 2, 32);
    G.drc_param = mem::runtime_alloc(0x10, 32);
}

// aux effects run one frame behind: process the buffers stored last frame
void run_aux_callbacks(Cpu* c) {
    int out = 1 - g_aux_frame;
    for (int b = 0; b < kAuxBuses; b++) {
        uint32_t buf = G.aux[out][b];
        if (!g_aux_cb[b]) {
            memset(mem::ptr(buf), 0, 4 * kTvChannels * kSamples);
            continue;
        }
        for (int ch = 0; ch < kTvChannels; ch++) st32(G.aux_ptrs + 4 * ch, buf + 4 * ch * kSamples);
        st32(G.aux_info, kTvChannels);
        st32(G.aux_info + 4, kSamples);
        guest_call(c, g_aux_cb[b], {G.aux_ptrs, g_aux_user[b], G.aux_info});
    }
}

void store_aux_input() {
    for (int b = 0; b < kAuxBuses; b++) {
        if (!g_aux_cb[b]) continue;
        uint32_t buf = G.aux[g_aux_frame][b];
        for (int ch = 0; ch < kTvChannels; ch++)
            for (int i = 0; i < kSamples; i++) st32(buf + 4 * (ch * kSamples + i), (uint32_t)((int32_t)g_tv_bus[b + 1][ch][i] >> 8));
    }
}

// main bus + returned aux, in s16<<8 units
void merge_tv(int32_t out[kTvChannels][kSamples]) {
    int in = 1 - g_aux_frame;
    for (int ch = 0; ch < kTvChannels; ch++)
        for (int i = 0; i < kSamples; i++) {
            float s = g_tv_bus[0][ch][i];
            for (int b = 0; b < kAuxBuses; b++)
                if (g_aux_cb[b]) s += (float)((int32_t)ld32(G.aux[in][b] + 4 * (ch * kSamples + i)) << 8) * (g_aux_return[b] / 32768.0f);
            out[ch][i] = (int32_t)std::clamp(s, -2147483648.0f, 2147483520.0f);
        }
}

// 2 -> 3 linear upsampler (Cemu's AXUpsampleLinear32To48)
void upsample(const int32_t* in, int32_t* out, float& hist, int shift) {
#if defined(__SWITCH__) && defined(__aarch64__)
    axsimd::upsample(in, out, kSamples, hist, shift);
#else
    float prev = hist;
    for (int i = 0; i < kSamples; i += 2) {
        float s0 = (float)in[i], s1 = (float)in[i + 1];
        *out++ = (int32_t)(prev * 0.66666669f + s0 * 0.33333331f) >> shift;
        *out++ = (int32_t)s0 >> shift;
        *out++ = (int32_t)(s1 * 0.66666669f + s0 * 0.33333331f) >> shift;
        prev = s1;
    }
    hist = prev;
#endif
}

void call_final_mix(Cpu* c, int dev, uint32_t param, uint32_t ptrs, uint32_t data, int channels, int devices, int samples) {
    for (int i = 0; i < channels * devices; i++) st32(ptrs + 4 * i, data + 4 * i * samples);
    st32(param + 0, ptrs);
    st16(param + 4, channels);
    st16(param + 6, samples);
    st16(param + 8, devices);
    st16(param + 0xA, channels);
    if (g_final_mix_cb[dev]) guest_call(c, g_final_mix_cb[dev], {param});
}

void output_frame(Cpu* c) {
    static int32_t tv[kTvChannels][kSamples];
    merge_tv(tv);
    bool before = g_upsample_stage[0] == 0;
    int n = before ? kSamples48 : kSamples;
    // final mix input: s16-range int32 per channel
    for (int ch = 0; ch < kTvChannels; ch++) {
        if (before) {
            int32_t tmp[kSamples48];
            upsample(tv[ch], tmp, g_up_hist[ch], 8);
            for (int i = 0; i < n; i++) st32(G.tv48 + 4 * (ch * n + i), (uint32_t)tmp[i]);
        } else {
            for (int i = 0; i < n; i++) st32(G.tv48 + 4 * (ch * n + i), (uint32_t)(tv[ch][i] >> 8));
        }
    }
    call_final_mix(c, 0, G.tv_param, G.tv_ptrs, G.tv48, kTvChannels, 1, n);
    // DRC: not output; the game still gets its callback with silence
    memset(mem::ptr(G.drc48), 0, 4 * kDrcChannels * 2 * n);
    call_final_mix(c, 1, G.drc_param, G.drc_ptrs, G.drc48, kDrcChannels, 2, n);

    // stereo out at 48 kHz (TV mode is stereo: front left/right)
    int16_t pcm[kSamples48 * 2];
    int32_t lr[2][kSamples48];
    for (int ch = 0; ch < 2; ch++) {
        if (before) {
            for (int i = 0; i < kSamples48; i++) lr[ch][i] = (int32_t)ld32(G.tv48 + 4 * (ch * n + i));
        } else {
            static float hist2[2];
            int32_t in[kSamples];
            for (int i = 0; i < kSamples; i++) in[i] = (int32_t)ld32(G.tv48 + 4 * (ch * n + i));
            upsample(in, lr[ch], hist2[ch], 0);
        }
    }
    for (int i = 0; i < kSamples48; i++) {
        pcm[i * 2] = (int16_t)std::clamp(lr[0][i], -32768, 32767);
        pcm[i * 2 + 1] = (int16_t)std::clamp(lr[1][i], -32768, 32767);
    }
    audio::push(pcm, kSamples48);
}

void frame_thread() {
    Cpu* c = threads::make_service_cpu("AX frame", 0x20000);
    host::set_thread_name("AX frame");
    threads::set_service_core(0);  // the game's audio threads live on core 0
    auto next = std::chrono::steady_clock::now();
    while (g_running) {
        // pace frames by the device: run slightly faster/slower to keep ~40 ms queued
        double level = (double)(audio::buffered_frames() - audio::target_frames()) / audio::target_frames();
        double stretch = 1.0 + std::clamp(level * 0.05, -0.05, 0.05);
        next += std::chrono::microseconds((int64_t)(3000 * stretch));
        threads::service_begin();  // a save state waits until the frame is done (voices, callbacks)
        {
            std::lock_guard<std::mutex> lk(g_ax_mutex);
            process_voices();
        }
        bool took = threads::ensure_core();  // callbacks run guest code
        run_aux_callbacks(c);
        uint32_t cbs[64];
        {
            std::lock_guard<std::mutex> lk(g_ax_mutex);
            memcpy(cbs, g_app_frame_cb, sizeof cbs);
        }
        for (uint32_t cb : cbs)
            if (cb) guest_call(c, cb);
        store_aux_input();
        output_frame(c);
        if (took) threads::release_core();
        g_aux_frame = 1 - g_aux_frame;
        threads::service_end();
        std::this_thread::sleep_until(next);
        auto now = std::chrono::steady_clock::now();
        if (now - next > std::chrono::milliseconds(30)) next = now;  // don't try to catch up after a stall
    }
}

Voice* voice(uint32_t vpb) {
    if (!g_vpb_base || vpb < g_vpb_base) return nullptr;
    uint32_t i = (vpb - g_vpb_base) / kVpbSize;
    return i < kMaxVoices ? &g_voices[i] : nullptr;
}

}  // namespace

// ---- save states: voices and the registered callbacks (the AX frame thread is idle meanwhile)
#include "../savestate.h"
void ax_ss_save(ss::Writer& w) {
    std::lock_guard<std::mutex> lk(g_ax_mutex);
    w.u8(g_running);
    w.u32(g_vpb_base);
    w.u32(kMaxVoices);
    for (auto& v : g_voices) w.pod(v);
    w.pod(g_app_frame_cb);
    w.pod(g_final_mix_cb);
    w.pod(g_aux_cb);
    w.pod(g_aux_user);
    w.pod(g_aux_return);
    w.pod(g_upsample_stage);
    w.pod(G);
    w.u32((uint32_t)g_aux_frame);
}
bool ax_ss_check(ss::Reader r, std::string& why) {
    bool running = r.u8();
    uint32_t base = r.u32();
    if (running && !g_running) { why = "audio is not initialized yet"; return false; }
    if (running && base != g_vpb_base) { why = "audio voices live elsewhere in this session"; return false; }
    return r.ok;
}
void ax_ss_load(ss::Reader& r) {
    std::lock_guard<std::mutex> lk(g_ax_mutex);
    r.u8();
    r.u32();
    uint32_t n = r.u32();
    for (uint32_t i = 0; i < n && i < (uint32_t)kMaxVoices; i++) g_voices[i] = r.pod<Voice>();
    g_app_frame_cb[0] = 0;
    r.bytes(g_app_frame_cb, sizeof g_app_frame_cb);
    r.bytes(g_final_mix_cb, sizeof g_final_mix_cb);
    r.bytes(g_aux_cb, sizeof g_aux_cb);
    r.bytes(g_aux_user, sizeof g_aux_user);
    r.bytes(g_aux_return, sizeof g_aux_return);
    r.bytes(g_upsample_stage, sizeof g_upsample_stage);
    GuestBuffers g = r.pod<GuestBuffers>();
    if (G.tv48 && g.tv48 != G.tv48) LOG("[savestate] AX buffers moved (%08X -> %08X)", g.tv48, G.tv48);
    g_aux_frame = (int)r.u32() & 1;
    memset(g_up_hist, 0, sizeof g_up_hist);
    audio::flush();
}

HLE(snd_core, AXInit) {
    std::lock_guard<std::mutex> lk(g_ax_mutex);
    if (g_running) return;
    g_vpb_base = mem::runtime_alloc(kVpbSize * kMaxVoices, 32);
    for (int i = 0; i < kMaxVoices; i++) {
        g_voices[i] = Voice{};
        g_voices[i].vpb = g_vpb_base + i * kVpbSize;
        st32(g_voices[i].vpb + kVpbIndex, i);
    }
    init_buffers();
    audio::init();
    g_running = true;
    std::thread(frame_thread).detach();
    LOG("[ax] initialized, %d voices", kMaxVoices);
}
HLE(snd_core, AXQuit) { g_running = false; }
HLE(snd_core, AXUserBegin) {}
HLE(snd_core, AXUserEnd) {}
HLE(snd_core, AXUserIsProtected) { ret(c, 0); }
HLE(snd_core, AXInitProfile) {}
HLE(snd_core, AXGetSwapProfile) { ret(c, 0); }

HLE(snd_core, AXRegisterAppFrameCallback) {
    std::lock_guard<std::mutex> lk(g_ax_mutex);
    for (uint32_t& cb : g_app_frame_cb)
        if (!cb) { cb = arg(c, 0); break; }
    ret(c, 0);
}
HLE(snd_core, AXDeregisterAppFrameCallback) {
    std::lock_guard<std::mutex> lk(g_ax_mutex);
    for (uint32_t& cb : g_app_frame_cb)
        if (cb == arg(c, 0)) cb = 0;
    ret(c, 0);
}
HLE(snd_core, AXRegisterDeviceFinalMixCallback) {
    if (arg(c, 0) < 3) g_final_mix_cb[arg(c, 0)] = arg(c, 1);
    ret(c, 0);
}
HLE(snd_core, AXGetDeviceFinalMixCallback) {
    if (arg(c, 1)) st32(arg(c, 1), arg(c, 0) < 3 ? g_final_mix_cb[arg(c, 0)] : 0);
    ret(c, 0);
}
HLE(snd_core, AXRegisterAuxCallback) {
    // (device, deviceIndex, auxBus, func, userParam); only the TV aux buses are mixed
    uint32_t dev = arg(c, 0), bus = arg(c, 2);
    LOG("[ax] aux callback: device %u index %u bus %u func %08X", dev, arg(c, 1), bus, arg(c, 3));
    if (bus >= kAuxBuses) { ret(c, (uint32_t)-5); return; }
    if (dev == 0) {
        g_aux_cb[bus] = arg(c, 3);
        g_aux_user[bus] = arg(c, 4);
    }
    ret(c, 0);
}
HLE(snd_core, AXSetAuxReturnVolume) {
    if (arg(c, 0) == 0 && arg(c, 2) < kAuxBuses) g_aux_return[arg(c, 2)] = (uint16_t)arg(c, 3);
    ret(c, 0);
}
HLE(snd_core, AXGetDeviceMode) { if (arg(c, 1)) st32(arg(c, 1), 0); ret(c, 0); }  // stereo
HLE(snd_core, AXSetDeviceUpsampleStage) {
    if (arg(c, 0) < 3) g_upsample_stage[arg(c, 0)] = arg(c, 1);
    ret(c, 0);
}
HLE(snd_core, AXSetDeviceLinearUpsampler) { ret(c, 0); }
HLE(snd_core, AXSetDeviceCompressor) { ret(c, 0); }
HLE(snd_core, AXSetDefaultMixerSelect) {}
HLE(snd_core, AXSetDRCVSMode) { ret(c, 0); }
HLE(snd_core, AXSetDRCVSLC) { ret(c, 0); }
HLE(snd_core, AXSetDRCVSSpeakerPosition) { ret(c, 0); }
HLE(snd_core, AXSetDRCVSSurroundDepth) { ret(c, 0); }
HLE(snd_core, AXSetDRCVSDownmixBalance) { ret(c, 0); }
HLE(snd_core, AXSetDRCVSSurroundLevelGain) { ret(c, 0); }
HLE(snd_core, AXSetDRCVSOutputGain) { ret(c, 0); }
HLE(snd_core, DRCVS_SetOutputMode) { ret(c, 0); }
HLE(snd_core, DRCVS_Process) { ret(c, 0); }
HLE(snd_core, AXRmtGetSamplesLeft) { ret(c, 0); }
HLE(snd_core, AXRmtGetSamples) { ret(c, 0); }
HLE(snd_core, AXRmtAdvancePtr) { ret(c, 0); }

// sound trace window, started by the capture key (input thread) and read by AX calls
static std::mutex g_sound_trace_mutex;
static FILE* g_sound_trace = nullptr;
static uint64_t g_sound_trace_end = 0;
static FILE* sound_trace_file() {  // g_ax_mutex held
    std::lock_guard<std::mutex> lk(g_sound_trace_mutex);
    if (g_sound_trace && timebase::now() > g_sound_trace_end) {
        fclose(g_sound_trace);
        g_sound_trace = nullptr;
        LOG("[ax] sound trace written");
    }
    return g_sound_trace;
}
namespace ax {
void start_sound_trace(const char* path, double seconds) {
    std::lock_guard<std::mutex> lk(g_sound_trace_mutex);
    if (g_sound_trace) fclose(g_sound_trace);
    g_sound_trace = fopen(path, "w");
    g_sound_trace_end = timebase::now() + (uint64_t)(seconds * timebase::kTicksPerSec);
    if (g_sound_trace) {
        fprintf(g_sound_trace, "# voice starts for %.0f s (time, frame phase, voice, sample data, guest call chain)\n", seconds);
        fflush(g_sound_trace);
    }
    LOG("[ax] recording sound activity to %s", path);
}
}  // namespace ax

// one line per voice start while the sound trace runs: time, frame phase, voice, sample data, call chain
static void trace_voice_start(Cpu* c, uint32_t voice_addr, uint32_t samples) {
    FILE* f = sound_trace_file();
    if (!f) return;
    char buf[400];
    int n = snprintf(buf, sizeof buf, "%10.3f ms  %-10s voice %08X samples %08X  lr=%08X",
                     timebase::now() * 1000.0 / timebase::kTicksPerSec, interp::phase_name(), voice_addr, samples, c->lr);
    for (uint32_t sp = c->r[1], i = 0; i < 9 && sp; i++) {
        uint32_t prev = ld32(sp);
        if (!prev || prev <= sp) break;
        n += snprintf(buf + n, sizeof buf - n, " <- %08X", ld32(prev + 4));
        sp = prev;
    }
    fprintf(f, "%s\n", buf);
    fflush(f);
}

// debug: WWHD_AX_STATS=1 logs calls per second of the voice API every 5 s
enum { kAxStatCount = 15 };
static const char* kAxStatNames[kAxStatCount] = {"AXAcquireVoiceEx", "AXFreeVoice", "AXSetVoiceState", "AXSetVoiceOffsets", "AXSetVoiceLoop", "AXSetVoiceEndOffsetEx", "AXSetVoiceLoopOffsetEx", "AXSetVoiceSrcRatio", "AXSetVoiceSrc", "AXSetVoiceVe", "AXSetVoiceDeviceMix", "AXSetVoiceAdpcm", "AXSetVoiceAdpcmLoop", "AXSetVoiceType", "AXSetVoicePriority"};
static std::atomic<uint32_t> g_ax_stats[kAxStatCount];
static void ax_stat(int i) {
    static const bool on = getenv("WWHD_AX_STATS") != nullptr;
    if (!on) return;
    g_ax_stats[i]++;
    static std::atomic<uint64_t> t0{timebase::now()};
    uint64_t t = timebase::now(), s0 = t0.load();
    if (t - s0 > 5 * timebase::kTicksPerSec && t0.compare_exchange_strong(s0, t)) {
        char buf[600];
        int n = snprintf(buf, sizeof buf, "[ax] calls/s (%s):", interp::phase_name());
        for (int k = 0; k < kAxStatCount; k++) {
            uint32_t v = g_ax_stats[k].exchange(0);
            if (v) n += snprintf(buf + n, sizeof buf - n, " %s=%.1f", kAxStatNames[k] + 2, v * (double)timebase::kTicksPerSec / (double)(t - s0));
        }
        LOG("%s", buf);
    }
}

// test aid: WWHD_SOUND_TRACE=file records voice starts for the first 60 s after the first voice
static void sound_trace_env() {
    static bool done = false;
    if (done) return;
    done = true;
    if (const char* e = getenv("WWHD_SOUND_TRACE")) ax::start_sound_trace(e, 60.0);
}

HLE(snd_core, AXAcquireVoiceEx) {
    ax_stat(0);
    sound_trace_env();
    {
        // debug: WWHD_VOICE_RATE=1 logs voice acquisitions per second every 5 s
        static const bool rate = getenv("WWHD_VOICE_RATE") != nullptr;
        if (rate) {
            static uint64_t n = 0, t0 = timebase::now();
            n++;
            uint64_t t = timebase::now();
            if (t - t0 > 5 * timebase::kTicksPerSec) {
                LOG("[ax] %.1f voices/s (%s)", n * (double)timebase::kTicksPerSec / (double)(t - t0), interp::phase_name());
                n = 0;
                t0 = t;
            }
        }
        // debug: WWHD_TRACE_VOICE=n logs voice acquisitions with the frame phase (frame interpolation)
        static int trace = getenv("WWHD_TRACE_VOICE") ? atoi(getenv("WWHD_TRACE_VOICE")) : 0;
        if (trace > 0) {
            trace--;
            char buf[256];
            int n = snprintf(buf, sizeof buf, "[ax] acquire voice (%s) lr=%08X", interp::phase_name(), c->lr);
            for (uint32_t sp = c->r[1], i = 0; i < 7 && sp; i++) {
                uint32_t prev = ld32(sp);
                if (!prev || prev <= sp) break;
                n += snprintf(buf + n, sizeof buf - n, " <- %08X", ld32(prev + 4));
                sp = prev;
            }
            LOG("%s", buf);
        }
    }
    // (priority, callbackEx, userParam)
    std::lock_guard<std::mutex> lk(g_ax_mutex);
    for (Voice& v : g_voices) {
        if (v.acquired) continue;
        uint32_t vpb = v.vpb;
        v = Voice{};
        v.vpb = vpb;
        v.acquired = true;
        st32(vpb + kVpbState, 0);
        st32(vpb + kVpbPriority, arg(c, 0));
        st32(vpb + kVpbCallback, 0);
        st32(vpb + kVpbCallbackEx, arg(c, 1));
        st32(vpb + kVpbUser, arg(c, 2));
        ret(c, vpb);
        return;
    }
    LOG("[ax] out of voices");
    ret(c, 0);
}
HLE(snd_core, AXFreeVoice) {
    ax_stat(1);
    std::lock_guard<std::mutex> lk(g_ax_mutex);
    if (Voice* v = voice(arg(c, 0))) {
        v->acquired = false;
        v->state = 0;
        st32(v->vpb + kVpbState, 0);
    }
}
HLE(snd_core, AXSetVoiceState) {
    ax_stat(2);
    std::lock_guard<std::mutex> lk(g_ax_mutex);
    if (Voice* v = voice(arg(c, 0))) {
        if (v->state != 1 && (arg(c, 1) & 0xFFFF) == 1) {
            if (v->type == 0) g_sfx_started++;
            trace_voice_start(c, arg(c, 0), v->samples);
        }
        v->state = arg(c, 1) & 0xFFFF;
        st32(v->vpb + kVpbState, v->state);
    }
}
HLE(snd_core, AXSetVoiceType) {
    ax_stat(13);
    std::lock_guard<std::mutex> lk(g_ax_mutex);
    if (Voice* v = voice(arg(c, 0))) v->type = (uint16_t)arg(c, 1);
}
HLE(snd_core, AXSetVoiceOffsets) {
    ax_stat(3);
    // AXPBOFFSET: +0 format, +2 loop, +4 loopOffset, +8 endOffset, +C currentOffset, +10 samples (relative to samples)
    std::lock_guard<std::mutex> lk(g_ax_mutex);
    Voice* v = voice(arg(c, 0));
    uint32_t o = arg(c, 1);
    if (!v) return;
    v->format = ld16(o);
    v->loop = ld16(o + 2);
    v->samples = ld32(o + 0x10);
    trace_voice_start(c, arg(c, 0), v->samples);  // new sample data on a voice = a (re)started sound
    uint32_t b = base_units(v->format, v->samples);
    v->loop_abs = b + ld32(o + 4);
    v->end_abs = b + ld32(o + 8);
    v->cur_abs = b + ld32(o + 0xC);
    write_offsets(*v);
}
HLE(snd_core, AXGetVoiceOffsets) {
    std::lock_guard<std::mutex> lk(g_ax_mutex);
    Voice* v = voice(arg(c, 0));
    uint32_t o = arg(c, 1);
    if (!v) return;
    uint32_t b = base_units(v->format, v->samples);
    st16(o, v->format);
    st16(o + 2, v->loop);
    st32(o + 4, v->loop_abs - b);
    st32(o + 8, v->end_abs - b);
    st32(o + 0xC, v->cur_abs - b);
    st32(o + 0x10, v->samples);
}
HLE(snd_core, AXSetVoiceLoop) {
    ax_stat(4);
    std::lock_guard<std::mutex> lk(g_ax_mutex);
    if (Voice* v = voice(arg(c, 0))) { v->loop = (uint16_t)arg(c, 1); write_offsets(*v); }
}
// "Ex" offsets are relative to a new sample base
HLE(snd_core, AXSetVoiceEndOffsetEx) {
    ax_stat(5);
    std::lock_guard<std::mutex> lk(g_ax_mutex);
    if (Voice* v = voice(arg(c, 0))) {
        v->samples = arg(c, 2);
        v->end_abs = base_units(v->format, v->samples) + arg(c, 1);
        write_offsets(*v);
    }
}
HLE(snd_core, AXSetVoiceLoopOffsetEx) {
    ax_stat(6);
    std::lock_guard<std::mutex> lk(g_ax_mutex);
    if (Voice* v = voice(arg(c, 0))) {
        v->samples = arg(c, 2);
        v->loop_abs = base_units(v->format, v->samples) + arg(c, 1);
        write_offsets(*v);
    }
}
HLE(snd_core, AXGetVoiceCurrentOffsetEx) {
    std::lock_guard<std::mutex> lk(g_ax_mutex);
    Voice* v = voice(arg(c, 0));
    ret(c, v ? v->cur_abs - base_units(v->format, arg(c, 1)) : 0);
}
HLE(snd_core, AXGetVoiceLoopCount) {
    std::lock_guard<std::mutex> lk(g_ax_mutex);
    Voice* v = voice(arg(c, 0));
    ret(c, v ? v->loop_count : 0);
}
HLE(snd_core, AXSetVoiceSrcRatio) {
    ax_stat(7);
    std::lock_guard<std::mutex> lk(g_ax_mutex);
    if (Voice* v = voice(arg(c, 0))) {
        double r = c->f[1].ps0 * 65536.0;
        v->ratio = (uint32_t)std::clamp(r, 0.0, (double)0x80000);
    }
    ret(c, 0);
}
HLE(snd_core, AXSetVoiceSrc) {
    ax_stat(8);
    // AXPBSRC: +0 ratioHi, +2 ratioLo, +4 currentFrac, +6 history[4]
    std::lock_guard<std::mutex> lk(g_ax_mutex);
    uint32_t s = arg(c, 1);
    if (Voice* v = voice(arg(c, 0))) {
        v->ratio = (uint32_t)ld16(s) << 16 | ld16(s + 2);
        v->frac = ld16(s + 4);
        v->prev = (int16_t)ld16(s + 6);
        v->cur = (int16_t)ld16(s + 8);
    }
}
HLE(snd_core, AXSetVoiceSrcType) {
    std::lock_guard<std::mutex> lk(g_ax_mutex);
    if (Voice* v = voice(arg(c, 0))) {
        uint32_t t = arg(c, 1);
        v->filter = t == 0 ? FILTER_NONE : t == 1 ? FILTER_LINEAR : FILTER_TAP;
    }
}
HLE(snd_core, AXSetVoiceVe) {
    ax_stat(9);
    // AXPBVE: +0 currentVolume u16, +2 currentDelta s16
    std::lock_guard<std::mutex> lk(g_ax_mutex);
    if (Voice* v = voice(arg(c, 0))) {
        v->ve_vol = ld16(arg(c, 1));
        v->ve_delta = (int16_t)ld16(arg(c, 1) + 2);
    }
}
HLE(snd_core, AXSetVoiceDeviceMix) {
    ax_stat(10);
    // (vpb, device, deviceIndex, AXCHMIX* mix): per channel, 4 buses of {vol u16, delta s16}
    std::lock_guard<std::mutex> lk(g_ax_mutex);
    Voice* v = voice(arg(c, 0));
    uint32_t dev = arg(c, 1), idx = arg(c, 2), mix = arg(c, 3);
    if (!v || !mix) { ret(c, (uint32_t)-3); return; }
    if (dev == 0 && idx == 0)
        for (int ch = 0; ch < kTvChannels; ch++)
            for (int b = 0; b < kBuses; b++) {
                uint32_t e = mix + 4 * (ch * kBuses + b);
                v->tv[ch][b].vol = ld16(e);
                v->tv[ch][b].delta = (int16_t)ld16(e + 2);
            }
    ret(c, 0);
}
HLE(snd_core, AXSetVoiceAdpcm) {
    ax_stat(11);
    // AXPBADPCM: a[16], gain, pred_scale, yn1, yn2
    std::lock_guard<std::mutex> lk(g_ax_mutex);
    uint32_t a = arg(c, 1);
    if (Voice* v = voice(arg(c, 0))) {
        for (int i = 0; i < 16; i++) v->coef[i] = (int16_t)ld16(a + 2 * i);
        v->scale = ld16(a + 0x22);
        v->yn1 = (int16_t)ld16(a + 0x24);
        v->yn2 = (int16_t)ld16(a + 0x26);
    }
}
HLE(snd_core, AXSetVoiceAdpcmLoop) {
    ax_stat(12);
    // AXPBADPCMLOOP: loop_pred_scale, loop_yn1, loop_yn2
    std::lock_guard<std::mutex> lk(g_ax_mutex);
    uint32_t a = arg(c, 1);
    if (Voice* v = voice(arg(c, 0))) {
        v->loop_scale = ld16(a);
        v->loop_yn1 = (int16_t)ld16(a + 2);
        v->loop_yn2 = (int16_t)ld16(a + 4);
    }
}
HLE(snd_core, AXSetVoiceLpf) {
    // AXPBLPF: on, yn1, a0, b0
    std::lock_guard<std::mutex> lk(g_ax_mutex);
    uint32_t p = arg(c, 1);
    if (Voice* v = voice(arg(c, 0))) {
        v->lpf_on = ld16(p);
        v->lpf_yn1 = (int16_t)ld16(p + 2);
        v->lpf_a0 = (int16_t)ld16(p + 4);
        v->lpf_b0 = (int16_t)ld16(p + 6);
    }
}
HLE(snd_core, AXSetVoiceLpfCoefs) {
    std::lock_guard<std::mutex> lk(g_ax_mutex);
    if (Voice* v = voice(arg(c, 0))) {
        v->lpf_a0 = (int16_t)arg(c, 1);
        v->lpf_b0 = (int16_t)arg(c, 2);
    }
}
HLE(snd_core, AXSetVoiceBiquad) {
    // AXPBBIQUAD: on, xn1, xn2, yn1, yn2, b0, b1, b2, a1, a2
    std::lock_guard<std::mutex> lk(g_ax_mutex);
    uint32_t p = arg(c, 1);
    if (Voice* v = voice(arg(c, 0))) {
        v->bq_on = ld16(p);
        v->bq_xn1 = (int16_t)ld16(p + 2);
        v->bq_xn2 = (int16_t)ld16(p + 4);
        v->bq_yn1 = (int16_t)ld16(p + 6);
        v->bq_yn2 = (int16_t)ld16(p + 8);
        v->bq_b0 = (int16_t)ld16(p + 0xA);
        v->bq_b1 = (int16_t)ld16(p + 0xC);
        v->bq_b2 = (int16_t)ld16(p + 0xE);
        v->bq_a1 = (int16_t)ld16(p + 0x10);
        v->bq_a2 = (int16_t)ld16(p + 0x12);
    }
}
HLE(snd_core, AXSetVoiceBiquadCoefs) {
    std::lock_guard<std::mutex> lk(g_ax_mutex);
    if (Voice* v = voice(arg(c, 0))) {
        v->bq_b0 = (int16_t)arg(c, 1);
        v->bq_b1 = (int16_t)arg(c, 2);
        v->bq_b2 = (int16_t)arg(c, 3);
        v->bq_a1 = (int16_t)arg(c, 4);
        v->bq_a2 = (int16_t)arg(c, 5);
    }
}
HLE(snd_core, AXComputeLpfCoefs) {
    // (freq, u16* a0, u16* b0)
    float t1 = cosf((float)arg(c, 0) / 32000.0f * 6.2831855f);
    float t2 = 2.0f - t1;
    t1 = -((sqrtf(t2 * t2 - 1.0f) - t2) * 32768.0f);
    uint16_t r = (uint16_t)t1;
    st16(arg(c, 1), 0x7FFF - r);
    st16(arg(c, 2), r);
}
// the remote speaker and voice priorities don't affect TV output
HLE(snd_core, AXSetVoicePriority) {
    ax_stat(14);}
HLE(snd_core, AXSetVoiceMixerSelect) { ret(c, 0); }
HLE(snd_core, AXSetVoiceRmtOn) {}
HLE(snd_core, AXSetVoiceRmtIIR) {}
HLE(snd_core, AXSetVoiceRmtIIRCoefs) {}
HLE(snd_core, AXDecodeAdpcmData) { LOG("[ax] AXDecodeAdpcmData called (not implemented)"); }
