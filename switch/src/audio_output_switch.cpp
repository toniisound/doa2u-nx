// SPDX-License-Identifier: GPL-3.0-or-later
// Nintendo Switch audio backend: libnx audout at 48 kHz stereo, fed by a
// mixer thread on core 2. Mirrors audio_output_sdl.cpp: one queue per guest
// voice, a 50 ms silence cushion when a voice starts or starves, and a pitch
// trim of at most 1% that holds the cushion against the device's real rate.
//
// DOA2U routes voices the way XAudio2 does: each voice may carry a dry output
// matrix to the speakers and a wet one to an I3DL2 reverb submix. A routed
// voice is converted through its matrices as it is queued, so each queued
// frame holds a dry and a wet stereo pair. The mixer sums the dry pairs to the
// device and the wet pairs into a reverb bus, which runs the same portable
// reverb (audio_reverb.cpp, FAudio's reimplementation of XAudio2's) and the
// game's crosstalk canceller as the SDL backend, then adds it to the mix.
//
// The console outputs stereo, so speakers() is always 2, whatever
// RECOMP_AUDIO_SURROUND says; the kernel's XC_AUDIO setting follows it.
//
// Having a device open also matters for speed: dsound_api_adapter.c then
// runs its output pump (ADPCM decode, HRTF for 3D voices) on its own thread,
// off the guest core, instead of inside the game's DirectSound calls.
#include "audio_output.h"
#include "audio_reverb.h"
#include "dsound_hrtf.h"

#include <switch.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace {

constexpr uint32_t kVoiceCount = 256;
constexpr uint32_t kMaxBufferBytes = 160000;
constexpr uint32_t kMinSampleRate = 1000;
constexpr uint32_t kMaxSampleRate = 200000;
constexpr float kMaxVolumeLevel = 16777216.0f; // XAUDIO2_MAX_VOLUME_LEVEL
constexpr uint32_t kPrerollMs = 50;
constexpr float kRateTrim = 0.01f;
constexpr uint32_t kDeviceRate = 48000;
constexpr uint32_t kSpeakers = 2;
constexpr uint32_t kPeriodFrames = 1024;   // 21.3 ms per device buffer
constexpr uint32_t kDeviceBuffers = 4;
constexpr uint32_t kQueueMs = 1000;        // per-voice capacity
constexpr uint32_t kFrameFloats = 4;       // dry L, dry R, wet L, wet R

struct VoiceRouting {
    uint32_t channels;           // 0: unrouted (default dry send, no wet)
    float dry[3 * kSpeakers];    // [source + destination * channels]
    float wet[3 * 2];
};

struct Voice {
    bool active = false;
    bool routed = false;
    bool wet = false;
    uint32_t sample_rate = 0, channels = 0, bits = 0;
    float gain = 1.0f;
    float ratio = 1.0f;
    float level_ms = kPrerollMs;
    std::vector<float> ring; // kFrameFloats per frame
    uint32_t capacity = 0;   // frames
    uint64_t write = 0;      // frames written
    double read = 0.0;       // frames consumed (fractional)
    uint64_t submitted = 0;

    uint32_t queued() const
    {
        const double q = static_cast<double>(write) - read;
        return q > 0.0 ? static_cast<uint32_t>(q) : 0u;
    }
};

std::mutex mutex;
Voice voices[kVoiceCount];
VoiceRouting routing[kVoiceCount];
bool attempted = false;
bool device_open = false;
bool muted = false;
bool reverb_ready = false;
float master_gain = 1.0f;
std::atomic<std::thread::id> owner_thread{};

std::mutex reverb_mutex;
RecompAudioReverbState *reverb = nullptr;
RecompXtalk xtalk{};

volatile bool mixer_running = false;
Thread mixer_thread;
AudioOutBuffer out_buffers[kDeviceBuffers];
int16_t *out_memory = nullptr;
std::vector<float> mix_scratch(kPeriodFrames * 2);
std::vector<float> wet_scratch(kPeriodFrames * 2);
unsigned long long dropped = 0, underruns = 0, submitted_buffers = 0;
unsigned long long reverb_updates = 0, routing_updates = 0, voice_resets = 0;

void drop(uint32_t slot, const char *reason)
{
    ++dropped;
    if (dropped <= 4 || (dropped & (dropped - 1)) == 0) {
        std::fprintf(stderr, "[audio-output] dropped=%llu slot=%u reason=%s\n", dropped, slot, reason);
    }
}

