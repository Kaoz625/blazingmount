// ui_vk.cpp - the PS5's UI pass (ui_vk.h says what it draws and how the program calls it).
//
// One pipeline inside the base class's render pass: the vertex layout of src/ui/draw.h (32 bytes: position, atlas
// coordinates, fill and outline colours as R8G8B8A8_UNORM, edge bias and outline width), premultiplied blending,
// no depth, viewport and scissor dynamic. The atlas is one R8_UNORM texture made by the template's
// vks::Texture2D::fromBuffer (linear filter). Shaders: shaders/ui.vert, shaders/ui.frag (compiled on the Dell by the
// template's compile-shaders.sh into shaders/glsl/multiversefighters/ui.*.spv).
#include "VulkanBuffer.h"
#include "VulkanDevice.h"
#include "VulkanInitializers.hpp"
#include "VulkanTexture.h"
#include "VulkanTools.h"
#include "platform.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

#include "ui_vk.h"

namespace mf::ui {
namespace {

constexpr const char* kShaderDir = "blazingmount/";

double NowMs() {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

}  // namespace

struct Ps5Ui::Impl {
  bool tried = false, ok = false;
  vks::VulkanDevice* vd = nullptr;
  VkDevice dev = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  bool srgbTarget = false;

  vks::Texture2D atlas{};
  bool atlasMade = false;
  vks::Buffer vertices[2];
  VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
  VkDescriptorPool pool = VK_NULL_HANDLE;
  VkDescriptorSet set = VK_NULL_HANDLE;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  VkPipeline pipe = VK_NULL_HANDLE;
  std::vector<VkShaderModule> modules;

  // every ~5 s: the vertices drawn and the CPU time of Draw
  std::vector<float> cpuMs;
  size_t vertexMax = 0;
  double windowStart = 0;
  uint64_t draws = 0, clipped = 0;

  ~Impl() { Destroy(); }

  void Destroy() {
    if (!dev) return;
    vkDeviceWaitIdle(dev);
    if (pipe) vkDestroyPipeline(dev, pipe, nullptr);
    if (layout) vkDestroyPipelineLayout(dev, layout, nullptr);
    for (VkShaderModule m : modules) vkDestroyShaderModule(dev, m, nullptr);
    if (pool) vkDestroyDescriptorPool(dev, pool, nullptr);
    if (setLayout) vkDestroyDescriptorSetLayout(dev, setLayout, nullptr);
    vertices[0].destroy();
    vertices[1].destroy();
    if (atlasMade) atlas.destroy();
    atlasMade = false;
    pipe = VK_NULL_HANDLE;
    layout = VK_NULL_HANDLE;
    modules.clear();
    pool = VK_NULL_HANDLE;
    setLayout = VK_NULL_HANDLE;
    dev = VK_NULL_HANDLE;
  }

  VkPipelineShaderStageCreateInfo Stage(const char* file, VkShaderStageFlagBits stage) {
    const std::string path = getShaderBasePath() + "glsl/" + kShaderDir + file;
    VkPipelineShaderStageCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    info.stage = stage;
    info.module = vks::tools::loadShader(path.c_str(), dev);
    info.pName = "main";
    modules.push_back(info.module);
    return info;
  }

