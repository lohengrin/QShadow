#include "vulkanrenderer.h"

#include "fsrconsts.h"
#include "videowidget.h"

#include <QDebug>
#include <QFile>
#include <QImage>
#include <QRect>
#include <QVulkanDeviceFunctions>
#include <QVulkanFunctions>
#include <QVulkanInstance>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace {

constexpr VkFormat kFrameFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkFormat kFsrFormat = VK_FORMAT_R32G32B32A32_SFLOAT;
constexpr VkDeviceSize kUboSize = 256;
// Stages that sample the per-renderer images shared by all in-flight frames.
constexpr VkPipelineStageFlags kSharedImageReaders =
    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;

constexpr VkFormat kPlaneFormat = VK_FORMAT_R8_UNORM;

// STORAGE: the YUV->RGBA compute pass writes the frame image directly.
constexpr VkImageUsageFlags kFrameUsage =
    VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
    VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
constexpr VkImageUsageFlags kPlaneUsage =
    VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
constexpr VkImageUsageFlags kDownUsage =
    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
    VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
constexpr VkImageUsageFlags kFsrUsage =
    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
    VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

// CPU port of the crt-geom maxscale() vertex block, identical to the old
// OpenGL implementation. Depends only on the output aspect ratio.
void crtGeomStretch(float aspect, float out[3])
{
    const float d = 1.6f;
    const float R = 2.0f;
    const float cosA = 1.001f;
    const float sinA = 0.001f;

    const auto intersect = [&](float x, float y) {
        const float xy = x * x + y * y;
        const float A = xy + d * d;
        const float B = 2.0f * (R * (x * sinA + y * sinA - d * cosA * cosA) - d * d);
        const float C = d * d + 2.0f * R * d * cosA * cosA;
        return (-B - std::sqrt(B * B - 4.0f * A * C)) / (2.0f * A);
    };
    const auto bkwtrans = [&](float px, float py) {
        const float c = intersect(px, py);
        float ux = c * px + R * sinA;
        float uy = c * py + R * sinA;
        ux /= R;
        uy /= R;
        const float t0 = sinA / cosA;
        const float p0x = ux / cosA;
        const float p0y = uy / cosA;
        const float A = t0 * t0 + t0 * t0 + 1.0f;
        const float B = -2.0f * (p0x * t0 + p0y * t0);
        const float C = p0x * p0x + p0y * p0y - 1.0f;
        const float a = (-B + std::sqrt(B * B - 4.0f * A * C)) / (2.0f * A);
        ux = (ux - a * sinA) / cosA;
        uy = (uy - a * sinA) / cosA;
        const float r = std::max(std::abs(R * std::acos(a)), 1e-5f);
        const float s = std::sin(r / R) / r;
        return std::pair<float, float>(ux * s, uy * s);
    };
    const auto fwtrans = [&](float ux, float uy) {
        const float r = std::max(std::sqrt(ux * ux + uy * uy), 1e-5f);
        const float sr = std::sin(r / R) / r;
        ux *= sr;
        uy *= sr;
        const float x = 1.0f - std::cos(r / R);
        const float D = d / R + x * cosA * cosA + ux * sinA + uy * sinA;
        return std::pair<float, float>(d * (ux * cosA - x * sinA) / D,
                                       d * (uy * cosA - x * sinA) / D);
    };

    const auto c = bkwtrans(-R * sinA / (1.0f + R / d * cosA * cosA),
                            -R * sinA / (1.0f + R / d * cosA * cosA));
    const float aa = 0.5f * aspect;
    const auto lo = fwtrans(-aa, c.second);
    const auto hi = fwtrans(aa, c.second);
    const auto lo2 = fwtrans(c.first, -0.5f);
    const auto hi2 = fwtrans(c.first, 0.5f);

    const float loX = lo.first / aspect;
    const float hiX = hi.first / aspect;
    const float loY = lo2.second;
    const float hiY = hi2.second;
    out[0] = (hiX + loX) * aspect * 0.5f;
    out[1] = (hiY + loY) * 0.5f;
    out[2] = std::max(hiX - loX, hiY - loY);
}

const char *programResource(VulkanRenderer::Program program)
{
    switch (program) {
    case VulkanRenderer::Program::Passthrough:
        return ":/shaders/passthrough.frag.spv";
    case VulkanRenderer::Program::Hq4x:
        return ":/shaders/hq4x.frag.spv";
    case VulkanRenderer::Program::Xbrz:
        return ":/shaders/xbrz.frag.spv";
    case VulkanRenderer::Program::CrtGeom:
        return ":/shaders/crt-geom.frag.spv";
    case VulkanRenderer::Program::CrtRoyale:
        return ":/shaders/crt-royale.frag.spv";
    }
    return nullptr;
}

} // namespace

VulkanRenderer::VulkanRenderer(VideoWidget *window)
    : m_win(window)
{
}

VulkanRenderer::~VulkanRenderer() = default;

// ---------------------------------------------------------------------------
// Resource helpers
// ---------------------------------------------------------------------------

bool VulkanRenderer::loadSpirv(const char *resource, std::vector<std::uint32_t> *out) const
{
    QFile f(QString::fromLatin1(resource));
    if (!f.open(QIODevice::ReadOnly))
        return false;
    const QByteArray bytes = f.readAll();
    if (bytes.size() % 4 != 0 || bytes.isEmpty())
        return false;
    out->resize(std::size_t(bytes.size()) / 4);
    std::memcpy(out->data(), bytes.constData(), std::size_t(bytes.size()));
    return true;
}

VkShaderModule VulkanRenderer::createShaderModule(const char *resource)
{
    std::vector<std::uint32_t> code;
    if (!loadSpirv(resource, &code))
        return VK_NULL_HANDLE;

    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = code.size() * sizeof(std::uint32_t);
    info.pCode = code.data();
    VkShaderModule module = VK_NULL_HANDLE;
    if (m_d->vkCreateShaderModule(m_win->device(), &info, nullptr, &module) != VK_SUCCESS) {
        qWarning() << "QShadow: failed to create shader module" << resource;
        return VK_NULL_HANDLE;
    }
    return module;
}

std::uint32_t VulkanRenderer::findMemoryType(std::uint32_t typeBits,
                                             VkMemoryPropertyFlags required,
                                             VkMemoryPropertyFlags preferred) const
{
    for (std::uint32_t i = 0; i < m_memProps.memoryTypeCount; ++i) {
        if (typeBits & (1u << i)) {
            const VkMemoryPropertyFlags flags = m_memProps.memoryTypes[i].propertyFlags;
            if ((flags & required) == required && (flags & preferred) == preferred)
                return i;
        }
    }
    for (std::uint32_t i = 0; i < m_memProps.memoryTypeCount; ++i) {
        if (typeBits & (1u << i)) {
            const VkMemoryPropertyFlags flags = m_memProps.memoryTypes[i].propertyFlags;
            if ((flags & required) == required)
                return i;
        }
    }
    return UINT32_MAX;
}

