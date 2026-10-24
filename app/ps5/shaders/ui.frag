#version 450
// The UI pass: the atlas holds distances to the glyph's edge (0.5 = the edge); one smooth step a pixel wide cuts
// it at any size, a second one further out draws the outline. Premultiplied out (blend ONE, ONE_MINUS_SRC_ALPHA).
// The same maths as src/ui/raster.h (the host pictures) and src/platform/xbox/shaders/ui.hlsl.
layout(binding = 0) uniform sampler2D atlas;

layout(location = 0) in vec2 inUV;
layout(location = 1) in vec4 inFill;
layout(location = 2) in vec4 inLine;
layout(location = 3) in vec2 inParams;

layout(location = 0) out vec4 outColor;

void main() {
  float d = texture(atlas, inUV).r;
  float w = max(fwidth(d) * 0.75, 1e-4);
  float edge = 0.5 - inParams.x;
  float fill = smoothstep(edge - w, edge + w, d);
  float outer = inParams.y > 0.0 ? smoothstep(edge - inParams.y - w, edge - inParams.y + w, d) : fill;
  float fa = inFill.a * fill;
  float la = inLine.a * max(outer - fill, 0.0);
  outColor = vec4(inFill.rgb * fa + inLine.rgb * la, fa + la);
}