bool inRange(float value, float low, float high)
{
    return std::isfinite(value) && value >= low && value <= high;
}

inline float sampleAt(const uint8_t *pcm, uint32_t index, uint32_t bits)
{
    switch (bits) {
    case 8: return (static_cast<int>(pcm[index]) - 128) / 128.0f;
    case 16: {
        int16_t s;
        std::memcpy(&s, pcm + index * 2u, 2);
        return s / 32768.0f;
    }
    default: {
        float f;
        std::memcpy(&f, pcm + index * 4u, 4);
        return f;
    }
    }
}

// Appends frames through the voice's routing; returns false when the queue
// is full. pcm == nullptr queues silence.
bool push(Voice &voice, const VoiceRouting &route, const uint8_t *pcm, uint32_t frames)
{
    if (voice.queued() + frames > voice.capacity) return false;
    const uint32_t channels = voice.channels;
    for (uint32_t i = 0; i < frames; ++i) {
        float out[kFrameFloats] = {0.0f, 0.0f, 0.0f, 0.0f};
        if (pcm != nullptr) {
            if (voice.routed) {
                for (uint32_t source = 0; source < channels; ++source) {
                    const float s = sampleAt(pcm, i * channels + source, voice.bits);
                    out[0] += s * route.dry[source + 0u * channels];
                    out[1] += s * route.dry[source + 1u * channels];
                    if (voice.wet) {
                        out[2] += s * route.wet[source + 0u * channels];
                        out[3] += s * route.wet[source + 1u * channels];
                    }
                }
            } else if (channels == 1) {
                out[0] = out[1] = sampleAt(pcm, i, voice.bits);
            } else {
                // Stereo maps straight through; a third (LFE) channel has no
                // speaker on a stereo device and is left out, as the default
                // XAudio2 matrix does for 2.1 into stereo.
                out[0] = sampleAt(pcm, i * channels, voice.bits);
                out[1] = sampleAt(pcm, i * channels + 1u, voice.bits);
            }
        }
        const uint64_t at = (voice.write + i) % voice.capacity;
        std::memcpy(&voice.ring[at * kFrameFloats], out, sizeof out);
    }
    voice.write += frames;
    return true;
}

// Mixes one device period; voices resample linearly.
void mixPeriod(int16_t *out)
{
    float *mix = mix_scratch.data();
    float *wet = wet_scratch.data();
    std::memset(mix, 0, sizeof(float) * kPeriodFrames * 2u);
    std::memset(wet, 0, sizeof(float) * kPeriodFrames * 2u);
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (Voice &voice : voices) {
            if (!voice.active || voice.capacity == 0) continue;
            const double step = static_cast<double>(voice.sample_rate) * voice.ratio / kDeviceRate;
            const float gain = voice.gain;
            const bool voice_wet = voice.wet;
            for (uint32_t f = 0; f < kPeriodFrames; ++f) {
                if (voice.read + 1.0 >= static_cast<double>(voice.write)) break; // starved
                const uint64_t base = static_cast<uint64_t>(voice.read);
                const float frac = static_cast<float>(voice.read - static_cast<double>(base));
                const float *a = &voice.ring[(base % voice.capacity) * kFrameFloats];
                const float *b = &voice.ring[((base + 1u) % voice.capacity) * kFrameFloats];
                mix[f * 2u] += (a[0] + (b[0] - a[0]) * frac) * gain;
                mix[f * 2u + 1u] += (a[1] + (b[1] - a[1]) * frac) * gain;
                if (voice_wet) {
                    wet[f * 2u] += (a[2] + (b[2] - a[2]) * frac) * gain;
                    wet[f * 2u + 1u] += (a[3] + (b[3] - a[3]) * frac) * gain;
                }
                voice.read += step;
            }
        }
    }
    {
        // Once configured the reverb runs every period, so its tail decays
        // after the sends stop, as XAudio2's submix does.
        std::lock_guard<std::mutex> lock(reverb_mutex);
        if (reverb != nullptr) {
            recomp_audio_reverb_process(reverb, wet, static_cast<int>(kPeriodFrames));
            recomp_xtalk_process_block(&xtalk, wet, kPeriodFrames);
            for (uint32_t i = 0; i < kPeriodFrames * 2u; ++i) mix[i] += wet[i];
        }
    }
    for (uint32_t i = 0; i < kPeriodFrames * 2u; ++i) {
        float s = mix[i] * master_gain;
        if (s > 1.0f) s = 1.0f;
        if (s < -1.0f) s = -1.0f;
        out[i] = static_cast<int16_t>(s * 32767.0f);
    }
}

