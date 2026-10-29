#include "dsound_hrtf.h"

#include <math.h>
#include <stdlib.h>

enum { FADE_STEPS = 32 };

/* The APU resamples in hardware with 8-point Lagrange interpolation (retail
   measurements, xemu PR 3105), between the two middle points of the window.
   Whole positions return a source sample exactly. */
static float resample(const RecompHrtfVoice *voice)
{
    /* Lagrange weight j is prod_{m != j} (t - m) / (j - m). The denominators
       are fixed, so each weight is one product of the shared (t - m) terms
       (prefix and suffix products) and one division; a whole t still gives
       exactly one weight of 1 and the rest 0. */
    enum { N = RECOMP_HRTF_LAGRANGE_TAPS };
    _Static_assert(RECOMP_HRTF_LAGRANGE_TAPS == 8, "denominators are for 8 points");
    static const double denominators[N] = {-5040.0, 720.0, -240.0, 144.0, -144.0, 240.0, -720.0, 5040.0};
    const double t = (N / 2 - 1) + voice->phase;
    double before[N], after[N];
    before[0] = 1.0;
    for (int m = 1; m < N; ++m) before[m] = before[m - 1] * (t - (m - 1));
    after[N - 1] = 1.0;
    for (int m = N - 2; m >= 0; --m) after[m] = after[m + 1] * (t - (m + 1));
    double sum = 0.0;
    for (int j = 0; j < N; ++j)
        sum += before[j] * after[j] / denominators[j] *
            voice->source[(voice->source_head + 1u + (uint32_t)j) % N];
    return (float)sum;
}

static void push_source(RecompHrtfVoice *voice, int16_t sample)
{
    voice->source_head = (voice->source_head + 1u) % RECOMP_HRTF_LAGRANGE_TAPS;
    voice->source[voice->source_head] = sample;
}

void recomp_hrtf_angles(float x, float y, float z, float *azimuth, float *elevation)
{
    /* 5849 0x36662F: piecewise ratio approximations, not atan2/asin. */
    float h = sqrtf(x * x + z * z), ax = fabsf(x), az = fabsf(z), ay = fabsf(y);
    float a = 0.0f, e;

    if (h != 0.0f) {
        a = az > ax ? 45.0f * ax / az : 90.0f - 45.0f * az / ax;
        if (z < 0.0f) a = 180.0f - a;
        if (x < 0.0f) a = -a;
    }
    if (h == 0.0f && ay == 0.0f) e = 0.0f;
    else e = ay < h ? 45.0f * ay / h : 90.0f - 45.0f * h / ay;
    *azimuth = a;
    *elevation = y < 0.0f ? -e : e;
}

/* 5849 0x36B5F2: CVTTSS2SI truncation toward zero. */
static int quantize(float t)
{
    return (int)t;
}

RecompHrtfFilter recomp_hrtf_lookup(const uint8_t *bank, const uint8_t *map,
    float azimuth, float elevation, bool fold_rear)
{
    /* 5849 0x366BFE; 0x369CAE sets fold_rear for surround or AC3 speakers. */
    int e = 6 * (quantize(elevation + (elevation >= 0.0f ? 3.0f : -3.0f)) / 6);
    int a = 0;
    if (e < -90 || e > 90) e = 0;
    if (abs(e) != 90) {
        int step = abs(e) > 60 ? 12 : abs(e) > 30 ? 6 : 3;
        a = step * (quantize(fabsf(azimuth) + step / 2.0f) / step);
        if (fold_rear && a > 90) a = 180 - a;
        if (a < 0 || a > 180) a = 0;
    }
    const uint8_t *entry = map + 2 * ((a / 3) * 31 + (e + 90) / 6);
    const int8_t *ear0 = (const int8_t *)bank + 32 * (entry[0] | entry[1] << 8);
    /* 0x369CAE: the canonical record's delay byte, negated with the ears. */
    if (azimuth >= 0.0f) return (RecompHrtfFilter){ear0, ear0 + 32, ear0[31]};
    return (RecompHrtfFilter){ear0 + 32, ear0, -ear0[31]};
}

