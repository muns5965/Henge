/*
 * Triangle draw harness for the translated SPIR-V (A4 pipeline, A5 pixels).
 *
 * Usage:
 *   draw_triangle <vertex.spv> <fragment.spv>
 *
 * The geometry and color come from examples/triangle/triangle.metal:
 *   vertices (NDC): (0, 0.75), (-0.75, -0.75), (0.75, -0.75)
 *   fragment color: opaque red (1, 0, 0, 1)
 * The clear color is chosen here (not taken from the shader).
 *
 * Expected pixels are computed here, independently of the translator:
 *   framebuffer x = (ndc_x + 1) / 2 * W,  y = (ndc_y + 1) / 2 * H
 *   pixel center (x + 0.5, y + 0.5) inside the triangle -> red, else clear.
 * Pixels whose center is within 1.0 px of a triangle edge are reported as a
 * boundary band and are NOT gated (rasterization rule not yet pinned down).
 *
 * Output: one line per step. Exit 0 only if A4 and A5 (gated pixels) pass.
 * Pipeline creation errors are reported with the raw VkResult and not masked.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define W 64
#define H 64
#define BPP 4
#define SZ (W * H * BPP)

static VkInstance inst;
static VkPhysicalDevice pd;
static VkDevice dev;
static VkQueue q;
static uint32_t qf;
static VkCommandPool pool;
static VkCommandBuffer cb;

static uint32_t find_mem(uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    fprintf(stderr, "no memory type\n"); exit(2);
}

static uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return NULL; }
    fclose(f);
    *len = (size_t)n;
    return b;
}

static void edge_dist(double px, double py, double ax, double ay, double bx, double by,
                      double *cross_out, double *dist_out) {
    double ex = bx - ax, ey = by - ay;
    double cross = ex * (py - ay) - ey * (px - ax);
    *cross_out = cross;
    *dist_out = fabs(cross) / sqrt(ex * ex + ey * ey);
}

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: %s <vertex.spv> <fragment.spv>\n", argv[0]); return 1; }
    size_t vlen = 0, flen = 0;
    uint8_t *vspv = read_file(argv[1], &vlen), *fspv = read_file(argv[2], &flen);
    if (!vspv || !fspv) { fprintf(stderr, "cannot read SPIR-V inputs\n"); return 1; }

    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo = &app;
    if (vkCreateInstance(&ici, NULL, &inst) != VK_SUCCESS) { fprintf(stderr, "instance\n"); return 2; }
    uint32_t n = 0; vkEnumeratePhysicalDevices(inst, &n, NULL);
    if (!n) { fprintf(stderr, "no device\n"); return 2; }
    VkPhysicalDevice *pds = malloc(n * sizeof *pds);
    vkEnumeratePhysicalDevices(inst, &n, pds); pd = pds[0]; free(pds);
    VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(pd, &props);
    printf("device: %s (type=%d)\n", props.deviceName, (int)props.deviceType);

    uint32_t qn = 0; vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, NULL);
    VkQueueFamilyProperties *qp = malloc(qn * sizeof *qp);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qp);
    qf = UINT32_MAX;
    for (uint32_t i = 0; i < qn; i++) if (qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { qf = i; break; }
    free(qp);
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueFamilyIndex = qf; qci.queueCount = 1; qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    vkCreateDevice(pd, &dci, NULL, &dev);
    vkGetDeviceQueue(dev, qf, 0, &q);
    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pci.queueFamilyIndex = qf;
    vkCreateCommandPool(dev, &pci, NULL, &pool);
    VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cbai.commandPool = pool; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = 1;
    vkAllocateCommandBuffers(dev, &cbai, &cb);

    /* Target image + view. */
    VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ii.imageType = VK_IMAGE_TYPE_2D; ii.format = VK_FORMAT_B8G8R8A8_UNORM;
    ii.extent = (VkExtent3D){ W, H, 1 }; ii.mipLevels = 1; ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage img; vkCreateImage(dev, &ii, NULL, &img);
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, img, &mr);
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = mr.size; mai.memoryTypeIndex = find_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkDeviceMemory imem; vkAllocateMemory(dev, &mai, NULL, &imem); vkBindImageMemory(dev, img, imem, 0);
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = VK_FORMAT_B8G8R8A8_UNORM;
    vi.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkImageView view; vkCreateImageView(dev, &vi, NULL, &view);

    /* Render pass: clear to the harness background, end in TRANSFER_SRC for readback. */
    VkAttachmentDescription ad; memset(&ad, 0, sizeof ad);
    ad.format = VK_FORMAT_B8G8R8A8_UNORM; ad.samples = VK_SAMPLE_COUNT_1_BIT;
    ad.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; ad.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ad.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; ad.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    ad.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; ad.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkAttachmentReference ar = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sp; memset(&sp, 0, sizeof sp);
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; sp.colorAttachmentCount = 1; sp.pColorAttachments = &ar;
    VkRenderPassCreateInfo rpi = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    rpi.attachmentCount = 1; rpi.pAttachments = &ad; rpi.subpassCount = 1; rpi.pSubpasses = &sp;
    VkRenderPass rp; vkCreateRenderPass(dev, &rpi, NULL, &rp);
    VkFramebufferCreateInfo fbi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    fbi.renderPass = rp; fbi.attachmentCount = 1; fbi.pAttachments = &view; fbi.width = W; fbi.height = H; fbi.layers = 1;
    VkFramebuffer fb; vkCreateFramebuffer(dev, &fbi, NULL, &fb);

    /* Readback buffer. */
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = SZ; bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer rbuf; vkCreateBuffer(dev, &bi, NULL, &rbuf);
    VkMemoryRequirements br; vkGetBufferMemoryRequirements(dev, rbuf, &br);
    VkMemoryAllocateInfo bmai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    bmai.allocationSize = br.size;
    bmai.memoryTypeIndex = find_mem(br.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory bmem; vkAllocateMemory(dev, &bmai, NULL, &bmem); vkBindBufferMemory(dev, rbuf, bmem, 0);

    /* A4: shader modules and graphics pipeline. */
    VkShaderModuleCreateInfo smi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    smi.codeSize = vlen; smi.pCode = (const uint32_t *)vspv;
    VkShaderModule vmod; VkResult r1 = vkCreateShaderModule(dev, &smi, NULL, &vmod);
    smi.codeSize = flen; smi.pCode = (const uint32_t *)fspv;
    VkShaderModule fmod; VkResult r2 = vkCreateShaderModule(dev, &smi, NULL, &fmod);
    printf("A4 vkCreateShaderModule: vertex=%d fragment=%d\n", (int)r1, (int)r2);

    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkPipelineLayout pl; vkCreatePipelineLayout(dev, &pli, NULL, &pl);
    VkPipelineShaderStageCreateInfo st[2]; memset(st, 0, sizeof st);
    st[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    /* Entry names default to "main". ENTRY_V / ENTRY_F override them; the
     * committed examples/triangle/out/triangle_*.spv declare "triangle_vertex"
     * and "triangle_fragment", and a "main" lookup on those returns VkResult=-13. */
    const char *entry_v = getenv("ENTRY_V") ? getenv("ENTRY_V") : "main";
    const char *entry_f = getenv("ENTRY_F") ? getenv("ENTRY_F") : "main";
    st[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = vmod; st[0].pName = entry_v;
    st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fmod; st[1].pName = entry_f;
    VkPipelineVertexInputStateCreateInfo vis = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport vp = { 0, 0, W, H, 0.0f, 1.0f };
    VkRect2D sc = { { 0, 0 }, { W, H } };
    VkPipelineViewportStateCreateInfo vps = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vps.viewportCount = 1; vps.pViewports = &vp; vps.scissorCount = 1; vps.pScissors = &sc;
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba; memset(&cba, 0, sizeof cba);
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cbs = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cbs.attachmentCount = 1; cbs.pAttachments = &cba;
    VkGraphicsPipelineCreateInfo gpi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    gpi.stageCount = 2; gpi.pStages = st; gpi.pVertexInputState = &vis; gpi.pInputAssemblyState = &ia;
    gpi.pViewportState = &vps; gpi.pRasterizationState = &rs; gpi.pMultisampleState = &ms;
    gpi.pColorBlendState = &cbs; gpi.layout = pl; gpi.renderPass = rp; gpi.subpass = 0;
    VkPipeline pipe = VK_NULL_HANDLE;
    VkResult r3 = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, NULL, &pipe);
    printf("A4 vkCreateGraphicsPipelines: VkResult=%d\n", (int)r3);
    if (r1 != VK_SUCCESS || r2 != VK_SUCCESS || r3 != VK_SUCCESS || pipe == VK_NULL_HANDLE) {
        printf("A4 RESULT: FAIL (pipeline not created; see VkResult above)\n");
        printf("A5 RESULT: NOT RUN (no pipeline)\n");
        return 1;
    }
    printf("A4 RESULT: PASS (pipeline created)\n");

    /* A5: draw and read back. */
    const float bg_r = 0x20 / 255.0f, bg_g = 0x40 / 255.0f, bg_b = 0x60 / 255.0f;
    VkCommandBufferBeginInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &cbi);
    VkClearValue clear; memset(&clear, 0, sizeof clear);
    clear.color.float32[0] = bg_r; clear.color.float32[1] = bg_g;
    clear.color.float32[2] = bg_b; clear.color.float32[3] = 1.0f;
    VkRenderPassBeginInfo rbi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rbi.renderPass = rp; rbi.framebuffer = fb;
    rbi.renderArea.extent = (VkExtent2D){ W, H };
    rbi.clearValueCount = 1; rbi.pClearValues = &clear;
    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRenderPass(cb);
    VkBufferImageCopy cp; memset(&cp, 0, sizeof cp);
    cp.imageSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    cp.imageExtent = (VkExtent3D){ W, H, 1 };
    vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rbuf, 1, &cp);
    vkEndCommandBuffer(cb);
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(q);
    uint8_t *got = NULL; vkMapMemory(dev, bmem, 0, SZ, 0, (void **)&got);

    /* Expected image, computed from the NDC geometry in triangle.metal. */
    double ax = (0.0 + 1) / 2 * W, ay = (0.75 + 1) / 2 * H;     /* apex  */
    double bx = (-0.75 + 1) / 2 * W, by = (-0.75 + 1) / 2 * H;  /* left  */
    double cx = (0.75 + 1) / 2 * W, cy = (-0.75 + 1) / 2 * H;   /* right */
    int gated_bad = 0, gated_total = 0, bnd = 0, bnd_red = 0, first_bad = -1;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        double px = x + 0.5, py = y + 0.5;
        double c1, d1, c2, d2, c3, d3;
        edge_dist(px, py, ax, ay, bx, by, &c1, &d1);
        edge_dist(px, py, bx, by, cx, cy, &c2, &d2);
        edge_dist(px, py, cx, cy, ax, ay, &c3, &d3);
        /* Inside if all edge functions share the orientation sign of the triangle. */
        double area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
        double s = area >= 0 ? 1.0 : -1.0;
        int inside = (c1 * s >= 0) && (c2 * s >= 0) && (c3 * s >= 0);
        double mind = fmin(d1, fmin(d2, d3));
        uint8_t *p = got + (y * W + x) * BPP;
        int is_red = (p[0] == 0x00 && p[1] == 0x00 && p[2] == 0xFF && p[3] == 0xFF);
        int is_bg  = (p[0] == 0x60 && p[1] == 0x40 && p[2] == 0x20 && p[3] == 0xFF);
        if (mind < 1.0) {                 /* boundary band: informational only */
            bnd++;
            if (is_red) bnd_red++;
            continue;
        }
        gated_total++;
        int want_red = inside;
        int ok = want_red ? is_red : is_bg;
        if (!ok) { gated_bad++; if (first_bad < 0) first_bad = y * W + x; }
    }
    printf("A5 gated pixels: %d checked, %d mismatched\n", gated_total, gated_bad);
    if (gated_bad) {
        int x = first_bad % W, y = first_bad / W; uint8_t *p = got + first_bad * BPP;
        printf("A5 first mismatch x=%d y=%d got BGRA=%02x%02x%02x%02x\n", x, y, p[0], p[1], p[2], p[3]);
    }
    printf("A5 boundary band (1px, not gated): %d pixels, %d read back red\n", bnd, bnd_red);
    printf("A5 RESULT: %s\n", gated_bad ? "FAIL" : "PASS");
    vkUnmapMemory(dev, bmem);
    return gated_bad ? 1 : 0;
}
