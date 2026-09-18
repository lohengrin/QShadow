#version 450

// Single-pass CRT emulation in the style of the libretro crt-royale
// "fake-bloom" preset (aperture grille, scanlines, halation bloom, gentle
// screen curvature, vignette).
//
// NOTE: this is an original, simplified single-pass adaptation written for
// this project (the upstream crt-royale is a multi-pass pipeline with
// offscreen buffers). It is not derived directly from libretro shader source;
// it only mimics the projected look. See ATTRIBUTION.md.

layout(set = 0, binding = 0) uniform sampler2D uTex;

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

// Curvature: pinch uv coordinates toward the center (barrel shape).
vec2 curve(vec2 uv)
{
    vec2 p = (uv - 0.5) * 2.0;
    float r2 = dot(p, p);
    p *= 1.0 + 0.10 * r2;
    return p * 0.5 + 0.5;
}

// 3x3 weighted box blur of a linearized frame, for the halation halo.
vec3 blur3x3(vec2 uv)
{
    vec2 px = 1.0 / uTexSize;
    vec3 acc = vec3(0.0);
    acc += texture(uTex, uv + px * vec2(-1.0, -1.0)).rgb * 0.0625;
    acc += texture(uTex, uv + px * vec2( 0.0, -1.0)).rgb * 0.125;
    acc += texture(uTex, uv + px * vec2( 1.0, -1.0)).rgb * 0.0625;
    acc += texture(uTex, uv + px * vec2(-1.0,  0.0)).rgb * 0.125;
    acc += texture(uTex, uv + px * vec2( 0.0,  0.0)).rgb * 0.25;
    acc += texture(uTex, uv + px * vec2( 1.0,  0.0)).rgb * 0.125;
    acc += texture(uTex, uv + px * vec2(-1.0,  1.0)).rgb * 0.0625;
    acc += texture(uTex, uv + px * vec2( 0.0,  1.0)).rgb * 0.125;
    acc += texture(uTex, uv + px * vec2( 1.0,  1.0)).rgb * 0.0625;
    return acc;
}

// Aperture grille phosphor mask, as a function of output pixel column.
vec3 maskWeights(vec2 outCoord)
{
    float x = outCoord.x;
    float col = mod(x, 3.0);
    if (col < 1.0)
        return vec3(1.0, 0.55, 0.55);
    if (col < 2.0)
        return vec3(0.55, 1.0, 0.55);
    return vec3(0.55, 0.55, 1.0);
}

void main()
{
    vec2 uv = curve(v_uv);

    // Everything outside the curved viewport is black.
    float edge = step(0.0, uv.x) * step(uv.x, 1.0) * step(0.0, uv.y) * step(uv.y, 1.0);

    vec3 col = texture(uTex, uv).rgb;

    // Linearize into light space.
    vec3 lin = pow(col, vec3(2.4));
    // Fake bloom: half of the "halation" is a blurred copy.
    vec3 halo = mix(lin, blur3x3(uv), 0.35);
    vec3 bright = max(vec3(0.0), lin - 0.75);
    vec3 res = halo + 0.55 * bright;

    // Scanlines along the vertical (output) axis.
    float lineDist = abs(fract(gl_FragCoord.y * 0.5) * 2.0 - 1.0); // 1.0 on a scanline
    float scan = mix(1.15, 0.42, pow(lineDist, 1.5));
    res *= scan;

    // Apply the phosphor mask in light space, then out of linear.
    res *= maskWeights(gl_FragCoord.xy);
    res = pow(res, vec3(1.0 / 2.2));

    // Subtle vignette.
    vec2 center = (v_uv - 0.5) * 2.0;
    float vig = 1.0 - 0.25 * dot(center, center);

    FragColor = vec4(res * vig * edge, 1.0);
}