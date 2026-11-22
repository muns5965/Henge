/*
 * Stage 1 pixel harness (no shaders, no AIR input).
 * Exercises Vulkan clear, clear-attachment-with-rect, and buffer->image copy
 * on a 64x64 B8G8R8A8_UNORM image, then reads back every pixel and compares
 * against expected values computed independently in this file.
 *
 * This validates the Vulkan driver path and this harness only.
 * It does NOT validate libapplegfx or pvg-vulkan draw paths.
 */
#include <vulkan/vulkan.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 64
#define H 64
#define BPP 4
#define SZ (W * H * BPP)

#define CHECK(x) do { VkResult _r = (x); if (_r != VK_SUCCESS) { \
    fprintf(stderr, "%s:%d VkResult=%d\n", __FILE__, __LINE__, (int)_r); exit(2); } } while (0)

typedef struct { VkInstance inst; VkPhysicalDevice pd; VkDevice dev; VkQueue q;
                 uint32_t qf; VkCommandPool pool; VkCommandBuffer cb; } Ctx;
typedef struct { VkBuffer buf; VkDeviceMemory mem; uint8_t *map; } Buf;

static Ctx c;
static VkImage img;
static VkImageLayout cur;      /* tracked layout of img */
static VkRenderPass rp;
static VkFramebuffer fb;
static Buf stage, rb;