void VulkanRenderer::createImage(VkFormat format, const QSize &size,
                                 VkImageUsageFlags usage, VkSampleCountFlagBits samples,
                                 ImageObj *out)
{
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent.width = std::uint32_t(size.width());
    info.extent.height = std::uint32_t(size.height());
    info.extent.depth = 1;
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = samples;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkResult res =
        m_d->vkCreateImage(m_win->device(), &info, nullptr, &out->image);
    if (res != VK_SUCCESS) {
        qWarning() << "QShadow: vkCreateImage failed" << res << size;
        return;
    }

    VkMemoryRequirements req{};
    m_d->vkGetImageMemoryRequirements(m_win->device(), out->image, &req);
    const std::uint32_t type =
        findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    if (type == UINT32_MAX) {
        qWarning() << "QShadow: no DEVICE_LOCAL memory type for image" << size;
        m_d->vkDestroyImage(m_win->device(), out->image, nullptr);
        out->image = VK_NULL_HANDLE;
        return;
    }

    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = type;
    res = m_d->vkAllocateMemory(m_win->device(), &alloc, nullptr, &out->memory);
    if (res != VK_SUCCESS) {
        qWarning() << "QShadow: vkAllocateMemory (image) failed" << res << req.size;
        m_d->vkDestroyImage(m_win->device(), out->image, nullptr);
        out->image = VK_NULL_HANDLE;
        return;
    }
    m_d->vkBindImageMemory(m_win->device(), out->image, out->memory, 0);

    VkImageViewCreateInfo vinfo{};
    vinfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vinfo.image = out->image;
    vinfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vinfo.format = format;
    vinfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vinfo.subresourceRange.levelCount = 1;
    vinfo.subresourceRange.layerCount = 1;
    res = m_d->vkCreateImageView(m_win->device(), &vinfo, nullptr, &out->view);
    if (res != VK_SUCCESS)
        qWarning() << "QShadow: vkCreateImageView failed" << res << size;
}

void VulkanRenderer::destroyImage(ImageObj &img)
{
    VkDevice dev = m_win->device();
    if (img.view)
        m_d->vkDestroyImageView(dev, img.view, nullptr);
    if (img.image)
        m_d->vkDestroyImage(dev, img.image, nullptr);
    if (img.memory)
        m_d->vkFreeMemory(dev, img.memory, nullptr);
    img = ImageObj{};
}

void VulkanRenderer::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                  VkMemoryPropertyFlags props, VkBuffer *buffer,
                                  VkDeviceMemory *memory, void **mapped)
{
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkResult res = m_d->vkCreateBuffer(m_win->device(), &info, nullptr, buffer);
    if (res != VK_SUCCESS) {
        qWarning() << "QShadow: vkCreateBuffer failed" << res << size;
        return;
    }

    VkMemoryRequirements req{};
    m_d->vkGetBufferMemoryRequirements(m_win->device(), *buffer, &req);
    const std::uint32_t type =
        findMemoryType(req.memoryTypeBits, props, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type == UINT32_MAX) {
        qWarning() << "QShadow: no suitable memory type for buffer" << size;
        m_d->vkDestroyBuffer(m_win->device(), *buffer, nullptr);
        *buffer = VK_NULL_HANDLE;
        return;
    }

    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = type;
    res = m_d->vkAllocateMemory(m_win->device(), &alloc, nullptr, memory);
    if (res != VK_SUCCESS) {
        qWarning() << "QShadow: vkAllocateMemory (buffer) failed" << res << req.size;
        m_d->vkDestroyBuffer(m_win->device(), *buffer, nullptr);
        *buffer = VK_NULL_HANDLE;
        return;
    }
    m_d->vkBindBufferMemory(m_win->device(), *buffer, *memory, 0);
    if (m_d->vkMapMemory(m_win->device(), *memory, 0, size, 0, mapped) != VK_SUCCESS)
        qWarning() << "QShadow: vkMapMemory failed" << size;
}

void VulkanRenderer::flushRange(VkDeviceMemory memory, VkDeviceSize size)
{
    VkMappedMemoryRange range{};
    range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    range.memory = memory;
    range.offset = 0;
    range.size = size;
    m_d->vkFlushMappedMemoryRanges(m_win->device(), 1, &range);
}

void VulkanRenderer::destroySwapSlot(PerSwap &slot)
{
    VkDevice dev = m_win->device();
    for (int b = 0; b < 2; ++b) {
        if (slot.ubo[b]) {
            m_d->vkUnmapMemory(dev, slot.uboMem[b]);
            m_d->vkDestroyBuffer(dev, slot.ubo[b], nullptr);
            m_d->vkFreeMemory(dev, slot.uboMem[b], nullptr);
        }
    }
    if (slot.staging) {
        m_d->vkUnmapMemory(dev, slot.stagingMem);
        m_d->vkDestroyBuffer(dev, slot.staging, nullptr);
        m_d->vkFreeMemory(dev, slot.stagingMem, nullptr);
    }
    slot = PerSwap{};
}

void VulkanRenderer::waitIdle()
{
    m_d->vkQueueWaitIdle(m_win->graphicsQueue());
}