/* 5849 hrtf_upload_voice_entry (0x369CAE) writes the table bytes unchanged to
   the APU HRIR registers, so the encoding is the APU's. Each tap is a sign bit,
   3-bit exponent and 4-bit mantissa (1/2048 to 31/32): of the readings tried
   on DOA2's own bank, only this one makes the near ear louder than the far
   ear at every elevation-0 azimuth, with the difference growing toward high
   frequencies. Taps apply unnormalized; the absolute scale is unconfirmed. */
static float tap_gain(int8_t tap)
{
    /* (e ? (16 + m) << (e - 1) : m), negated when the sign bit is set, for
       every byte: decoding per tap per sample was most of the 3D voice cost. */
    static const float numerators[256] = {
    0.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f, 9.0f, 10.0f, 11.0f, 12.0f, 13.0f, 14.0f, 15.0f,
    16.0f, 17.0f, 18.0f, 19.0f, 20.0f, 21.0f, 22.0f, 23.0f, 24.0f, 25.0f, 26.0f, 27.0f, 28.0f, 29.0f, 30.0f, 31.0f,
    32.0f, 34.0f, 36.0f, 38.0f, 40.0f, 42.0f, 44.0f, 46.0f, 48.0f, 50.0f, 52.0f, 54.0f, 56.0f, 58.0f, 60.0f, 62.0f,
    64.0f, 68.0f, 72.0f, 76.0f, 80.0f, 84.0f, 88.0f, 92.0f, 96.0f, 100.0f, 104.0f, 108.0f, 112.0f, 116.0f, 120.0f, 124.0f,
    128.0f, 136.0f, 144.0f, 152.0f, 160.0f, 168.0f, 176.0f, 184.0f, 192.0f, 200.0f, 208.0f, 216.0f, 224.0f, 232.0f, 240.0f, 248.0f,
    256.0f, 272.0f, 288.0f, 304.0f, 320.0f, 336.0f, 352.0f, 368.0f, 384.0f, 400.0f, 416.0f, 432.0f, 448.0f, 464.0f, 480.0f, 496.0f,
    512.0f, 544.0f, 576.0f, 608.0f, 640.0f, 672.0f, 704.0f, 736.0f, 768.0f, 800.0f, 832.0f, 864.0f, 896.0f, 928.0f, 960.0f, 992.0f,
    1024.0f, 1088.0f, 1152.0f, 1216.0f, 1280.0f, 1344.0f, 1408.0f, 1472.0f, 1536.0f, 1600.0f, 1664.0f, 1728.0f, 1792.0f, 1856.0f, 1920.0f, 1984.0f,
    -0.0f, -1.0f, -2.0f, -3.0f, -4.0f, -5.0f, -6.0f, -7.0f, -8.0f, -9.0f, -10.0f, -11.0f, -12.0f, -13.0f, -14.0f, -15.0f,
    -16.0f, -17.0f, -18.0f, -19.0f, -20.0f, -21.0f, -22.0f, -23.0f, -24.0f, -25.0f, -26.0f, -27.0f, -28.0f, -29.0f, -30.0f, -31.0f,
    -32.0f, -34.0f, -36.0f, -38.0f, -40.0f, -42.0f, -44.0f, -46.0f, -48.0f, -50.0f, -52.0f, -54.0f, -56.0f, -58.0f, -60.0f, -62.0f,
    -64.0f, -68.0f, -72.0f, -76.0f, -80.0f, -84.0f, -88.0f, -92.0f, -96.0f, -100.0f, -104.0f, -108.0f, -112.0f, -116.0f, -120.0f, -124.0f,
    -128.0f, -136.0f, -144.0f, -152.0f, -160.0f, -168.0f, -176.0f, -184.0f, -192.0f, -200.0f, -208.0f, -216.0f, -224.0f, -232.0f, -240.0f, -248.0f,
    -256.0f, -272.0f, -288.0f, -304.0f, -320.0f, -336.0f, -352.0f, -368.0f, -384.0f, -400.0f, -416.0f, -432.0f, -448.0f, -464.0f, -480.0f, -496.0f,
    -512.0f, -544.0f, -576.0f, -608.0f, -640.0f, -672.0f, -704.0f, -736.0f, -768.0f, -800.0f, -832.0f, -864.0f, -896.0f, -928.0f, -960.0f, -992.0f,
    -1024.0f, -1088.0f, -1152.0f, -1216.0f, -1280.0f, -1344.0f, -1408.0f, -1472.0f, -1536.0f, -1600.0f, -1664.0f, -1728.0f, -1792.0f, -1856.0f, -1920.0f, -1984.0f};
    return numerators[(uint8_t)tap] / 2048.0f;
}

