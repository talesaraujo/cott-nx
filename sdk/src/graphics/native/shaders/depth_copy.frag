#version 450
// Depth resolve: the depth of the source pixel, as a float, unchanged.
layout(set = 0, binding = 0) uniform sampler2D depth_texture;
layout(push_constant) uniform Push { ivec2 offset; } push;  // source pixel minus destination pixel
layout(location = 0) out float depth_out;
void main() {
  depth_out = texelFetch(depth_texture, ivec2(gl_FragCoord.xy) + push.offset, 0).r;
}
