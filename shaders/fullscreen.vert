#version 450

// Fullscreen triangle generated entirely from the vertex index, so no vertex
// buffer is needed. uv (0,0) sits at the top-left of the viewport (Vulkan has
// its NDC y-axis pointing down), matching the source frame's first scanline.

layout(location = 0) out vec2 v_uv;

void main()
{
    const vec2 uv = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    v_uv = uv;
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}