void mixerLoop(void *)
{
    // Prime every device buffer, then refill each one as it is released.
    for (uint32_t i = 0; i < kDeviceBuffers; ++i) {
        mixPeriod(static_cast<int16_t *>(out_buffers[i].buffer));
        audoutAppendAudioOutBuffer(&out_buffers[i]);
    }
    while (mixer_running) {
        AudioOutBuffer *released = nullptr;
        u32 count = 0;
        if (R_FAILED(audoutWaitPlayFinish(&released, &count, 100'000'000ull)) || released == nullptr) {
            continue;
        }
        mixPeriod(static_cast<int16_t *>(released->buffer));
        audoutAppendAudioOutBuffer(released);
    }
}

void resetVoice(Voice &voice)
{
    voice.active = false;
    voice.write = 0;
    voice.read = 0.0;
}

} // namespace

extern "C" uint32_t recomp_audio_output_speakers(void)
{
    return kSpeakers;
}

extern "C" int recomp_audio_output_on_owner_thread(void)
{
    const std::thread::id owner = owner_thread.load(std::memory_order_relaxed);
    return owner == std::thread::id{} || owner == std::this_thread::get_id();
}

extern "C" int recomp_audio_output_enabled(void)
{
    return device_open ? 1 : 0;
}

extern "C" int recomp_audio_output_ready(void)
{
    return device_open || muted;
}

extern "C" void recomp_audio_output_initialize(void)
{
    std::lock_guard<std::mutex> lock(mutex);
    if (attempted) return;
    attempted = true;
    owner_thread = std::this_thread::get_id();

    if (const char *setting = std::getenv("RECOMP_AUDIO_GAIN")) {
        char *end = nullptr;
        const double gain = std::strtod(setting, &end);
        if (end == setting || *end != '\0' || !std::isfinite(gain) || gain < 0.0 || gain > 1.0) {
            std::fprintf(stderr, "[audio-output] muted invalid RECOMP_AUDIO_GAIN (expected 0..1)\n");
            return;
        }
        master_gain = static_cast<float>(gain);
    }
    if (master_gain == 0.0f) {
        muted = true;
        std::fprintf(stderr, "[audio-output] muted RECOMP_AUDIO_GAIN=0\n");
        return;
    }
    Result rc = audoutInitialize();
    if (R_FAILED(rc)) {
        std::fprintf(stderr, "[audio-output] audoutInitialize failed 0x%x\n", rc);
        return;
    }
    rc = audoutStartAudioOut();
    if (R_FAILED(rc)) {
        std::fprintf(stderr, "[audio-output] audoutStartAudioOut failed 0x%x\n", rc);
        audoutExit();
        return;
    }
    const size_t bytes = kPeriodFrames * 2u * sizeof(int16_t); // 4 KiB, page sized
    out_memory = static_cast<int16_t *>(aligned_alloc(0x1000, bytes * kDeviceBuffers));
    if (out_memory == nullptr) {
        audoutStopAudioOut();
        audoutExit();
        return;
    }
    std::memset(out_memory, 0, bytes * kDeviceBuffers);
    for (uint32_t i = 0; i < kDeviceBuffers; ++i) {
        out_buffers[i].next = nullptr;
        out_buffers[i].buffer = reinterpret_cast<uint8_t *>(out_memory) + i * bytes;
        out_buffers[i].buffer_size = bytes;
        out_buffers[i].data_size = bytes;
        out_buffers[i].data_offset = 0;
    }
    mixer_running = true;
    if (R_FAILED(threadCreate(&mixer_thread, mixerLoop, nullptr, nullptr, 0x10000, 0x2B, 2)) ||
        R_FAILED(threadStart(&mixer_thread))) {
        mixer_running = false;
        audoutStopAudioOut();
        audoutExit();
        std::fprintf(stderr, "[audio-output] mixer thread failed\n");
        return;
    }
    device_open = true;
    std::fprintf(stderr,
        "[audio-output] initialized backend=audout rate=%u channels=%u master_gain=%.3f\n",
        audoutGetSampleRate(), audoutGetChannelCount(), master_gain);
}