void VulkanRenderer::transition(VkCommandBuffer cb, VkImage img, VkImageLayout oldLayout,
                                VkImageLayout newLayout, VkAccessFlags srcAccess,
                                VkAccessFlags dstAccess, VkPipelineStageFlags srcStage,
                                VkPipelineStageFlags dstStage)
{
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = img;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    m_d->vkCmdPipelineBarrier(cb, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

// ---------------------------------------------------------------------------
// Pipeline / layout creation
// ---------------------------------------------------------------------------

VkPipeline VulkanRenderer::createGfxPipeline(const char *fragResource, VkRenderPass renderPass,
                                             bool hasDepth)
{
    VkShaderModule vs = createShaderModule(":/shaders/fullscreen.vert.spv");
    VkShaderModule fs = createShaderModule(fragResource);
    if (!vs || !fs) {
        qWarning() << "QShadow: missing shader module for" << fragResource;
        if (vs)
            m_d->vkDestroyShaderModule(m_win->device(), vs, nullptr);
        if (fs)
            m_d->vkDestroyShaderModule(m_win->device(), fs, nullptr);
        return VK_NULL_HANDLE;
    }

    const VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
         VK_SHADER_STAGE_VERTEX_BIT, vs, "main", nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
         VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", nullptr},
    };

    const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dynamicStates;

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    ia.primitiveRestartEnable = VK_FALSE;

    VkPipelineViewportStateCreateInfo vp{};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_FALSE;
    ds.depthWriteEnable = VK_FALSE;
    ds.depthCompareOp = VK_COMPARE_OP_ALWAYS;
    ds.maxDepthBounds = 1.0f;

    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &blend;

    VkGraphicsPipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vi;
    info.pInputAssemblyState = &ia;
    info.pViewportState = &vp;
    info.pRasterizationState = &rs;
    info.pMultisampleState = &ms;
    info.pDepthStencilState = hasDepth ? &ds : nullptr;
    info.pColorBlendState = &cb;
    info.pDynamicState = &dyn;
    info.layout = m_gfxPipeLayout;
    info.renderPass = renderPass;
    info.subpass = 0;

    VkPipeline pipe = VK_NULL_HANDLE;
    const VkResult res =
        m_d->vkCreateGraphicsPipelines(m_win->device(), VK_NULL_HANDLE, 1, &info, nullptr, &pipe);
    if (res != VK_SUCCESS)
        qWarning() << "QShadow: vkCreateGraphicsPipelines failed" << res << "for" << fragResource;

    m_d->vkDestroyShaderModule(m_win->device(), fs, nullptr);
    m_d->vkDestroyShaderModule(m_win->device(), vs, nullptr);
    return pipe;
}

VkPipeline VulkanRenderer::createComputePipeline(const char *compResource, VkPipelineLayout layout)
{
    VkShaderModule cs = createShaderModule(compResource);
    if (!cs)
        return VK_NULL_HANDLE;

    VkPipelineShaderStageCreateInfo stage{};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = cs;
    stage.pName = "main";

    VkComputePipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    info.stage = stage;
    info.layout = layout ? layout : m_compPipeLayout;

    VkPipeline pipe = VK_NULL_HANDLE;
    const VkResult res =
        m_d->vkCreateComputePipelines(m_win->device(), VK_NULL_HANDLE, 1, &info, nullptr, &pipe);
    if (res != VK_SUCCESS)
        qWarning() << "QShadow: vkCreateComputePipelines failed" << res << "for" << compResource;
    m_d->vkDestroyShaderModule(m_win->device(), cs, nullptr);
    return pipe;
}

void VulkanRenderer::destroyPipeline(VkPipeline &pipe)
{
    if (pipe) {
        m_d->vkDestroyPipeline(m_win->device(), pipe, nullptr);
        pipe = VK_NULL_HANDLE;
    }
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void VulkanRenderer::initResources()
{
    m_d = m_win->vulkanInstance()->deviceFunctions(m_win->device());
    QVulkanFunctions *f = m_win->vulkanInstance()->functions();
    f->vkGetPhysicalDeviceMemoryProperties(m_win->physicalDevice(), &m_memProps);

    // Samplers.
    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = VK_FILTER_NEAREST;
    si.minFilter = VK_FILTER_NEAREST;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 0.0f;
    m_d->vkCreateSampler(m_win->device(), &si, nullptr, &m_samplerNearest);

    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    m_d->vkCreateSampler(m_win->device(), &si, nullptr, &m_samplerLinear);

    // Descriptor set layouts.
    const VkDescriptorSetLayoutBinding texBindings[] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
    };
    VkDescriptorSetLayoutCreateInfo lci{};
    lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    lci.bindingCount = 2;
    lci.pBindings = texBindings;
    m_d->vkCreateDescriptorSetLayout(m_win->device(), &lci, nullptr, &m_texLayout);

    const VkDescriptorSetLayoutBinding compBindings[] = {
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    };
    lci.bindingCount = 3;
    lci.pBindings = compBindings;
    m_d->vkCreateDescriptorSetLayout(m_win->device(), &lci, nullptr, &m_compLayout);

    const VkPushConstantRange pushRange = {
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushParams)};

    VkPipelineLayoutCreateInfo pl{};
    pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &m_texLayout;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pushRange;
    m_d->vkCreatePipelineLayout(m_win->device(), &pl, nullptr, &m_gfxPipeLayout);

    VkPipelineLayoutCreateInfo cl{};
    cl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    cl.setLayoutCount = 1;
    cl.pSetLayouts = &m_compLayout;
    m_d->vkCreatePipelineLayout(m_win->device(), &cl, nullptr, &m_compPipeLayout);

    // Offscreen render pass (native-grid downscale pass target).
    VkAttachmentDescription att{};
    att.format = kFrameFormat;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &colorRef;

    // m_down is shared by every in-flight frame: wait for the previous frame's
    // shader reads of it before this pass overwrites it (write-after-read).
    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = 0;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rp{};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp.attachmentCount = 1;
    rp.pAttachments = &att;
    rp.subpassCount = 1;
    rp.pSubpasses = &sub;
    rp.dependencyCount = 1;
    rp.pDependencies = &dep;
    m_d->vkCreateRenderPass(m_win->device(), &rp, nullptr, &m_offscreenPass);

    m_pipeDown = createGfxPipeline(":/shaders/passthrough.frag.spv", m_offscreenPass, false);
    m_pipeEasu = createComputePipeline(":/shaders/fsr_easu.comp.spv");
    m_pipeRcas = createComputePipeline(":/shaders/fsr_rcas.comp.spv");

    // GPU YUV -> RGBA: needs rgba8 storage images and sampled/uploadable R8
    // planes (rgba8 storage is mandatory in Vulkan; checked anyway).
    {
        VkFormatProperties rgba{};
        VkFormatProperties r8{};
        f->vkGetPhysicalDeviceFormatProperties(m_win->physicalDevice(), kFrameFormat, &rgba);
        f->vkGetPhysicalDeviceFormatProperties(m_win->physicalDevice(), kPlaneFormat, &r8);
        const bool formatsOk =
            (rgba.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)
            && (r8.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)
            && (r8.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)
            && (r8.optimalTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_DST_BIT);

        const VkDescriptorSetLayoutBinding yuvBindings[] = {
            {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        };
        VkDescriptorSetLayoutCreateInfo yli{};
        yli.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        yli.bindingCount = 4;
        yli.pBindings = yuvBindings;
        m_d->vkCreateDescriptorSetLayout(m_win->device(), &yli, nullptr, &m_yuvSetLayout);

        VkPipelineLayoutCreateInfo ypl{};
        ypl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        ypl.setLayoutCount = 1;
        ypl.pSetLayouts = &m_yuvSetLayout;
        m_d->vkCreatePipelineLayout(m_win->device(), &ypl, nullptr, &m_yuvPipeLayout);

        m_pipeYuv = createComputePipeline(":/shaders/yuv2rgb.comp.spv", m_yuvPipeLayout);
        m_pipeYuyv = createComputePipeline(":/shaders/yuyv2rgb.comp.spv", m_yuvPipeLayout);
        m_yuvOk = formatsOk && m_pipeYuv != VK_NULL_HANDLE && m_pipeYuyv != VK_NULL_HANDLE;
        if (!m_yuvOk)
            qWarning() << "QShadow: GPU YUV conversion unavailable; converting on the CPU";
        m_win->setGpuYuvSupported(m_yuvOk);
    }

    // hq4x lookup table.
    QImage lutImg(QStringLiteral(":/shaders/hq4x.png"));
    if (!lutImg.isNull()) {
        const QImage rt = lutImg.convertToFormat(QImage::Format_RGBA8888);
        uploadImageBlocking(rt, kFrameFormat,
                            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, &m_lut);
    }
}

void VulkanRenderer::initSwapChainResources()
{
    waitIdle();
    const int n = m_win->concurrentFrameCount();
    m_perSwap.resize(std::size_t(n));

    VkDescriptorPoolSize poolSizes[] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, std::uint32_t(9u * n)},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, std::uint32_t(2u * n)},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, std::uint32_t(3u * n)},
    };
    VkDescriptorPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool.maxSets = std::uint32_t(5u * n);
    pool.poolSizeCount = 3;
    pool.pPoolSizes = poolSizes;
    m_d->vkCreateDescriptorPool(m_win->device(), &pool, nullptr, &m_pool);

    for (int i = 0; i < n; ++i) {
        PerSwap &slot = m_perSwap[std::size_t(i)];
        for (int b = 0; b < 2; ++b)
            createBuffer(kUboSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &slot.ubo[b], &slot.uboMem[b],
                         &slot.uboMapped[b]);

        VkDescriptorSetLayout layouts[5] = {m_texLayout, m_texLayout, m_compLayout, m_compLayout,
                                            m_yuvSetLayout};
        VkDescriptorSet sets[5] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE,
                                   VK_NULL_HANDLE, VK_NULL_HANDLE};
        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = m_pool;
        ai.descriptorSetCount = 5;
        ai.pSetLayouts = layouts;
        if (m_d->vkAllocateDescriptorSets(m_win->device(), &ai, sets) == VK_SUCCESS) {
            slot.gfxSet = sets[0];
            slot.downSet = sets[1];
            slot.compSets[0] = sets[2];
            slot.compSets[1] = sets[3];
            slot.yuvSet = sets[4];
        }
    }

    const bool hasDepth = m_win->depthStencilFormat() != VK_FORMAT_UNDEFINED;
    for (int i = 0; i < 5; ++i)
        m_pipeSwap[std::size_t(i)] =
            createGfxPipeline(programResource(Program(i)), m_win->defaultRenderPass(), hasDepth);
}