  bool Init(const Ps5UiTarget& t, const Font& font) {
    const double started = NowMs();
    if (!font.ok()) {
      say("ui: no font atlas - menus and the HUD fall back to rectangles");
      return false;
    }
    vd = t.device;
    dev = vd->logicalDevice;
    queue = t.queue;
    srgbTarget = t.colorFormat == VK_FORMAT_B8G8R8A8_SRGB || t.colorFormat == VK_FORMAT_R8G8B8A8_SRGB ||
                 t.colorFormat == VK_FORMAT_A8B8G8R8_SRGB_PACK32;

    atlas.fromBuffer(const_cast<uint8_t*>(font.pixels.data()), font.pixels.size(), VK_FORMAT_R8_UNORM, font.width, font.height, vd, queue,
                     VK_FILTER_LINEAR);
    atlasMade = true;
    for (vks::Buffer& b : vertices) {
      VK_CHECK_RESULT(vd->createBuffer(VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                       &b, kMaxVertices * sizeof(Vertex)));
      VK_CHECK_RESULT(b.map());
    }

    const std::vector<VkDescriptorSetLayoutBinding> bindings = {
        vks::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT, 0)};
    VkDescriptorSetLayoutCreateInfo layoutCI = vks::initializers::descriptorSetLayoutCreateInfo(bindings);
    VK_CHECK_RESULT(vkCreateDescriptorSetLayout(dev, &layoutCI, nullptr, &setLayout));
    const std::vector<VkDescriptorPoolSize> sizes = {vks::initializers::descriptorPoolSize(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1)};
    VkDescriptorPoolCreateInfo poolCI = vks::initializers::descriptorPoolCreateInfo(sizes, 1);
    VK_CHECK_RESULT(vkCreateDescriptorPool(dev, &poolCI, nullptr, &pool));
    VkDescriptorSetAllocateInfo allocInfo = vks::initializers::descriptorSetAllocateInfo(pool, &setLayout, 1);
    VK_CHECK_RESULT(vkAllocateDescriptorSets(dev, &allocInfo, &set));
    const VkWriteDescriptorSet write =
        vks::initializers::writeDescriptorSet(set, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 0, &atlas.descriptor);
    vkUpdateDescriptorSets(dev, 1, &write, 0, nullptr);

    VkPushConstantRange push = vks::initializers::pushConstantRange(VK_SHADER_STAGE_VERTEX_BIT, 16, 0);
    VkPipelineLayoutCreateInfo plCI = vks::initializers::pipelineLayoutCreateInfo(&setLayout, 1);
    plCI.pushConstantRangeCount = 1;
    plCI.pPushConstantRanges = &push;
    VK_CHECK_RESULT(vkCreatePipelineLayout(dev, &plCI, nullptr, &layout));

