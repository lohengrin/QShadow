#version 330 core

// Fullscreen blit for the optional pre-shader downscale pass.
//
// The decimation itself comes from rendering this quad into an FBO that is
// 1/factor the size of the frame with GL_NEAREST filtering: each (fewer)
// output texel point-samples every Nth source texel, recovering the native
// grid that the source/receiver pre-upscaled. uv must stay unscaled (0..1) —
// multiplying it by the factor would sample only the top-left corner and
// clamp the rest.
//
// V is flipped: the source frame has row 0 = image top (uploaded verbatim),
// but an FBO's texture has its row 0 at the framebuffer bottom, so without
// the flip the intermediate comes out vertically mirrored.

layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec2 a_uv;
out vec2 v_uv;

void main()
{
    gl_Position = vec4(a_pos, 0.0, 1.0);
    v_uv = vec2(a_uv.x, 1.0 - a_uv.y);
}