void VulkanRenderer::releaseSwapChainResources()
{
    waitIdle();
    for (auto &slot : m_perSwap)
        destroySwapSlot(slot);
    m_perSwap.clear();

    for (auto &pipe : m_pipeSwap)
        destroyPipeline(pipe);

    if (m_pool) {
        m_d->vkDestroyDescriptorPool(m_win->device(), m_pool, nullptr);
        m_pool = VK_NULL_HANDLE;
    }
}

void VulkanRenderer::releaseResources()
{
    waitIdle();

    m_win->setGpuYuvSupported(false);
    destroyPipeline(m_pipeYuyv);
    destroyPipeline(m_pipeYuv);
    destroyPipeline(m_pipeRcas);
    destroyPipeline(m_pipeEasu);
    destroyPipeline(m_pipeDown);

    if (m_downFbo) {
        m_d->vkDestroyFramebuffer(m_win->device(), m_downFbo, nullptr);
        m_downFbo = VK_NULL_HANDLE;
    }
    if (m_offscreenPass) {
        m_d->vkDestroyRenderPass(m_win->device(), m_offscreenPass, nullptr);
        m_offscreenPass = VK_NULL_HANDLE;
    }

    for (ImageObj &plane : m_yuv)
        destroyImage(plane);
    destroyImage(m_yuyv);
    destroyImage(m_fsrB);
    destroyImage(m_fsrA);
    destroyImage(m_down);
    destroyImage(m_frame);
    destroyImage(m_lut);

    if (m_yuvPipeLayout)
        m_d->vkDestroyPipelineLayout(m_win->device(), m_yuvPipeLayout, nullptr);
    if (m_yuvSetLayout)
        m_d->vkDestroyDescriptorSetLayout(m_win->device(), m_yuvSetLayout, nullptr);
    if (m_compPipeLayout)
        m_d->vkDestroyPipelineLayout(m_win->device(), m_compPipeLayout, nullptr);
    if (m_gfxPipeLayout)
        m_d->vkDestroyPipelineLayout(m_win->device(), m_gfxPipeLayout, nullptr);
    if (m_compLayout)
        m_d->vkDestroyDescriptorSetLayout(m_win->device(), m_compLayout, nullptr);
    if (m_texLayout)
        m_d->vkDestroyDescriptorSetLayout(m_win->device(), m_texLayout, nullptr);
    if (m_samplerLinear)
        m_d->vkDestroySampler(m_win->device(), m_samplerLinear, nullptr);
    if (m_samplerNearest)
        m_d->vkDestroySampler(m_win->device(), m_samplerNearest, nullptr);

    m_downFbo = VK_NULL_HANDLE;
    m_yuvPipeLayout = VK_NULL_HANDLE;
    m_yuvSetLayout = VK_NULL_HANDLE;
    m_yuvOk = false;
    for (QSize &s : m_yuvSize)
        s = QSize();
    m_yuyvSize = QSize();
    m_frameSize = m_downSize = m_fsrSize = QSize();
    m_frameLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    m_d = nullptr;
}

// ---------------------------------------------------------------------------
// Size/managed resources
// ---------------------------------------------------------------------------

void VulkanRenderer::ensureFrameResources(const QSize &size)
{
    if (m_frame.image && m_frameSize == size)
        return;
    if (m_frame.image) {
        waitIdle();
        destroyImage(m_frame);
    }
    m_frameSize = size;
    createImage(kFrameFormat, size, kFrameUsage, VK_SAMPLE_COUNT_1_BIT, &m_frame);
    m_frameLayout = VK_IMAGE_LAYOUT_UNDEFINED;
}

void VulkanRenderer::ensureDownResources(const QSize &size)
{
    if (m_down.image && m_downSize == size && m_downFbo)
        return;
    if (m_down.image) {
        waitIdle();
        destroyImage(m_down);
        if (m_downFbo) {
            m_d->vkDestroyFramebuffer(m_win->device(), m_downFbo, nullptr);
            m_downFbo = VK_NULL_HANDLE;
        }
    }
    m_downSize = size;
    createImage(kFrameFormat, size, kDownUsage, VK_SAMPLE_COUNT_1_BIT, &m_down);

    VkFramebufferCreateInfo fi{};
    fi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fi.renderPass = m_offscreenPass;
    fi.attachmentCount = 1;
    fi.pAttachments = &m_down.view;
    fi.width = std::uint32_t(size.width());
    fi.height = std::uint32_t(size.height());
    fi.layers = 1;
    m_d->vkCreateFramebuffer(m_win->device(), &fi, nullptr, &m_downFbo);
}

