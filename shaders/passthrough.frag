#version 330 core

// Plain texture sample. "None" uses NEAREST filtering, "Bilinear smooth"
// uses LINEAR filtering; the mode only changes texture parameters.

uniform sampler2D uTex;

in vec2 v_uv;
out vec4 FragColor;

void main()
{
    FragColor = vec4(texture(uTex, v_uv).rgb, 1.0);
}