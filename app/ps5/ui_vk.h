// ui_vk.h - the PS5's UI pass (Vulkan, in mihawk's PS5_VulkanTemplate title): every menu, the HUD with text, the
// training overlays and the blocks view, as one list of quads (src/ui/draw.h) drawn by ONE pipeline after the 3D
// look, inside the base class's render pass. The font atlas (assets/ui/mf-font.sdf) is one R8 texture; each frame's
// vertices go into that frame's slot of a host-visible buffer (two frames in flight, as the look's uniforms).
//
// Like render3d.h, this header is included at file scope from the title's ps5/src/prelude.h (the template compiles
// the program inside a namespace of its own) and ui_vk.cpp is built as its own file in the title.
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "../src/ui/draw.h"

namespace vks {
struct VulkanDevice;
}

namespace mf::ui {

// The cover atlas (BlazingMount app/src/covers.h): RGBA bytes, uploaded again when `version` moves (at most twice a second)
struct ImageSource {
  const uint8_t* rgba = nullptr;
  uint32_t width = 0, height = 0;
  uint64_t version = 0;
};

struct Ps5UiTarget {
  vks::VulkanDevice* device = nullptr;
  VkQueue queue = VK_NULL_HANDLE;
  VkRenderPass renderPass = VK_NULL_HANDLE;
  VkPipelineCache pipelineCache = VK_NULL_HANDLE;
  VkFormat colorFormat = VK_FORMAT_UNDEFINED;
  uint32_t width = 1920, height = 1080;
  uint32_t frameSlot = 0;   // the base class's currentBuffer
};

class Ps5Ui {
 public:
  static constexpr size_t kMaxVertices = 16384;   // a frame's quads x 6 (tests/ui_test.cpp holds every screen under it)
  Ps5Ui();
  ~Ps5Ui();
  Ps5Ui(const Ps5Ui&) = delete;
  Ps5Ui& operator=(const Ps5Ui&) = delete;

  // Records the batch into `cmd` (inside the render pass, after the look). The first call uploads the atlas and
  // makes the pipeline. false: the UI could not start (logged once) - the program draws its rectangles instead.
  bool Draw(VkCommandBuffer cmd, const Ps5UiTarget& target, const Font& font, const std::vector<Vertex>& vertices,
            const ImageSource* images = nullptr);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl;
};

}  // namespace mf::ui