void VulkanRenderer::ensureFsrResources(const QSize &size)
{
    if (m_fsrA.image && m_fsrSize == size)
        return;
    if (m_fsrA.image) {
        waitIdle();
        destroyImage(m_fsrA);
        destroyImage(m_fsrB);
    }
    m_fsrSize = size;
    createImage(kFsrFormat, size, kFsrUsage, VK_SAMPLE_COUNT_1_BIT, &m_fsrA);
    createImage(kFsrFormat, size, kFsrUsage, VK_SAMPLE_COUNT_1_BIT, &m_fsrB);
}

void VulkanRenderer::ensureStaging(VkDeviceSize needed, PerSwap *slot)
{
    if (slot->staging && slot->stagingSize >= needed)
        return;
    if (slot->staging) {
        waitIdle();
        VkDevice dev = m_win->device();
        m_d->vkUnmapMemory(dev, slot->stagingMem);
        m_d->vkDestroyBuffer(dev, slot->staging, nullptr);
        m_d->vkFreeMemory(dev, slot->stagingMem, nullptr);
        slot->staging = VK_NULL_HANDLE;
    }
    createBuffer(needed, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &slot->staging, &slot->stagingMem,
                 &slot->stagingMapped);
    slot->stagingSize = slot->staging ? needed : 0;
}

void VulkanRenderer::uploadImageBlocking(const QImage &img, VkFormat format,
                                         VkImageUsageFlags usage, ImageObj *out)
{
    if (img.isNull())
        return;
    createImage(format, img.size(), usage, VK_SAMPLE_COUNT_1_BIT, out);
    if (!out->image)
        return;

    const VkDeviceSize bytes =
        VkDeviceSize(img.width()) * VkDeviceSize(img.height()) * 4;
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stagingMem = VK_NULL_HANDLE;
    void *mapped = nullptr;
    createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &staging, &stagingMem, &mapped);
    if (!staging) {
        destroyImage(*out);
        return;
    }
    std::memcpy(mapped, img.constBits(), std::size_t(bytes));

    const VkDevice dev = m_win->device();
    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = m_win->graphicsCommandPool();
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    m_d->vkAllocateCommandBuffers(dev, &ai, &cb);

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    m_d->vkBeginCommandBuffer(cb, &bi);

    transition(cb, out->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
               VK_PIPELINE_STAGE_TRANSFER_BIT);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {std::uint32_t(img.width()), std::uint32_t(img.height()), 1};
    region.bufferRowLength = std::uint32_t(img.width());
    m_d->vkCmdCopyBufferToImage(cb, staging, out->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                &region);

    transition(cb, out->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
               VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    m_d->vkEndCommandBuffer(cb);

    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    m_d->vkCreateFence(dev, &fci, nullptr, &fence);

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    m_d->vkQueueSubmit(m_win->graphicsQueue(), 1, &si, fence);
    m_d->vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
    m_d->vkDestroyFence(dev, fence, nullptr);
    m_d->vkFreeCommandBuffers(dev, m_win->graphicsCommandPool(), 1, &cb);
    m_d->vkUnmapMemory(dev, stagingMem);
    m_d->vkDestroyBuffer(dev, staging, nullptr);
    m_d->vkFreeMemory(dev, stagingMem, nullptr);
}

// ---------------------------------------------------------------------------
// Frame helpers
// ---------------------------------------------------------------------------

