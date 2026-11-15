#version 450

// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Guillaume Millet

// Plain texture sample. "None" uses NEAREST filtering, "Bilinear smooth"
// uses LINEAR filtering; the mode only changes the sampler bound at set 0.

layout(set = 0, binding = 0) uniform sampler2D uTex;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 FragColor;

void main()
{
    FragColor = vec4(texture(uTex, v_uv).rgb, 1.0);
}