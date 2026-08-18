#version 460

// A full-screen triangle rather than a quad. One triangle covering the
// viewport has no interior edge, so it cannot crack along a diagonal, and it
// rasterizes as one primitive rather than two.
//
// No vertex buffer: the three corners come from the vertex index, which is
// what lets the sky be drawn without a packet describing any geometry.
layout(location = 0) out vec2 skyNdc;

void main()
{
    // (-1,-1), (3,-1), (-1,3): a triangle whose clipped interior is exactly
    // the viewport.
    vec2 corner = vec2(
        (gl_VertexIndex == 1) ? 3.0 : -1.0,
        (gl_VertexIndex == 2) ? 3.0 : -1.0);
    skyNdc = corner;
    // Depth 1.0 -- the far plane. The sky is drawn first with depth writes
    // off, so every later fragment wins on depth.
    gl_Position = vec4(corner, 1.0, 1.0);
}