void VulkanRenderer::uploadFrame(const QImage &img, PerSwap *slot, VkCommandBuffer cb)
{
    const QSize size = img.size();
    if (size.isEmpty())
        return;
    ensureFrameResources(size);

    const VkDeviceSize bytes = VkDeviceSize(size.width()) * VkDeviceSize(size.height()) * 4;
    ensureStaging(bytes, slot);
    if (!slot->stagingMapped)
        return;

    std::memcpy(slot->stagingMapped, img.constBits(), std::size_t(bytes));
    flushRange(slot->stagingMem, bytes);

    // m_frame is shared by every in-flight frame: the previous frame may still
    // be sampling it, so the source stage must cover its shader reads.
    transition(cb, m_frame.image, m_frameLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
               VK_ACCESS_TRANSFER_WRITE_BIT, kSharedImageReaders | VK_PIPELINE_STAGE_TRANSFER_BIT,
               VK_PIPELINE_STAGE_TRANSFER_BIT);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {std::uint32_t(size.width()), std::uint32_t(size.height()), 1};
    region.bufferRowLength = std::uint32_t(size.width());
    m_d->vkCmdCopyBufferToImage(cb, slot->staging, m_frame.image,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    transition(cb, m_frame.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
               VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    m_frameLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

void VulkanRenderer::ensureYuvResources(const VideoFrame &frame)
{
    for (int i = 0; i < 3; ++i) {
        const QSize size(frame.planes[i].width, frame.planes[i].height);
        if (m_yuv[i].image && m_yuvSize[i] == size)
            continue;
        if (m_yuv[i].image) {
            waitIdle();
            destroyImage(m_yuv[i]);
        }
        m_yuvSize[i] = size;
        createImage(kPlaneFormat, size, kPlaneUsage, VK_SAMPLE_COUNT_1_BIT, &m_yuv[i]);
    }
}

void VulkanRenderer::uploadYuvFrame(const VideoFrame &frame, PerSwap *slot, VkCommandBuffer cb)
{
    const QSize size = frame.size();
    if (size.isEmpty())
        return;
    ensureFrameResources(size);
    ensureYuvResources(frame);
    for (const ImageObj &plane : m_yuv) {
        if (!plane.image)
            return;
    }

    // All three planes go through one staging buffer, each at a 16-byte aligned
    // offset; rows keep the decoder's padded stride (bufferRowLength).
    VkDeviceSize offset[3];
    VkDeviceSize total = 0;
    for (int i = 0; i < 3; ++i) {
        offset[i] = total;
        const VkDeviceSize bytes = VkDeviceSize(frame.planes[i].stride) * VkDeviceSize(frame.planes[i].height);
        total += (bytes + 15) & ~VkDeviceSize(15);
    }
    ensureStaging(total, slot);
    if (!slot->stagingMapped)
        return;
    for (int i = 0; i < 3; ++i) {
        const VideoFrame::Plane &p = frame.planes[i];
        std::memcpy(static_cast<char *>(slot->stagingMapped) + offset[i], p.data.constData(),
                    std::size_t(p.stride) * std::size_t(p.height));
    }
    flushRange(slot->stagingMem, VK_WHOLE_SIZE);

    // The plane images and m_frame are shared by every in-flight frame, so the
    // previous frame's shader reads must finish before they are overwritten.
    // Contents are fully replaced each time, hence UNDEFINED as the old layout.
    for (int i = 0; i < 3; ++i) {
        transition(cb, m_yuv[i].image, VK_IMAGE_LAYOUT_UNDEFINED,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                   kSharedImageReaders | VK_PIPELINE_STAGE_TRANSFER_BIT,
                   VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy region{};
        region.bufferOffset = offset[i];
        region.bufferRowLength = std::uint32_t(frame.planes[i].stride);
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {std::uint32_t(frame.planes[i].width),
                              std::uint32_t(frame.planes[i].height), 1};
        m_d->vkCmdCopyBufferToImage(cb, slot->staging, m_yuv[i].image,
                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        transition(cb, m_yuv[i].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                   VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    }

    VkDescriptorImageInfo planeInfo[3]{};
    for (int i = 0; i < 3; ++i) {
        // Y is fetched texel-exact; the subsampled chroma planes are filtered.
        planeInfo[i].sampler = i == 0 ? m_samplerNearest : m_samplerLinear;
        planeInfo[i].imageView = m_yuv[i].view;
        planeInfo[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    VkWriteDescriptorSet writes[3]{};
    for (int i = 0; i < 3; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = slot->yuvSet;
        writes[i].dstBinding = std::uint32_t(i);
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &planeInfo[i];
    }
    m_d->vkUpdateDescriptorSets(m_win->device(), 3, writes, 0, nullptr);

    convertToFrame(cb, slot, m_pipeYuv, size);
}

void VulkanRenderer::uploadYuyvFrame(const VideoFrame &frame, PerSwap *slot, VkCommandBuffer cb)
{
    const QSize size = frame.size();
    const VideoFrame::Plane &p = frame.planes[0];
    if (size.isEmpty() || (size.width() & 1) || (p.stride & 3))
        return;
    ensureFrameResources(size);

    // One RGBA8 texel holds two pixels (Y0 Cb Y1 Cr).
    const QSize texels(size.width() / 2, size.height());
    if (!m_yuyv.image || m_yuyvSize != texels) {
        if (m_yuyv.image) {
            waitIdle();
            destroyImage(m_yuyv);
        }
        m_yuyvSize = texels;
        createImage(kFrameFormat, texels, kPlaneUsage, VK_SAMPLE_COUNT_1_BIT, &m_yuyv);
    }
    if (!m_yuyv.image)
        return;

    const VkDeviceSize bytes = VkDeviceSize(p.stride) * VkDeviceSize(size.height());
    ensureStaging(bytes, slot);
    if (!slot->stagingMapped)
        return;
    std::memcpy(slot->stagingMapped, p.data.constData(), std::size_t(bytes));
    flushRange(slot->stagingMem, VK_WHOLE_SIZE);

    // Shared across in-flight frames, like the planar images: see uploadYuvFrame.
    transition(cb, m_yuyv.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               0, VK_ACCESS_TRANSFER_WRITE_BIT,
               kSharedImageReaders | VK_PIPELINE_STAGE_TRANSFER_BIT,
               VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy region{};
    region.bufferRowLength = std::uint32_t(p.stride / 4); // in texels
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {std::uint32_t(texels.width()), std::uint32_t(texels.height()), 1};
    m_d->vkCmdCopyBufferToImage(cb, slot->staging, m_yuyv.image,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    transition(cb, m_yuyv.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
               VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    VkDescriptorImageInfo info{};
    info.sampler = m_samplerNearest;
    info.imageView = m_yuyv.view;
    info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = slot->yuvSet;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &info;
    m_d->vkUpdateDescriptorSets(m_win->device(), 1, &write, 0, nullptr);

    convertToFrame(cb, slot, m_pipeYuyv, size);
}

void VulkanRenderer::convertToFrame(VkCommandBuffer cb, PerSwap *slot, VkPipeline pipe,
                                    const QSize &size)
{
    // m_frame is shared by every in-flight frame: wait for the previous frame's
    // shader reads before overwriting. Contents are fully replaced, so UNDEFINED.
    transition(cb, m_frame.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0,
               VK_ACCESS_SHADER_WRITE_BIT, kSharedImageReaders | VK_PIPELINE_STAGE_TRANSFER_BIT,
               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    VkDescriptorImageInfo outInfo{};
    outInfo.imageView = m_frame.view;
    outInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = slot->yuvSet;
    write.dstBinding = 3;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    write.pImageInfo = &outInfo;
    m_d->vkUpdateDescriptorSets(m_win->device(), 1, &write, 0, nullptr);

    m_d->vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    m_d->vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, m_yuvPipeLayout, 0, 1,
                                 &slot->yuvSet, 0, nullptr);
    m_d->vkCmdDispatch(cb, (std::uint32_t(size.width()) + 7) / 8,
                       (std::uint32_t(size.height()) + 7) / 8, 1);

    transition(cb, m_frame.image, VK_IMAGE_LAYOUT_GENERAL,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT,
               VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    m_frameLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

void VulkanRenderer::updateGfxDescriptors(VkDescriptorSet set, VkImageView srcView,
                                          VkSampler srcSampler)
{
    VkDescriptorImageInfo srcInfo{};
    srcInfo.sampler = srcSampler;
    srcInfo.imageView = srcView;
    srcInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkDescriptorImageInfo lutInfo{};
    lutInfo.sampler = m_samplerLinear;
    lutInfo.imageView = m_lut.view;
    lutInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet writes[2]{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = set;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].pImageInfo = &srcInfo;

    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = set;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].pImageInfo = &lutInfo;

    m_d->vkUpdateDescriptorSets(m_win->device(), 2, writes, 0, nullptr);
}

void VulkanRenderer::updateCompDescriptors(VkDescriptorSet compSet, VkBuffer ubo,
                                           VkImageView inputView, VkImageView outputView)
{
    VkDescriptorBufferInfo uboInfo{};
    uboInfo.buffer = ubo;
    uboInfo.offset = 0;
    uboInfo.range = 64;

    VkDescriptorImageInfo inputInfo{};
    inputInfo.sampler = m_samplerLinear;
    inputInfo.imageView = inputView;
    inputInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkDescriptorImageInfo outputInfo{};
    outputInfo.imageView = outputView;
    outputInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet writes[3]{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = compSet;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[0].pBufferInfo = &uboInfo;

    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = compSet;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].pImageInfo = &inputInfo;

    writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet = compSet;
    writes[2].dstBinding = 2;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[2].pImageInfo = &outputInfo;

    m_d->vkUpdateDescriptorSets(m_win->device(), 3, writes, 0, nullptr);
}

void VulkanRenderer::writeEasuConsts(PerSwap *slot, const QSize &src, const QSize &out)
{
    const fsr::EasuConsts c =
        fsr::easuCon(float(src.width()), float(src.height()), float(src.width()),
                     float(src.height()), float(out.width()), float(out.height()));
    std::memcpy(slot->uboMapped[0], &c, sizeof(fsr::EasuConsts));
    flushRange(slot->uboMem[0], sizeof(fsr::EasuConsts));
}

void VulkanRenderer::writeRcasConsts(PerSwap *slot, float sharpness)
{
    std::uint32_t buf[16] = {};
    buf[0] = fsr::rcasCon(sharpness);
    std::memcpy(slot->uboMapped[1], buf, sizeof(buf));
    flushRange(slot->uboMem[1], sizeof(buf));
}

void VulkanRenderer::setViewportRect(VkCommandBuffer cb, const QRect &rect)
{
    VkViewport vp{};
    vp.x = float(rect.x());
    vp.y = float(rect.y());
    vp.width = float(rect.width());
    vp.height = float(rect.height());
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    m_d->vkCmdSetViewport(cb, 0, 1, &vp);

    VkRect2D sc{};
    sc.offset.x = std::max(0, rect.x());
    sc.offset.y = std::max(0, rect.y());
    sc.extent.width = std::uint32_t(std::max(0, rect.width()));
    sc.extent.height = std::uint32_t(std::max(0, rect.height()));
    m_d->vkCmdSetScissor(cb, 0, 1, &sc);
}

// ---------------------------------------------------------------------------
// Passes
// ---------------------------------------------------------------------------

void VulkanRenderer::recordDownscale(VkCommandBuffer cb, PerSwap *slot, const QSize &target)
{
    VkClearValue clear{};
    VkRenderPassBeginInfo rp{};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass = m_offscreenPass;
    rp.framebuffer = m_downFbo;
    rp.renderArea.extent.width = std::uint32_t(target.width());
    rp.renderArea.extent.height = std::uint32_t(target.height());
    rp.clearValueCount = 1;
    rp.pClearValues = &clear;
    m_d->vkCmdBeginRenderPass(cb, &rp, VK_SUBPASS_CONTENTS_INLINE);

    setViewportRect(cb, QRect(0, 0, target.width(), target.height()));
    m_d->vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeDown);
    m_d->vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, m_gfxPipeLayout, 0, 1,
                                 &slot->downSet, 0, nullptr);

    PushParams pp{};
    pp.uTexSize[0] = float(target.width());
    pp.uTexSize[1] = float(target.height());
    pp.uInSize[0] = float(target.width());
    pp.uInSize[1] = float(target.height());
    pp.uOutSize[0] = float(target.width());
    pp.uOutSize[1] = float(target.height());
    pp.uAspect[0] = float(target.width()) / float(std::max(1, target.height()));
    pp.uAspect[1] = 1.0f;
    pp.uModScale = float(target.width());
    m_d->vkCmdPushConstants(cb, m_gfxPipeLayout,
                            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                            sizeof(PushParams), &pp);

    m_d->vkCmdDraw(cb, 3, 1, 0, 0);
    m_d->vkCmdEndRenderPass(cb);

    // The offscreen pass has no external dependency, so order its writes before
    // any later compute/fragment sampling of m_down.
    transition(cb, m_down.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
               VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
}

void VulkanRenderer::recordEasu(VkCommandBuffer cb, VkDescriptorSet set, const QSize &out)
{
    m_d->vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeEasu);
    m_d->vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, m_compPipeLayout, 0, 1,
                                 &set, 0, nullptr);
    const std::uint32_t gx = (std::uint32_t(out.width()) + 15) / 16;
    const std::uint32_t gy = (std::uint32_t(out.height()) + 15) / 16;
    m_d->vkCmdDispatch(cb, gx, gy, 1);
}

void VulkanRenderer::recordRcas(VkCommandBuffer cb, VkDescriptorSet set, const QSize &out)
{
    m_d->vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeRcas);
    m_d->vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, m_compPipeLayout, 0, 1,
                                 &set, 0, nullptr);
    const std::uint32_t gx = (std::uint32_t(out.width()) + 15) / 16;
    const std::uint32_t gy = (std::uint32_t(out.height()) + 15) / 16;
    m_d->vkCmdDispatch(cb, gx, gy, 1);
}

void VulkanRenderer::recordBlit(VkCommandBuffer cb, PerSwap *slot, VkImageView srcView,
                                VkSampler sampler, const QRect &target, Program program,
                                const PushParams &pp)
{
    updateGfxDescriptors(slot->gfxSet, srcView, sampler);

    m_d->vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeSwap[std::size_t(program)]);
    m_d->vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, m_gfxPipeLayout, 0, 1,
                                 &slot->gfxSet, 0, nullptr);
    m_d->vkCmdPushConstants(cb, m_gfxPipeLayout,
                            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                            sizeof(PushParams), &pp);
    setViewportRect(cb, target);
    m_d->vkCmdDraw(cb, 3, 1, 0, 0);
}

void VulkanRenderer::beginSwapPass(VkCommandBuffer cb, const QSize &outSize)
{
    VkClearValue clears[2] = {};
    clears[1].depthStencil = {1.0f, 0};
    const bool hasDepth = m_win->depthStencilFormat() != VK_FORMAT_UNDEFINED;

    VkRenderPassBeginInfo rp{};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass = m_win->defaultRenderPass();
    rp.framebuffer = m_win->currentFramebuffer();
    rp.renderArea.extent.width = std::uint32_t(outSize.width());
    rp.renderArea.extent.height = std::uint32_t(outSize.height());
    rp.clearValueCount = hasDepth ? 2u : 1u;
    rp.pClearValues = clears;
    m_d->vkCmdBeginRenderPass(cb, &rp, VK_SUBPASS_CONTENTS_INLINE);
}

void VulkanRenderer::endSwapPass(VkCommandBuffer cb)
{
    m_d->vkCmdEndRenderPass(cb);
}

// ---------------------------------------------------------------------------
// Frame entry point
// ---------------------------------------------------------------------------

void VulkanRenderer::startNextFrame()
{
    bool drewPass = false;
    std::optional<std::chrono::steady_clock::time_point> shownStamp;
    recordFrame(&drewPass, &shownStamp);
    if (!drewPass)
        presentClearedFrame();

    // QVulkanWindow needs exactly one frameReady() per startNextFrame(). Skipping
    // it leaves the window waiting for this frame forever: it never asks for
    // another one and the picture freezes for good. So it is called here,
    // unconditionally, whatever recordFrame() did (including its early returns).
    m_win->frameReady();
    if (shownStamp)
        m_win->noteSubmitted(*shownStamp);
}

void VulkanRenderer::presentClearedFrame()
{
    const QSize outSize = m_win->swapChainImageSize();
    if (outSize.isEmpty())
        return; // no swapchain extent to clear; frameReady() must still follow
    VkCommandBuffer cb = m_win->currentCommandBuffer();
    beginSwapPass(cb, outSize);
    endSwapPass(cb);
}

void VulkanRenderer::recordFrame(bool *drewPass,
                                 std::optional<std::chrono::steady_clock::time_point> *shownStamp)
{
    if (!m_win->isValid() || m_perSwap.empty())
        return;

    const std::size_t idx = std::size_t(m_win->currentFrame());
    if (idx >= m_perSwap.size())
        return;
    PerSwap *slot = &m_perSwap[idx];
    VkCommandBuffer cb = m_win->currentCommandBuffer();

    VideoFramePtr frame;
    std::chrono::steady_clock::time_point stamp;
    const bool newFrame = m_win->takePendingFrame(&frame, &stamp);
    if (newFrame) {
        if (frame->kind == VideoFrame::Kind::Yuv) {
            if (m_yuvOk)
                uploadYuvFrame(*frame, slot, cb);
        } else if (frame->kind == VideoFrame::Kind::Yuyv) {
            if (m_yuvOk)
                uploadYuyvFrame(*frame, slot, cb);
        } else {
            uploadFrame(frame->rgba, slot, cb);
        }
    }

    const QSize outSize = m_win->swapChainImageSize();
    if (outSize.isEmpty())
        return;

    const bool haveFrame = m_frame.image && !m_frameSize.isEmpty();

    // Letterboxed target rect.
    const bool preserveAspect = m_win->m_state.preserveAspect.load();
    QSize targetSize = outSize;
    QRect target(0, 0, outSize.width(), outSize.height());
    if (haveFrame && preserveAspect) {
        const QSize scaled = m_frameSize.scaled(outSize, Qt::KeepAspectRatio);
        targetSize = scaled;
        target = QRect((outSize.width() - scaled.width()) / 2,
                       (outSize.height() - scaled.height()) / 2, scaled.width(),
                       scaled.height());
    }

    const int mode = m_win->m_state.shaderMode.load();
    const int downscale = m_win->m_state.downscale.load();

    if (haveFrame && !targetSize.isEmpty()) {
        // Optional pre-downscale to the native grid.
        VkImageView srcView = m_frame.view;
        QSize srcSize = m_frameSize;
        if (downscale > 1) {
            const QSize down(std::max(2, srcSize.width() / downscale),
                             std::max(2, srcSize.height() / downscale));
            if (down != srcSize) {
                ensureDownResources(down);
                updateGfxDescriptors(slot->downSet, m_frame.view, m_samplerNearest);
                recordDownscale(cb, slot, down);
                srcView = m_down.view;
                srcSize = down;
            }
        }

        // FSR upscale / sharpen.
        VkImageView finalView = srcView;
        QSize finalSize = srcSize;
        bool fsrActive = false;
        const bool sharpOnly = mode == int(VideoWidget::ShaderMode::FsrSharpenOnly);
        if (VideoWidget::isFsrMode(mode)) {
            const QSize fsrOut =
                sharpOnly ? srcSize : VideoWidget::fsrUpscaleSize(srcSize, targetSize, mode);
            const bool doUpscale =
                fsrOut.width() > srcSize.width() || fsrOut.height() > srcSize.height();
            if (sharpOnly || doUpscale) {
                ensureFsrResources(fsrOut);
                fsrActive = true;

                bool easuRan = false;
                if (fsrOut != srcSize) {
                    transition(cb, m_fsrA.image, VK_IMAGE_LAYOUT_UNDEFINED,
                               VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT,
                               kSharedImageReaders, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
                    updateCompDescriptors(slot->compSets[0], slot->ubo[0], srcView, m_fsrA.view);
                    writeEasuConsts(slot, srcSize, fsrOut);
                    recordEasu(cb, slot->compSets[0], fsrOut);
                    transition(cb, m_fsrA.image, VK_IMAGE_LAYOUT_GENERAL,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                               VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
                    easuRan = true;
                }

                VkImageView rcasIn = easuRan ? m_fsrA.view : srcView;
                const VkImage rcasOut = easuRan ? m_fsrB.image : m_fsrA.image;
                VkImageView rcasOutView = easuRan ? m_fsrB.view : m_fsrA.view;
                const QSize rcasOutSize = easuRan ? fsrOut : srcSize;

                transition(cb, rcasOut, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0,
                           VK_ACCESS_SHADER_WRITE_BIT, kSharedImageReaders,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
                updateCompDescriptors(slot->compSets[1], slot->ubo[1], rcasIn, rcasOutView);
                const float sharpness =
                    mode == int(VideoWidget::ShaderMode::FsrSharpenOnly) ? 0.5f : 1.0f;
                writeRcasConsts(slot, sharpness);
                recordRcas(cb, slot->compSets[1], rcasOutSize);
                transition(cb, rcasOut, VK_IMAGE_LAYOUT_GENERAL,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT,
                           VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

                finalView = rcasOutView;
                finalSize = rcasOutSize;
            }
        }

        const Program program = [mode]() {
            switch (mode) {
            case int(VideoWidget::ShaderMode::Hq4x):
                return Program::Hq4x;
            case int(VideoWidget::ShaderMode::Xbrz):
                return Program::Xbrz;
            case int(VideoWidget::ShaderMode::CrtGeom):
                return Program::CrtGeom;
            case int(VideoWidget::ShaderMode::CrtRoyale):
                return Program::CrtRoyale;
            default:
                return Program::Passthrough;
            }
        }();

        VkSampler blitSampler = m_samplerLinear;
        if (mode == int(VideoWidget::ShaderMode::None))
            blitSampler = m_samplerNearest;
        if (fsrActive && finalSize == targetSize)
            blitSampler = m_samplerNearest;

        // Shared program parameters.
        PushParams pp{};
        pp.uTexSize[0] = float(finalSize.width());
        pp.uTexSize[1] = float(finalSize.height());
        pp.uInSize[0] = float(srcSize.width());
        pp.uInSize[1] = float(srcSize.height());
        pp.uOutSize[0] = float(targetSize.width());
        pp.uOutSize[1] = float(targetSize.height());
        const float aspect = float(targetSize.width()) / float(std::max(1, targetSize.height()));
        pp.uAspect[0] = aspect;
        pp.uAspect[1] = 1.0f;
        pp.uModScale = float(targetSize.width());

        if (mode == int(VideoWidget::ShaderMode::CrtGeom)) {
            // Only changes with the output aspect ratio, so cache it.
            if (aspect != m_crtAspect) {
                crtGeomStretch(aspect, m_crtStretch);
                m_crtAspect = aspect;
            }
            std::copy(m_crtStretch, m_crtStretch + 3, pp.uStretch);
        }

        beginSwapPass(cb, outSize);
        recordBlit(cb, slot, finalView, blitSampler, target, program, pp);
        endSwapPass(cb);
    } else {
        // Nothing decoded yet: still present a cleared frame.
        beginSwapPass(cb, outSize);
        endSwapPass(cb);
    }

    *drewPass = true;
    if (newFrame)
        *shownStamp = stamp;
}

