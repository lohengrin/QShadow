#version 330 core

// CRT-interlaced (screen curvature variant), single pass.
//
// Ported from the libretro glsl-shaders crt/shaders/crt-geom.glsl:
//     Copyright (C) 2010-2012 cgwg, Themaister and DOLLS
//     Licensed under the GNU General Public License, version 2 or later
//     (see https://www.gnu.org/licenses/gpl-2.0.html)
//
// Changes for this app: standalone GLSL 330 core, all shader parameters baked
// to the upstream defaults, curvature geometry (maxscale/aspect) computed on
// the CPU once per frame and passed as uniforms.

uniform sampler2D uTex;
uniform vec2 uTexSize;   // source texture size, pixels
uniform vec2 uInSize;    // source frame size, pixels
uniform vec2 uOutSize;   // target rect size, pixels
uniform vec2 uAspect;    // target aspect (x/y, 1.0)
uniform vec3 uStretch;   // maxscale geometry
uniform float uModScale; // TextureSize.x * OutputSize.x / InputSize.x

in vec2 v_uv;
out vec4 FragColor;

// Defaults from the upstream #pragma/#define block.
const float CRTgamma = 2.4;
const float monitorgamma = 2.2;
const float d = 1.6;
const float CURVATURE = 1.0;
const float R = 2.0;
const float cornersize = 0.03;
const float cornersmooth = 1000.0;
const float x_tilt = 0.0;
const float y_tilt = 0.0;
const float overscan_x = 100.0;
const float overscan_y = 100.0;
const float DOTMASK = 0.3;
const float SHARPER = 1.0;
const float scanline_weight = 0.3;
const float lum = 0.0;
const float interlace_detect = 1.0;
const float SATURATION = 1.0;
const float INV = 1.0;

vec2 sinangle = vec2(sin(x_tilt), sin(y_tilt)) + vec2(0.001);
vec2 cosangle = vec2(cos(x_tilt), cos(y_tilt)) + vec2(0.001);

#define LINEAR_PROCESSING
#define OVERSAMPLE
#define FIX(c) max(abs(c), 1e-5)
#define PI 3.141592653589
#define TEX2D(c) pow(texture(uTex, (c)), vec4(CRTgamma))

float intersect(vec2 xy)
{
    float A = dot(xy, xy) + d * d;
    float B = 2.0 * (R * (dot(xy, sinangle) - d * cosangle.x * cosangle.y) - d * d);
    float C = d * d + 2.0 * R * d * cosangle.x * cosangle.y;
    return (-B - sqrt(B * B - 4.0 * A * C)) / (2.0 * A);
}

vec2 bkwtrans(vec2 xy)
{
    float c = intersect(xy);
    vec2 point = vec2(c) * xy;
    point -= vec2(-R) * sinangle;
    point /= vec2(R);
    vec2 tang = sinangle / cosangle;
    vec2 poc = point / cosangle;
    float A = dot(tang, tang) + 1.0;
    float B = -2.0 * dot(poc, tang);
    float C = dot(poc, poc) - 1.0;
    float a = (-B + sqrt(B * B - 4.0 * A * C)) / (2.0 * A);
    vec2 uv = (point - a * sinangle) / cosangle;
    float r = FIX(R * acos(a));
    return uv * r / sin(r / R);
}

vec2 transform(vec2 coord)
{
    coord *= uTexSize / uInSize;
    coord = (coord - vec2(0.5)) * uAspect * uStretch.z + uStretch.xy;
    return (bkwtrans(coord) / vec2(overscan_x / 100.0, overscan_y / 100.0) / uAspect + vec2(0.5)) * uInSize / uTexSize;
}

float corner(vec2 coord)
{
    coord *= uTexSize / uInSize;
    coord = (coord - vec2(0.5)) * vec2(overscan_x / 100.0, overscan_y / 100.0) + vec2(0.5);
    coord = min(coord, vec2(1.0) - coord) * uAspect;
    vec2 cdist = vec2(cornersize);
    coord = (cdist - min(coord, cdist));
    float dist = sqrt(dot(coord, coord));
    return clamp((cdist.x - dist) * cornersmooth, 0.0, 1.0) * 1.0001;
}

vec4 scanlineWeights(float distance, vec4 color)
{
    vec4 wid = 2.0 + 2.0 * pow(color, vec4(4.0));
    vec4 weights = vec4(distance / scanline_weight);
    return (lum + 1.4) * exp(-pow(weights * inversesqrt(0.5 * wid), wid)) / (0.6 + 0.2 * wid);
}

