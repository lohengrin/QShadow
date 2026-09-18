#pragma once

#include <cmath>
#include <cstdint>

// Inline port of the constant-packing stage of AMD FidelityFX Super Resolution
// 1.0 (FFX_FSR1_VERSION v1.20210629), MIT licensed (see third_party/fsr).
//
// The reference functions FsrEasuCon / FsrRcasCon live in
// third_party/fsr/ffx_fsr1.h (GLSL). They only repack floats into 32-bit
// words, so for the FP32 shader path used here this C++ port is exact. FSR 1.0
// is per-pixel math on those constants; see shaders/fsr_easu.comp and
// shaders/fsr_rcas.comp for where they are consumed.
//
// The RCAS constant (packed by rcasCon) is the only one read by the FP32 RCAS
// shader; Con1..Con3 are packed by the reference implementation for the
// half/FP16 path and are unused here, but they are written for symmetry.

namespace fsr {

// EASU constant set for a call to FsrEasuCon().
struct EasuConsts {
    std::uint32_t con0[4];
    std::uint32_t con1[4];
    std::uint32_t con2[4];
    std::uint32_t con3[4];
};

inline std::uint32_t f32Bits(float x)
{
    std::uint32_t bits;
    __builtin_memcpy(&bits, &x, sizeof(bits));
    return bits;
}

// Mirrors FsrEasuCon(inputViewport, inputSize, outputSize). The input
// "viewport" equals the input size here (full-frame upscale, no dynamic
// resolution offsets).
inline EasuConsts easuCon(float inputViewportW, float inputViewportH,
                          float inputSizeW, float inputSizeH,
                          float outputSizeW, float outputSizeH)
{
    const float rcpOutW = 1.0f / outputSizeW;
    const float rcpOutH = 1.0f / outputSizeH;
    const float rcpInW = 1.0f / inputSizeW;
    const float rcpInH = 1.0f / inputSizeH;

    EasuConsts c{};
    // Output integer position -> viewport pixel position.
    c.con0[0] = f32Bits(inputViewportW * rcpOutW);
    c.con0[1] = f32Bits(inputViewportH * rcpOutH);
    c.con0[2] = f32Bits(0.5f * inputViewportW * rcpOutW - 0.5f);
    c.con0[3] = f32Bits(0.5f * inputViewportH * rcpOutH - 0.5f);
    // Viewport pixel position -> normalized image space (upper-left of 'F' tap).
    c.con1[0] = f32Bits(rcpInW);
    c.con1[1] = f32Bits(rcpInH);
    // Centers of gather4, offsets from upper-left of 'F'.
    c.con1[2] = f32Bits(1.0f * rcpInW);
    c.con1[3] = f32Bits(-1.0f * rcpInH);
    // From gather0 instead of 'F'.
    c.con2[0] = f32Bits(-1.0f * rcpInW);
    c.con2[1] = f32Bits(2.0f * rcpInH);
    c.con2[2] = f32Bits(1.0f * rcpInW);
    c.con2[3] = f32Bits(2.0f * rcpInH);
    c.con3[0] = f32Bits(0.0f * rcpInW);
    c.con3[1] = f32Bits(4.0f * rcpInH);
    c.con3[2] = 0;
    c.con3[3] = 0;
    return c;
}

// RCAS constant for a given sharpness in "stops" (0 = full sharpening; larger
// values are halvings of the sharpening strength). Mirrors
// FsrRcasCon(): sharpness = exp2(-sharpness).
inline std::uint32_t rcasCon(float sharpness)
{
    return f32Bits(std::exp2(-sharpness));
}

} // namespace fsr