extern "C" void recomp_audio_output_shutdown(void)
{
    if (!recomp_audio_output_on_owner_thread()) {
        std::fprintf(stderr, "[audio-output] shutdown deferred to owner thread\n");
        return;
    }
    if (!device_open) return;
    mixer_running = false;
    threadWaitForExit(&mixer_thread);
    threadClose(&mixer_thread);
    audoutStopAudioOut();
    audoutExit();
    device_open = false;
    {
        std::lock_guard<std::mutex> lock(reverb_mutex);
        recomp_audio_reverb_destroy(reverb);
        reverb = nullptr;
        reverb_ready = false;
    }
    std::fprintf(stderr,
        "[audio-output] summary submitted_buffers=%llu dropped=%llu underruns=%llu "
        "voice_resets=%llu reverb_updates=%llu routing_updates=%llu\n",
        submitted_buffers, dropped, underruns, voice_resets, reverb_updates, routing_updates);
}

extern "C" void recomp_audio_output_reset_voice(uint32_t slot)
{
    if (!recomp_audio_output_on_owner_thread()) return;
    std::lock_guard<std::mutex> lock(mutex);
    if (slot < kVoiceCount) {
        if (voices[slot].active) ++voice_resets;
        resetVoice(voices[slot]);
        routing[slot] = {};
    }
}

extern "C" int recomp_audio_output_set_reverb(const RecompAudioReverb *parameters)
{
    if (!recomp_audio_output_on_owner_thread() || !parameters) return 0;
    const RecompAudioReverb &p = *parameters;
    if (p.room < -10000 || p.room > 0 || p.room_hf < -10000 || p.room_hf > 0 ||
        !inRange(p.room_rolloff, 0.0f, 10.0f) ||
        !inRange(p.decay_time, 0.1f, 20.0f) ||
        !inRange(p.decay_hf_ratio, 0.1f, 2.0f) ||
        p.reflections < -10000 || p.reflections > 1000 ||
        !inRange(p.reflections_delay, 0.0f, 0.3f) ||
        p.reverb < -10000 || p.reverb > 2000 ||
        !inRange(p.reverb_delay, 0.0f, 0.1f) ||
        !inRange(p.diffusion, 0.0f, 100.0f) ||
        !inRange(p.density, 0.0f, 100.0f) ||
        !inRange(p.hf_reference, 20.0f, 20000.0f)) return 0;
    recomp_audio_output_initialize();
    if (muted) return 1;
    if (!device_open) return 0;
    std::lock_guard<std::mutex> lock(reverb_mutex);
    if (reverb == nullptr) {
        reverb = recomp_audio_reverb_create(static_cast<int>(kDeviceRate));
        xtalk = {};
        if (reverb == nullptr) {
            std::fprintf(stderr, "[audio-output] reverb unavailable\n");
            return 0;
        }
        std::fprintf(stderr, "[audio-output] reverb ready freq=%u\n", kDeviceRate);
    }
    recomp_audio_reverb_set_i3dl2(reverb, &p);
    reverb_ready = true;
    ++reverb_updates;
    return 1;
}

extern "C" int recomp_audio_output_set_voice_routing(uint32_t slot, uint32_t channels,
    const float *dry, const float *wet)
{
    if (!recomp_audio_output_on_owner_thread() || slot >= kVoiceCount ||
        channels < 1 || channels > 3 || !dry || !wet) return 0;
    VoiceRouting route{};
    route.channels = channels;
    bool wet_send = false;
    for (uint32_t i = 0; i < channels * kSpeakers; ++i) {
        if (!inRange(dry[i], -kMaxVolumeLevel, kMaxVolumeLevel)) return 0;
        route.dry[i] = dry[i];
    }
    for (uint32_t i = 0; i < channels * 2u; ++i) {
        if (!inRange(wet[i], -kMaxVolumeLevel, kMaxVolumeLevel)) return 0;
        route.wet[i] = wet[i];
        wet_send |= wet[i] != 0.0f;
    }
    recomp_audio_output_initialize();
    if (muted) return 1;
    if (!device_open || (wet_send && !reverb_ready)) return 0;
    std::lock_guard<std::mutex> lock(mutex);
    Voice &voice = voices[slot];
    if (voice.active && voice.channels != channels) return 0;
    if (std::memcmp(&routing[slot], &route, sizeof route) == 0) return 1;
    // A voice switching between direct and routed output restarts its queue;
    // a routed voice keeps what it queued and takes the new matrices from its
    // next submission, as an XAudio2 output matrix change does.
    if (voice.active && !voice.routed) resetVoice(voice);
    routing[slot] = route;
    ++routing_updates;
    return 1;
}

