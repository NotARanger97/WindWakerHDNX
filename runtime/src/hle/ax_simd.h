#pragma once
#if defined(__SWITCH__) && defined(__aarch64__)
#include <arm_neon.h>

namespace axsimd {
// Explicit multiply then add: runtime builds disable FP contraction. Gain
// ramps still advance sequentially, retaining scalar rounding and final gain.
// AX's fixed 96-sample frames are divisible by four (eight for upsample).
inline void envelope(float* samples, int count, float vol, float delta) {
    if (delta == 0) {
        const float32x4_t gain = vdupq_n_f32(vol);
        for (int i = 0; i < count; i += 4)
            vst1q_f32(samples + i, vmulq_f32(vld1q_f32(samples + i), gain));
        return;
    }
    for (int i = 0; i < count; i += 4) {
        float gains[4];
        for (float& gain : gains) { vol += delta; gain = vol; }
        vst1q_f32(samples + i, vmulq_f32(vld1q_f32(samples + i), vld1q_f32(gains)));
    }
}
inline float mix(const float* in, float* out, int count, float vol, float delta) {
    if (delta == 0) {
        const float32x4_t gain = vdupq_n_f32(vol);
        for (int i = 0; i < count; i += 4)
            vst1q_f32(out + i, vaddq_f32(vld1q_f32(out + i), vmulq_f32(vld1q_f32(in + i), gain)));
    } else {
        for (int i = 0; i < count; i += 4) {
            float gains[4];
            for (float& gain : gains) { vol += delta; gain = vol; }
            vst1q_f32(out + i, vaddq_f32(vld1q_f32(out + i), vmulq_f32(vld1q_f32(in + i), vld1q_f32(gains))));
        }
    }
    return vol;
}
// Four independent 2 -> 3 pairs. s0 also takes the float conversion round trip,
// matching the original scalar upsampler for integers outside exact FP range.
inline void upsample(const int32_t* in, int32_t* out, int count, float& hist, int shift) {
    float32x4_t previous = vdupq_n_f32(hist);
    const int32x4_t shifts = vdupq_n_s32(-shift);
    for (int i = 0; i < count; i += 8) {
        const int32x4x2_t pairs = vld2q_s32(in + i);
        const float32x4_t s0 = vcvtq_f32_s32(pairs.val[0]), s1 = vcvtq_f32_s32(pairs.val[1]);
        const float32x4_t prev = vextq_f32(previous, s1, 3);
        const float32x4_t third = vmulq_n_f32(s0, 0.33333331f);
        int32x4x3_t result;
        result.val[0] = vshlq_s32(vcvtq_s32_f32(vaddq_f32(vmulq_n_f32(prev, 0.66666669f), third)), shifts);
        result.val[1] = vshlq_s32(vcvtq_s32_f32(s0), shifts);
        result.val[2] = vshlq_s32(vcvtq_s32_f32(vaddq_f32(vmulq_n_f32(s1, 0.66666669f), third)), shifts);
        vst3q_s32(out + (i / 2) * 3, result);
        previous = s1;
    }
    hist = vgetq_lane_f32(previous, 3);
}
} // namespace axsimd
#endif
