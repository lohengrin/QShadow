#pragma once

#include "videoframe.h"

#include <QImage>
#include <QRect>
#include <QSize>
#include <QVulkanWindowRenderer>

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

#include <vulkan/vulkan.h>

class VideoWidget;
class QVulkanDeviceFunctions;

// Push constants shared by every graphics pass. The GLSL Params blocks in all
// fragment shaders declare the same members in the same order, so the layout
// (std430, 48 bytes total) is positional:
//   vec2 uTexSize;  vec2 uInSize;  vec2 uOutSize;  vec2 uAspect;
//   vec3 uStretch;  float uModScale;
struct PushParams {
    float uTexSize[2];
    float uInSize[2];
    float uOutSize[2];
    float uAspect[2];
    float uStretch[3];
    float uModScale;
};

// All Vulkan rendering. Owns the textures, offscreen/FSR passes and pipelines,
// and runs entirely on QVulkanWindow's render path (startNextFrame()). The GUI
// thread only feeds QImages through VideoWidget.
class VulkanRenderer : public QVulkanWindowRenderer {
public:
    explicit VulkanRenderer(VideoWidget *window);
    ~VulkanRenderer() override;

    void initResources() override;
    void initSwapChainResources() override;
    void releaseSwapChainResources() override;
    void releaseResources() override;
    void startNextFrame() override;

    enum class Program {
        Passthrough = 0,
        Hq4x,
        Xbrz,
        CrtGeom,
        CrtRoyale,
    };

private:
    struct ImageObj {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
    };