static uint32_t find_mem(uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(c.pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    fprintf(stderr, "no suitable memory type\n"); exit(2);
}

static Buf make_buf(VkDeviceSize size, VkBufferUsageFlags usage) {
    Buf b;
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = size; bi.usage = usage; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    CHECK(vkCreateBuffer(c.dev, &bi, NULL, &b.buf));
    VkMemoryRequirements r; vkGetBufferMemoryRequirements(c.dev, b.buf, &r);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = r.size;
    ai.memoryTypeIndex = find_mem(r.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    CHECK(vkAllocateMemory(c.dev, &ai, NULL, &b.mem));
    CHECK(vkBindBufferMemory(c.dev, b.buf, b.mem, 0));
    void *p; CHECK(vkMapMemory(c.dev, b.mem, 0, size, 0, &p));
    b.map = (uint8_t *)p;
    return b;
}

static void begin_cb(void) {
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    CHECK(vkResetCommandBuffer(c.cb, 0));
    CHECK(vkBeginCommandBuffer(c.cb, &bi));
}

static void submit_cb(void) {
    CHECK(vkEndCommandBuffer(c.cb));
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1; si.pCommandBuffers = &c.cb;
    CHECK(vkQueueSubmit(c.q, 1, &si, VK_NULL_HANDLE));
    CHECK(vkQueueWaitIdle(c.q));
}

static void transition(VkImageLayout nl, VkAccessFlags sa, VkAccessFlags da,
                       VkPipelineStageFlags ss, VkPipelineStageFlags ds) {
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.oldLayout = cur; b.newLayout = nl;
    b.srcAccessMask = sa; b.dstAccessMask = da;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdPipelineBarrier(c.cb, ss, ds, 0, 0, NULL, 0, NULL, 1, &b);
    cur = nl;
}

/* Read the whole image into out (SZ bytes, B,G,R,A per pixel, row-major). */
static void readback(uint8_t *out) {
    begin_cb();
    transition(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
               VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
               VK_ACCESS_TRANSFER_READ_BIT,
               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
               VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy r; memset(&r, 0, sizeof r);
    r.imageSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    r.imageExtent = (VkExtent3D){ W, H, 1 };
    vkCmdCopyImageToBuffer(c.cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.buf, 1, &r);
    submit_cb();
    memcpy(out, rb.map, SZ);
}

static int compare(const char *name, const uint8_t *got, const uint8_t *want) {
    int bad = 0, first = -1;
    for (int i = 0; i < W * H; i++) {
        if (memcmp(got + i * BPP, want + i * BPP, BPP) != 0) {
            if (first < 0) first = i;
            bad++;
        }
    }
    if (bad == 0) {
        printf("%-28s PASS  (%d/%d pixels exact)\n", name, W * H, W * H);
        return 0;
    }
    int x = first % W, y = first / W;
    printf("%-28s FAIL  (%d mismatched pixels; first at x=%d y=%d got BGRA=%02x%02x%02x%02x want=%02x%02x%02x%02x)\n",
           name, bad, x, y,
           got[first*4+0], got[first*4+1], got[first*4+2], got[first*4+3],
           want[first*4+0], want[first*4+1], want[first*4+2], want[first*4+3]);
    return 1;
}

/* Expected values, computed here independently of any Vulkan driver. */
#define BG_B 0x80
#define BG_G 0xEE
#define BG_R 0x11
#define BG_A 0xFF

static void expect_background(uint8_t *w) {
    for (int i = 0; i < W * H; i++) {
        w[i*4+0] = BG_B; w[i*4+1] = BG_G; w[i*4+2] = BG_R; w[i*4+3] = BG_A;
    }
}

static void expect_pattern(uint8_t *w) {
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        uint8_t *p = w + (y * W + x) * BPP;
        p[0] = (uint8_t)((x * 3) & 255);       /* B */
        p[1] = (uint8_t)((y * 5) & 255);       /* G */
        p[2] = (uint8_t)(((x + y) * 7) & 255); /* R */
        p[3] = 255;                            /* A */
    }
}

int main(void) {
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "stage1_pixels"; app.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo = &app;
    CHECK(vkCreateInstance(&ici, NULL, &c.inst));

    uint32_t n = 0;
    CHECK(vkEnumeratePhysicalDevices(c.inst, &n, NULL));
    if (n == 0) { fprintf(stderr, "no Vulkan physical device\n"); return 2; }
    VkPhysicalDevice *pds = malloc(n * sizeof *pds);
    CHECK(vkEnumeratePhysicalDevices(c.inst, &n, pds));
    c.pd = pds[0]; free(pds);

    VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(c.pd, &props);
    printf("device: %s (type=%d)\n", props.deviceName, (int)props.deviceType);

    uint32_t qn = 0; vkGetPhysicalDeviceQueueFamilyProperties(c.pd, &qn, NULL);
    VkQueueFamilyProperties *qp = malloc(qn * sizeof *qp);
    vkGetPhysicalDeviceQueueFamilyProperties(c.pd, &qn, qp);
    c.qf = UINT32_MAX;
    for (uint32_t i = 0; i < qn; i++) if (qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { c.qf = i; break; }
    free(qp);
    if (c.qf == UINT32_MAX) { fprintf(stderr, "no graphics queue\n"); return 2; }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueFamilyIndex = c.qf; qci.queueCount = 1; qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    CHECK(vkCreateDevice(c.pd, &dci, NULL, &c.dev));
    vkGetDeviceQueue(c.dev, c.qf, 0, &c.q);

    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pci.queueFamilyIndex = c.qf;
    CHECK(vkCreateCommandPool(c.dev, &pci, NULL, &c.pool));
    VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cbai.commandPool = c.pool; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = 1;
    CHECK(vkAllocateCommandBuffers(c.dev, &cbai, &c.cb));

    VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ii.imageType = VK_IMAGE_TYPE_2D; ii.format = VK_FORMAT_B8G8R8A8_UNORM;
    ii.extent = (VkExtent3D){ W, H, 1 }; ii.mipLevels = 1; ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    CHECK(vkCreateImage(c.dev, &ii, NULL, &img));
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(c.dev, img, &mr);
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = find_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkDeviceMemory imem; CHECK(vkAllocateMemory(c.dev, &mai, NULL, &imem));
    CHECK(vkBindImageMemory(c.dev, img, imem, 0));
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = VK_FORMAT_B8G8R8A8_UNORM;
    vi.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkImageView view; CHECK(vkCreateImageView(c.dev, &vi, NULL, &view));

    VkAttachmentDescription ad; memset(&ad, 0, sizeof ad);
    ad.format = VK_FORMAT_B8G8R8A8_UNORM; ad.samples = VK_SAMPLE_COUNT_1_BIT;
    ad.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; ad.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ad.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    ad.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    ad.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ad.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference ar = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sp; memset(&sp, 0, sizeof sp);
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.colorAttachmentCount = 1; sp.pColorAttachments = &ar;
    VkRenderPassCreateInfo rpi = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    rpi.attachmentCount = 1; rpi.pAttachments = &ad; rpi.subpassCount = 1; rpi.pSubpasses = &sp;
    CHECK(vkCreateRenderPass(c.dev, &rpi, NULL, &rp));
    VkFramebufferCreateInfo fbi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    fbi.renderPass = rp; fbi.attachmentCount = 1; fbi.pAttachments = &view;
    fbi.width = W; fbi.height = H; fbi.layers = 1;
    CHECK(vkCreateFramebuffer(c.dev, &fbi, NULL, &fb));

    stage = make_buf(SZ, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    rb = make_buf(SZ, VK_BUFFER_USAGE_TRANSFER_DST_BIT);

    uint8_t *got = malloc(SZ), *want = malloc(SZ);
    int failures = 0;

    /* T1: full-image clear to a solid color. */
    cur = VK_IMAGE_LAYOUT_UNDEFINED;
    begin_cb();
    transition(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
               VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkClearColorValue bg; memset(&bg, 0, sizeof bg);
    bg.float32[0] = BG_R / 255.0f; bg.float32[1] = BG_G / 255.0f;
    bg.float32[2] = BG_B / 255.0f; bg.float32[3] = 1.0f;
    VkImageSubresourceRange full = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdClearColorImage(c.cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &bg, 1, &full);
    submit_cb();
    readback(got);
    expect_background(want);
    failures += compare("T1 full clear", got, want);

    /* T2: clear a rectangle (10,20)-(15,27) to red using a render pass, no shader. */
    begin_cb();
    transition(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
               VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    VkRenderPassBeginInfo rbi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rbi.renderPass = rp; rbi.framebuffer = fb;
    rbi.renderArea.offset = (VkOffset2D){ 0, 0 };
    rbi.renderArea.extent = (VkExtent2D){ W, H };
    vkCmdBeginRenderPass(c.cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    VkClearAttachment ca; memset(&ca, 0, sizeof ca);
    ca.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; ca.colorAttachment = 0;
    ca.clearValue.color.float32[0] = 1.0f; ca.clearValue.color.float32[3] = 1.0f;
    VkClearRect cr; memset(&cr, 0, sizeof cr);
    cr.rect.offset = (VkOffset2D){ 10, 20 }; cr.rect.extent = (VkExtent2D){ 5, 7 };
    cr.baseArrayLayer = 0; cr.layerCount = 1;
    vkCmdClearAttachments(c.cb, 1, &ca, 1, &cr);
    vkCmdEndRenderPass(c.cb);
    submit_cb();
    cur = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    readback(got);
    expect_background(want);
    for (int y = 20; y < 27; y++) for (int x = 10; x < 15; x++) {
        uint8_t *p = want + (y * W + x) * BPP;
        p[0] = 0x00; p[1] = 0x00; p[2] = 0xFF; p[3] = 0xFF;
    }
    failures += compare("T2 rect clear (render)", got, want);

    /* T3: buffer -> image copy of a per-pixel pattern, repeated 3 times. */
    expect_pattern(want);
    for (int run = 1; run <= 3; run++) {
        memcpy(stage.map, want, SZ);
        begin_cb();
        transition(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy r; memset(&r, 0, sizeof r);
        r.imageSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        r.imageExtent = (VkExtent3D){ W, H, 1 };
        vkCmdCopyBufferToImage(c.cb, stage.buf, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
        submit_cb();
        cur = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        readback(got);
        char name[64];
        snprintf(name, sizeof name, "T3 buffer->image run %d", run);
        failures += compare(name, got, want);
    }

    printf("RESULT: %s (%d failing test group(s))\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
