#version 330 core

// Scaled fullscreen blit used to downsample the capture texture to its native
// source resolution. Combined with a GL_NEAREST filter, uScale selects every
// Nth source texel (point decimation), recovering the original pixels that the
// ShadowCast upscaler duplicated.

uniform vec2 uScale; // source pixels per output pixel (2.0 or 4.0)

layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec2 a_uv;
out vec2 v_uv;

void main()
{
    gl_Position = vec4(a_pos, 0.0, 1.0);
    v_uv = a_uv * uScale;
}