vec3 saturation(vec3 textureColor)
{
    float l = length(textureColor) * 0.5775;
    vec3 luminanceWeighting = vec3(0.3, 0.6, 0.1);
    if (l < 0.5)
        luminanceWeighting.rgb = (luminanceWeighting.rgb * luminanceWeighting.rgb) + (luminanceWeighting.rgb * luminanceWeighting.rgb);
    float luminance = dot(textureColor, luminanceWeighting);
    vec3 greyScaleColor = vec3(luminance);
    return mix(greyScaleColor, textureColor, SATURATION);
}

const vec3 pwr = vec3(1.0 / ((-0.7 * (1.0 - scanline_weight) + 1.0) * (-0.5 * DOTMASK + 1.0)) - 1.25);

vec3 inv_gamma(vec3 col, vec3 power)
{
    vec3 cir = col - 1.0;
    cir *= cir;
    col = mix(sqrt(col), sqrt(1.0 - cir), power);
    return col;
}

void main()
{
    vec2 xy = (CURVATURE > 0.5) ? transform(v_uv) : v_uv;
    float cval = corner(xy);

    vec2 ilfac = vec2(1.0, clamp(floor(uInSize.y / 200.0), 1.0, 2.0));
    vec2 one = ilfac / vec2(SHARPER * uTexSize.x, uTexSize.y);
    float mod_factor = v_uv.x * uModScale;

    vec2 ratio_scale = (xy * uTexSize - vec2(0.5)) / ilfac;
    float filter_ = uInSize.y / uOutSize.y;
    vec2 uv_ratio = fract(ratio_scale);

    // Snap to the center of the underlying texel.
    xy = (floor(ratio_scale) * ilfac + vec2(0.5)) / uTexSize;

    // Lanczos coefficients along the scanline.
    vec4 coeffs = PI * vec4(1.0 + uv_ratio.x, uv_ratio.x, 1.0 - uv_ratio.x, 2.0 - uv_ratio.x);
    coeffs = FIX(coeffs);
    coeffs = 2.0 * sin(coeffs) * sin(coeffs / 2.0) / (coeffs * coeffs);
    coeffs /= dot(coeffs, vec4(1.0));

    vec4 col = clamp(mat4(TEX2D(xy + vec2(-one.x, 0.0)),
                          TEX2D(xy),
                          TEX2D(xy + vec2(one.x, 0.0)),
                          TEX2D(xy + vec2(2.0 * one.x, 0.0))) * coeffs,
                     0.0, 1.0);
    vec4 col2 = clamp(mat4(TEX2D(xy + vec2(-one.x, one.y)),
                           TEX2D(xy + vec2(0.0, one.y)),
                           TEX2D(xy + one),
                           TEX2D(xy + vec2(2.0 * one.x, one.y))) * coeffs,
                      0.0, 1.0);

    vec4 weights = scanlineWeights(uv_ratio.y, col);
    vec4 weights2 = scanlineWeights(1.0 - uv_ratio.y, col2);
    uv_ratio.y = uv_ratio.y + 1.0 / 3.0 * filter_;
    weights = (weights + scanlineWeights(uv_ratio.y, col)) / 3.0;
    weights2 = (weights2 + scanlineWeights(abs(1.0 - uv_ratio.y), col2)) / 3.0;
    uv_ratio.y = uv_ratio.y - 2.0 / 3.0 * filter_;
    weights = weights + scanlineWeights(abs(uv_ratio.y), col) / 3.0;
    weights2 = weights2 + scanlineWeights(abs(1.0 - uv_ratio.y), col2) / 3.0;

    vec3 mul_res = (col * weights + col2 * weights2).rgb * vec3(cval);

    // Dot-mask emulation: output pixels alternately tinted green and magenta.
    vec3 dotMaskWeights = mix(vec3(1.0, 1.0 - DOTMASK, 1.0),
                              vec3(1.0 - DOTMASK, 1.0, 1.0 - DOTMASK),
                              floor(mod(mod_factor, 2.0)));
    mul_res *= dotMaskWeights;

    if (INV == 1.0)
        mul_res = inv_gamma(mul_res, pwr);
    else
        mul_res = pow(mul_res, vec3(1.0 / monitorgamma));

    mul_res = saturation(mul_res);

    FragColor = vec4(mul_res, 1.0);
}