extern "C" void recomp_audio_output_submit(
    uint32_t slot, const uint8_t *pcm, uint32_t bytes,
    uint32_t sample_rate, uint32_t channels, uint32_t bits_per_sample,
    int32_t volume_hundredth_db)
{
    recomp_audio_output_initialize();
    if (!device_open || bytes == 0) return;
    if (slot >= kVoiceCount || !pcm || bytes > kMaxBufferBytes ||
        sample_rate < kMinSampleRate || sample_rate > kMaxSampleRate ||
        channels < 1 || channels > 3 ||
        (bits_per_sample != 8 && bits_per_sample != 16 && bits_per_sample != 32)) {
        drop(slot, "invalid-pcm");
        return;
    }
    const uint32_t block_align = channels * (bits_per_sample / 8u);
    if (bytes % block_align != 0) {
        drop(slot, "partial-frame");
        return;
    }
    const uint32_t frames = bytes / block_align;

    std::lock_guard<std::mutex> lock(mutex);
    Voice &voice = voices[slot];
    const VoiceRouting &route = routing[slot];
    if (route.channels && route.channels != channels) {
        drop(slot, "routing-channel-mismatch");
        return;
    }
    const bool routed = route.channels != 0u;
    bool wet_send = false;
    for (uint32_t i = 0; routed && i < channels * 2u; ++i) wet_send |= route.wet[i] != 0.0f;
    wet_send = wet_send && reverb_ready;
    if (voice.active && (voice.sample_rate != sample_rate || voice.channels != channels ||
                         voice.bits != bits_per_sample || voice.routed != routed ||
                         voice.wet != wet_send)) {
        resetVoice(voice);
    }
    if (!voice.active) {
        voice.sample_rate = sample_rate;
        voice.channels = channels;
        voice.bits = bits_per_sample;
        voice.routed = routed;
        voice.wet = wet_send;
        // Room for the cushion plus the pump's bursts: kQueueMs, or twice
        // this submission if a voice delivers larger blocks.
        uint32_t capacity = sample_rate * kQueueMs / 1000u;
        if (capacity < frames * 2u + sample_rate * kPrerollMs / 1000u) {
            capacity = frames * 2u + sample_rate * kPrerollMs / 1000u;
        }
        if (voice.capacity < capacity) {
            voice.ring.assign(static_cast<size_t>(capacity) * kFrameFloats, 0.0f);
            voice.capacity = capacity;
        }
        voice.write = 0;
        voice.read = 0.0;
        voice.ratio = 1.0f;
        voice.level_ms = kPrerollMs;
        voice.submitted = 0;
        voice.active = true;
    }

    if (volume_hundredth_db < -10000) volume_hundredth_db = -10000;
    if (volume_hundredth_db > 0) volume_hundredth_db = 0;
    voice.gain = volume_hundredth_db == -10000 ? 0.0f : std::pow(10.0f, volume_hundredth_db / 2000.0f);

    const uint32_t queued = voice.queued();
    const bool starved = queued <= 1u;
    if (starved && voice.submitted > 0) ++underruns;
    float queued_ms = queued * 1000.0f / sample_rate;
    if (starved) {
        // Restart from a clean cushion: drop the fractional tail, queue silence.
        voice.read = static_cast<double>(voice.write);
        push(voice, route, nullptr, sample_rate * kPrerollMs / 1000u);
        queued_ms = static_cast<float>(kPrerollMs);
        voice.level_ms = queued_ms;
    }
    voice.level_ms += (queued_ms - voice.level_ms) / 16.0f;
    float ratio = 1.0f + (voice.level_ms - kPrerollMs) / 10000.0f;
    if (ratio < 1.0f - kRateTrim) ratio = 1.0f - kRateTrim;
    if (ratio > 1.0f + kRateTrim) ratio = 1.0f + kRateTrim;
    voice.ratio = ratio;

    if (!push(voice, route, pcm, frames)) {
        drop(slot, "queue-full");
        return;
    }
    voice.submitted += frames;
    ++submitted_buffers;
}
