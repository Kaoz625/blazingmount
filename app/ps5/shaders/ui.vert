#version 450
// The UI pass (src/platform/ps5/ui_vk.cpp, src/ui/draw.h): textured quads in the fight's 1080p layout.
layout(location = 0) in vec2 inPos;      // layout pixels, y down
layout(location = 1) in vec2 inUV;       // the font atlas
layout(location = 2) in vec4 inFill;     // sRGB bytes + alpha (R8G8B8A8_UNORM)
layout(location = 3) in vec4 inLine;     // the outline's colour
layout(location = 4) in vec2 inParams;   // x: edge bias (bolder), y: outline width (distance units)

layout(push_constant) uniform Push {
  vec4 p;   // x: 1 when the swapchain is sRGB (it encodes on write: hand it linear colours)
} push;

layout(location = 0) out vec2 outUV;
layout(location = 1) out vec4 outFill;
layout(location = 2) out vec4 outLine;
layout(location = 3) out vec2 outParams;

vec3 toLinear(vec3 c) {
  return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}

void main() {
  outUV = inUV;
  outParams = inParams;
  outFill = inFill;
  outLine = inLine;
  if (push.p.x > 0.5) {
    outFill.rgb = toLinear(inFill.rgb);
    outLine.rgb = toLinear(inLine.rgb);
  }
  gl_Position = vec4(inPos.x / 960.0 - 1.0, inPos.y / 540.0 - 1.0, 0.0, 1.0);
}