static float tap(const RecompHrtfVoice *voice, const int8_t *ear, int delay)
{
    float sum = 0.0f;
    if (ear == NULL) return 0.0f;
    const uint32_t start = voice->head - (uint32_t)delay;
    for (int k = 0; k < RECOMP_HRTF_TAPS; ++k)
        sum += tap_gain(ear[k]) * voice->history[(start - (uint32_t)k) % RECOMP_HRTF_HISTORY];
    return sum;
}

/* The effect writes these 24-bit coefficients on its first frame. DSOUND gives
   both ears of a pair one route gain, so filtering each source equals
   filtering the summed bins. */
void recomp_xtalk_process(RecompXtalk *xtalk, float *left, float *right)
{
    const float pole = (float)0x7645a1 / 0x800000, feed = (float)(0xf88313 - 0x1000000) / 0x800000;
    float *past_left = &xtalk->past[0][xtalk->head];
    float *past_right = &xtalk->past[1][xtalk->head];
    xtalk->state[1] = pole * xtalk->state[1] + feed * *past_left;
    xtalk->state[0] = pole * xtalk->state[0] + feed * *past_right;
    *left += xtalk->state[0];
    *right += xtalk->state[1];
    *past_left = *left;
    *past_right = *right;
    xtalk->head = (xtalk->head + 1u) % RECOMP_XTALK_DELAY;
}

void recomp_xtalk_process_block(RecompXtalk *xtalk, float *pcm, uint32_t frames)
{
    for (uint32_t i = 0; i < frames; ++i)
        recomp_xtalk_process(xtalk, &pcm[2u * i], &pcm[2u * i + 1u]);
}

uint32_t recomp_hrtf_render(RecompHrtfVoice *voice, const int16_t *input,
    uint32_t input_frames, uint32_t source_rate, float *out, uint32_t out_frames)
{
    const double step = (double)source_rate / RECOMP_HRTF_RATE;
    uint32_t written = 0u, consumed = 0u;

    while (consumed < input_frames && written < out_frames) {
        if (voice->phase >= 1.0) {
            push_source(voice, input[consumed++]);
            voice->phase -= 1.0;
            continue;
        }
        voice->head = (voice->head + 1u) % RECOMP_HRTF_HISTORY;
        voice->history[voice->head] = resample(voice);
        voice->phase += step;

        const RecompHrtfFilter from = voice->rendered, to = voice->filter;
        float left = tap(voice, to.left, to.delay > 0 ? to.delay : 0);
        float right = tap(voice, to.right, to.delay < 0 ? -to.delay : 0);
        /* The APU steps a new filter's taps in over 32 samples (one frame). */
        if (from.left != to.left || from.right != to.right || from.delay != to.delay) {
            const float mix = (float)++voice->fade / FADE_STEPS;
            left = left * mix + tap(voice, from.left, from.delay > 0 ? from.delay : 0) * (1.0f - mix);
            right = right * mix + tap(voice, from.right, from.delay < 0 ? -from.delay : 0) * (1.0f - mix);
            if (voice->fade == FADE_STEPS) voice->rendered = to;
        }
        if (voice->rendered.left == to.left && voice->rendered.right == to.right &&
            voice->rendered.delay == to.delay) voice->fade = 0u;
        recomp_xtalk_process(&voice->xtalk, &left, &right);
        out[3u * written] = left / 32768.0f;
        out[3u * written + 1u] = right / 32768.0f;
        out[3u * written + 2u] = voice->history[voice->head] / 32768.0f;
        ++written;
    }
    while (consumed < input_frames) push_source(voice, input[consumed++]), voice->phase -= 1.0;
    return written;
}
