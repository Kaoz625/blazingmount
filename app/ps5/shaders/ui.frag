#version 450
// The UI pass: the atlas holds distances to the glyph's edge (0.5 = the edge); one smooth step a pixel wide cuts
// it at any size, a second one further out draws the outline. Premultiplied out (blend ONE, ONE_MINUS_SRC_ALPHA).
// The same maths as src/ui/raster.h (the host pictures) and src/platform/xbox/shaders/ui.hlsl.
layout(binding = 0) uniform sampler2D atlas;
layout(binding = 1) uniform sampler2D covers;   // BlazingMount: the cover atlas, RGBA sRGB bytes, straight alpha

layout(location = 0) in vec2 inUV;
layout(location = 1) in vec4 inFill;
layout(location = 2) in vec4 inLine;
layout(location = 3) in vec2 inParams;

layout(location = 0) out vec4 outColor;

vec3 toLinear(vec3 c) {
  return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}

void main() {
  if (inParams.y < 0.0) {   // an image quad (draw.h Batch::Image): the cover, dimmed by the fill's alpha
    vec4 c = texture(covers, inUV);
    vec3 rgb = inParams.x > 0.5 ? toLinear(c.rgb) : c.rgb;
    float a = c.a * inFill.a;
    outColor = vec4(rgb * a, a);
    return;
  }
  float d = texture(atlas, inUV).r;
  float w = max(fwidth(d) * 0.75, 1e-4);
  float edge = 0.5 - inParams.x;
  float fill = smoothstep(edge - w, edge + w, d);
  float outer = inParams.y > 0.0 ? smoothstep(edge - inParams.y - w, edge - inParams.y + w, d) : fill;
  float fa = inFill.a * fill;
  float la = inLine.a * max(outer - fill, 0.0);
  outColor = vec4(inFill.rgb * fa + inLine.rgb * la, fa + la);
}