    struct PerSwap {
        VkBuffer ubo[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
        VkDeviceMemory uboMem[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
        void *uboMapped[2] = {nullptr, nullptr};

        VkBuffer staging = VK_NULL_HANDLE;
        VkDeviceMemory stagingMem = VK_NULL_HANDLE;
        void *stagingMapped = nullptr;
        VkDeviceSize stagingSize = 0;

        VkDescriptorSet gfxSet = VK_NULL_HANDLE;
        VkDescriptorSet downSet = VK_NULL_HANDLE;
        VkDescriptorSet compSets[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
        VkDescriptorSet yuvSet = VK_NULL_HANDLE;
    };

    VideoWidget *m_win;
    QVulkanDeviceFunctions *m_d = nullptr;
    VkPhysicalDeviceMemoryProperties m_memProps{};

    void destroyImage(ImageObj &img);
    void destroySwapSlot(PerSwap &slot);
    void waitIdle();

    // ---- resource helpers ----
    bool loadSpirv(const char *resource, std::vector<std::uint32_t> *out) const;
    VkShaderModule createShaderModule(const char *resource);
    std::uint32_t findMemoryType(std::uint32_t typeBits, VkMemoryPropertyFlags required,
                                 VkMemoryPropertyFlags preferred) const;

    void createImage(VkFormat format, const QSize &size, VkImageUsageFlags usage,
                     VkSampleCountFlagBits samples, ImageObj *out);
void createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                      VkMemoryPropertyFlags props, VkBuffer *buffer, VkDeviceMemory *memory,
                      void **mapped);
    void flushRange(VkDeviceMemory memory, VkDeviceSize size);
    void uploadImageBlocking(const QImage &img, VkFormat format,
                             VkImageUsageFlags usage, ImageObj *out);

    void ensureFrameResources(const QSize &size);
    void ensureDownResources(const QSize &size);
    void ensureFsrResources(const QSize &size);
    void ensureYuvResources(const VideoFrame &frame);
    void ensureStaging(VkDeviceSize needed, PerSwap *slot);

    void transition(VkCommandBuffer cb, VkImage img, VkImageLayout oldLayout,
                    VkImageLayout newLayout, VkAccessFlags srcAccess,
                    VkAccessFlags dstAccess, VkPipelineStageFlags srcStage,
                    VkPipelineStageFlags dstStage);

    VkPipeline createGfxPipeline(const char *fragResource, VkRenderPass renderPass, bool hasDepth);
    VkPipeline createComputePipeline(const char *compResource, VkPipelineLayout layout = VK_NULL_HANDLE);
    void destroyPipeline(VkPipeline &pipe);

    // ---- per-frame helpers ----
    void uploadFrame(const QImage &img, PerSwap *slot, VkCommandBuffer cb);
    // Planar YUV frame: upload the three planes, then convert to RGBA into
    // m_frame with a compute pass so everything downstream is unchanged.
    void uploadYuvFrame(const VideoFrame &frame, PerSwap *slot, VkCommandBuffer cb);
    // Packed YUYV frame: upload as a half-width RGBA8 texture and convert.
    void uploadYuyvFrame(const VideoFrame &frame, PerSwap *slot, VkCommandBuffer cb);
    // Shared tail of both: run `pipe` over the sampled planes (descriptors for
    // bindings 0..2 are written by the caller) into m_frame.
    void convertToFrame(VkCommandBuffer cb, PerSwap *slot, VkPipeline pipe, const QSize &size);
    void updateGfxDescriptors(VkDescriptorSet set, VkImageView srcView, VkSampler srcSampler);
    void updateCompDescriptors(VkDescriptorSet compSet, VkBuffer ubo, VkImageView inputView,
                               VkImageView outputView);
    void writeEasuConsts(PerSwap *slot, const QSize &src, const QSize &out);
    void writeRcasConsts(PerSwap *slot, float sharpness);
    void setViewportRect(VkCommandBuffer cb, const QRect &rect);

    void recordDownscale(VkCommandBuffer cb, PerSwap *slot, const QSize &target);
    void recordEasu(VkCommandBuffer cb, VkDescriptorSet set, const QSize &outSize);
    void recordRcas(VkCommandBuffer cb, VkDescriptorSet set, const QSize &outSize);
    void recordBlit(VkCommandBuffer cb, PerSwap *slot, VkImageView srcView,
                    VkSampler sampler, const QRect &target, Program program,
                    const PushParams &pp);
    // Records everything for one frame. It may return early at any point; the
    // caller (startNextFrame) completes the frame either way. *drewPass is set
    // once the swapchain pass has been recorded, *shownStamp when a newly
    // decoded frame was drawn (for the latency readout).
    void recordFrame(bool *drewPass,
                     std::optional<std::chrono::steady_clock::time_point> *shownStamp);
    // A cleared swapchain pass: leaves the image presentable when nothing was drawn.
    void presentClearedFrame();
    void beginSwapPass(VkCommandBuffer cb, const QSize &outSize);
    void endSwapPass(VkCommandBuffer cb);

    // ---- pipeline / layout objects ----
    VkDescriptorSetLayout m_texLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_compLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_gfxPipeLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_compPipeLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_pool = VK_NULL_HANDLE;

    VkSampler m_samplerNearest = VK_NULL_HANDLE;
    VkSampler m_samplerLinear = VK_NULL_HANDLE;

    VkRenderPass m_offscreenPass = VK_NULL_HANDLE;
    VkPipeline m_pipeDown = VK_NULL_HANDLE;
    std::array<VkPipeline, 5> m_pipeSwap = {};
    VkPipeline m_pipeEasu = VK_NULL_HANDLE;
    VkPipeline m_pipeRcas = VK_NULL_HANDLE;

    // GPU YUV -> RGBA conversion (planar JPEG frames).
    VkDescriptorSetLayout m_yuvSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_yuvPipeLayout = VK_NULL_HANDLE;
    VkPipeline m_pipeYuv = VK_NULL_HANDLE;
    VkPipeline m_pipeYuyv = VK_NULL_HANDLE;
    bool m_yuvOk = false;
    ImageObj m_yuv[3];       // Y, Cb, Cr (R8_UNORM)
    QSize m_yuvSize[3];
    ImageObj m_yuyv;         // packed YUYV as RGBA8, width / 2
    QSize m_yuyvSize;

    std::vector<PerSwap> m_perSwap;

    ImageObj m_lut;
    ImageObj m_frame;
    VkImageLayout m_frameLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    QSize m_frameSize;

    ImageObj m_down;
    QSize m_downSize;
    VkFramebuffer m_downFbo = VK_NULL_HANDLE;

    ImageObj m_fsrA;
    ImageObj m_fsrB;
    QSize m_fsrSize;

    // CRT-Geom stretch factors for m_crtAspect (recomputed only when it changes).
    float m_crtAspect = -1.0f;
    float m_crtStretch[3] = {0.0f, 0.0f, 0.0f};
};