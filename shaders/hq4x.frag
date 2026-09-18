#version 450

// hq4x single-pass upscaler.
// Faithful GLSL 330 -> 450 (Vulkan) port of the libretro glsl-shaders
// single-pass hq4x (hqx/single-pass/shader-files/hq4x.glsl, auto-translated
// from the Cg original by hylian). The interpolation weights are looked up in
// the 256x256 RGBA table shipped as hq4x.png.

layout(set = 0, binding = 0) uniform sampler2D uTex;
layout(set = 0, binding = 1) uniform sampler2D uLut;

layout(push_constant) uniform Params {
    vec2 uTexSize;
    vec2 uInSize;
    vec2 uOutSize;
    vec2 uAspect;
    vec3 uStretch;
    float uModScale;
};

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 FragColor;

vec3 toYuv(vec3 c)
{
    return vec3(dot(c, vec3(0.299072266, 0.586914062, 0.114013672)),
                dot(c, vec3(-0.168945312, -0.331054688, 0.5)),
                dot(c, vec3(0.5, -0.418945312, -0.080993652)));
}

bool isDiff(vec3 w, vec3 y)
{
    vec3 d = abs(toYuv(w) - y);
    return d.x > 0.188232422 || d.y > 0.0274505615 || d.z > 0.0235290527;
}

void main()
{
    vec2 ps = 1.0 / uTexSize;

    // Neighborhood texture coordinates (TEX1..TEX3 from the original vertex
    // shader, folded into the fragment).
    vec4 TEX1 = vec4(v_uv.x, v_uv.x, v_uv.x, v_uv.y) + vec4(-ps.x, 0.0, ps.x, -ps.y);
    vec4 TEX2 = vec4(v_uv.x, v_uv.x, v_uv.x, v_uv.y) + vec4(-ps.x, 0.0, ps.x, 0.0);
    vec4 TEX3 = vec4(v_uv.x, v_uv.x, v_uv.x, v_uv.y) + vec4(-ps.x, 0.0, ps.x, ps.y);

    vec3 w1 = texture(uTex, vec2(TEX1.x, TEX1.w)).rgb; // top-left
    vec3 w2 = texture(uTex, vec2(TEX1.y, TEX1.w)).rgb; // top-center
    vec3 w3 = texture(uTex, vec2(TEX1.z, TEX1.w)).rgb; // top-right
    vec3 w4 = texture(uTex, vec2(TEX2.x, TEX2.w)).rgb; // mid-left
    vec3 w5 = texture(uTex, v_uv).rgb;                 // center
    vec3 w6 = texture(uTex, vec2(TEX2.z, TEX2.w)).rgb; // mid-right
    vec3 w7 = texture(uTex, vec2(TEX3.x, TEX3.w)).rgb; // bottom-left
    vec3 w8 = texture(uTex, vec2(TEX3.y, TEX3.w)).rgb; // bottom-center
    vec3 w9 = texture(uTex, vec2(TEX3.z, TEX3.w)).rgb; // bottom-right

    // Sub-pixel position and candidate interpolation sources.
    vec2 fp = fract(v_uv * uTexSize);
    vec2 a = -0.5 + fp;
    vec2 quad = step(0.0, a) - step(0.0, -a);

    vec3 TMP1 = w5;
    vec3 TMP2 = texture(uTex, v_uv + vec2(ps.x, ps.y) * quad).rgb;
    vec3 TMP3 = texture(uTex, v_uv + vec2(ps.x, 0.0) * quad).rgb;
    vec3 TMP4 = texture(uTex, v_uv + vec2(0.0, ps.y) * quad).rgb;

    vec3 y5 = toYuv(w5);

    // 3x3 pattern index (bits 1,2,4 for row 1, 8,16 for row 2, 32,64,128 row 3).
    float pattern = 0.0;
    if (isDiff(w1, y5)) pattern += 1.0;
    if (isDiff(w2, y5)) pattern += 2.0;
    if (isDiff(w3, y5)) pattern += 4.0;
    if (isDiff(w4, y5)) pattern += 8.0;
    if (isDiff(w6, y5)) pattern += 16.0;
    if (isDiff(w7, y5)) pattern += 32.0;
    if (isDiff(w8, y5)) pattern += 64.0;
    if (isDiff(w9, y5)) pattern += 128.0;

    // Cross pattern: |w4-w2|, |w2-w6|, |w8-w4|, |w6-w8|.
    float cross = 0.0;
    if (isDiff(w4, w2)) cross += 1.0;
    if (isDiff(w2, w6)) cross += 2.0;
    if (isDiff(w8, w4)) cross += 4.0;
    if (isDiff(w6, w8)) cross += 8.0;

    vec2 index;
    index.x = pattern;
    index.y = cross * 16.0 + dot(floor(fp * 4.0), vec2(1.0, 4.0));

    vec2 lutCoord = index * vec2(0.00390625, 0.00390625) + vec2(0.001953125, 0.001953125);
    vec4 weights = texture(uLut, lutCoord);
    weights /= dot(weights, vec4(1.0));

    vec3 res = weights.x * TMP1 + weights.y * TMP2 + weights.z * TMP3 + weights.w * TMP4;
    FragColor = vec4(res, 1.0);
}