    VkPipelineInputAssemblyStateCreateInfo ia = vks::initializers::pipelineInputAssemblyStateCreateInfo(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0, VK_FALSE);
    VkPipelineRasterizationStateCreateInfo rs =
        vks::initializers::pipelineRasterizationStateCreateInfo(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE, 0);
    VkPipelineColorBlendAttachmentState blend = vks::initializers::pipelineColorBlendAttachmentState(0xf, VK_TRUE);
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;   // premultiplied
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    VkPipelineColorBlendStateCreateInfo cb = vks::initializers::pipelineColorBlendStateCreateInfo(1, &blend);
    VkPipelineDepthStencilStateCreateInfo ds = vks::initializers::pipelineDepthStencilStateCreateInfo(VK_FALSE, VK_FALSE, VK_COMPARE_OP_ALWAYS);
    VkPipelineViewportStateCreateInfo vp = vks::initializers::pipelineViewportStateCreateInfo(1, 1, 0);
    VkPipelineMultisampleStateCreateInfo ms = vks::initializers::pipelineMultisampleStateCreateInfo(VK_SAMPLE_COUNT_1_BIT, 0);
    const std::vector<VkDynamicState> dyn = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynCI = vks::initializers::pipelineDynamicStateCreateInfo(dyn);
    const std::vector<VkVertexInputBindingDescription> vbind = {
        vks::initializers::vertexInputBindingDescription(0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX)};
    const std::vector<VkVertexInputAttributeDescription> vattr = {
        vks::initializers::vertexInputAttributeDescription(0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, x)),
        vks::initializers::vertexInputAttributeDescription(0, 1, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, u)),
        vks::initializers::vertexInputAttributeDescription(0, 2, VK_FORMAT_R8G8B8A8_UNORM, offsetof(Vertex, fill)),
        vks::initializers::vertexInputAttributeDescription(0, 3, VK_FORMAT_R8G8B8A8_UNORM, offsetof(Vertex, line)),
        vks::initializers::vertexInputAttributeDescription(0, 4, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, bias))};
    VkPipelineVertexInputStateCreateInfo input = vks::initializers::pipelineVertexInputStateCreateInfo(vbind, vattr);
    std::array<VkPipelineShaderStageCreateInfo, 2> stages = {Stage("ui.vert.spv", VK_SHADER_STAGE_VERTEX_BIT),
                                                             Stage("ui.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT)};
    VkGraphicsPipelineCreateInfo pci = vks::initializers::pipelineCreateInfo(layout, t.renderPass, 0);
    pci.pInputAssemblyState = &ia;
    pci.pRasterizationState = &rs;
    pci.pColorBlendState = &cb;
    pci.pDepthStencilState = &ds;
    pci.pViewportState = &vp;
    pci.pMultisampleState = &ms;
    pci.pDynamicState = &dynCI;
    pci.pVertexInputState = &input;
    pci.stageCount = 2;
    pci.pStages = stages.data();
    VK_CHECK_RESULT(vkCreateGraphicsPipelines(dev, t.pipelineCache, 1, &pci, nullptr, &pipe));
    say("ui: font atlas %ux%u (%zu glyphs, %zu kerning pairs), one pipeline, %zu vertices a frame, swapchain %s; ready in %.1f ms",
        font.width, font.height, font.glyphs.size(), font.kerns.size(), kMaxVertices, srgbTarget ? "sRGB (linear colours handed over)" : "UNORM",
        NowMs() - started);
    return true;
  }

  bool Draw(VkCommandBuffer cmd, const Ps5UiTarget& t, const Font& font, const std::vector<Vertex>& vs) {
    const double started = NowMs();
    if (!tried) {
      tried = true;
      try {
        ok = Init(t, font);
      } catch (const std::exception& e) {
        say("ui: start-up failed (%s) - rectangles instead", e.what());
        ok = false;
      } catch (...) {
        say("ui: start-up failed - rectangles instead");
        ok = false;
      }
      if (!ok) Destroy();
    }
    if (!ok) return false;
    const uint32_t slot = t.frameSlot & 1u;
    size_t n = std::min(vs.size(), kMaxVertices);
    n -= n % 6;
    if (n < vs.size()) ++clipped;
    if (n > 0) {
      std::memcpy(vertices[slot].mapped, vs.data(), n * sizeof(Vertex));
      const VkViewport viewport = vks::initializers::viewport(static_cast<float>(t.width), static_cast<float>(t.height), 0.0f, 1.0f);
      const VkRect2D scissor = vks::initializers::rect2D(static_cast<int32_t>(t.width), static_cast<int32_t>(t.height), 0, 0);
      vkCmdSetViewport(cmd, 0, 1, &viewport);
      vkCmdSetScissor(cmd, 0, 1, &scissor);
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
      const float push[4] = {srgbTarget ? 1.0f : 0.0f, 0, 0, 0};
      vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), push);
      const VkDeviceSize zero = 0;
      vkCmdBindVertexBuffers(cmd, 0, 1, &vertices[slot].buffer, &zero);
      vkCmdDraw(cmd, static_cast<uint32_t>(n), 1, 0, 0);
    }
    vertexMax = std::max(vertexMax, vs.size());
    cpuMs.push_back(static_cast<float>(NowMs() - started));
    if (windowStart == 0) windowStart = started;
    if (started - windowStart >= 5000.0) {
      std::vector<float> s = cpuMs;
      std::sort(s.begin(), s.end());
      double sum = 0;
      for (float v : s) sum += v;
      say("ui: %zu frames: Draw on the CPU avg %.3f ms, p99 %.3f; up to %zu vertices (%zu quads) a frame%s", s.size(), sum / s.size(),
          s[s.size() * 99 / 100], vertexMax, vertexMax / 6, clipped ? "; SOME FRAMES CLIPPED" : "");
      cpuMs.clear();
      vertexMax = 0;
      windowStart = started;
    }
    ++draws;
    return true;
  }
};

Ps5Ui::Ps5Ui() : impl(std::make_unique<Impl>()) {}
Ps5Ui::~Ps5Ui() = default;

bool Ps5Ui::Draw(VkCommandBuffer cmd, const Ps5UiTarget& target, const Font& font, const std::vector<Vertex>& vertices) {
  return impl->Draw(cmd, target, font, vertices);
}

}  // namespace mf::ui
