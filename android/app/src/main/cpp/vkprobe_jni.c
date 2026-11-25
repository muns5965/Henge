/* S5 of docs/gpu-research/implementation-plan.md: which Vulkan devices does THIS phone expose to an app, and do they have what
   libapplegfx-vulkan calls (Vulkan 1.3 dynamic rendering, buffer device address)? Read-only: it creates one VkInstance, asks questions,
   destroys it. No device is created, no queue is used, nothing is drawn, no file is touched, and it does not know about the virtual machine.
   The text it returns is the whole result; a missing libvulkan or a failing call is reported in it, never crashes the app. */
#include <jni.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include <vulkan/vulkan.h>

#define OUT_MAX 6000

static void add(char *out, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void add(char *out, const char *fmt, ...) {
    size_t n = strlen(out);
    if (n >= OUT_MAX - 1) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(out + n, OUT_MAX - n, fmt, ap);
    va_end(ap);
}

static int has_ext(const VkExtensionProperties *e, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; i++) if (!strcmp(e[i].extensionName, name)) return 1;
    return 0;
}

static void probe(char *out) {
    uint32_t inst_ver = 0;
    if (vkEnumerateInstanceVersion(&inst_ver) != VK_SUCCESS) inst_ver = VK_API_VERSION_1_0;
    add(out, "instance version: %u.%u.%u\n", VK_VERSION_MAJOR(inst_ver), VK_VERSION_MINOR(inst_ver), VK_VERSION_PATCH(inst_ver));
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "henge-vkprobe", .apiVersion = inst_ver >= VK_API_VERSION_1_1 ? VK_API_VERSION_1_1 : VK_API_VERSION_1_0};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkInstance inst = VK_NULL_HANDLE;
    VkResult r = vkCreateInstance(&ici, NULL, &inst);
    if (r != VK_SUCCESS) { add(out, "vkCreateInstance FAILED: %d\n", (int)r); return; }
    uint32_t n = 0;
    r = vkEnumeratePhysicalDevices(inst, &n, NULL);
    add(out, "physical devices: %u (result %d)\n", n, (int)r);
    if (r == VK_SUCCESS && n > 0) {
        if (n > 8) n = 8;
        VkPhysicalDevice devs[8];
        r = vkEnumeratePhysicalDevices(inst, &n, devs);
        for (uint32_t i = 0; i < n && (r == VK_SUCCESS || r == VK_INCOMPLETE); i++) {
            VkPhysicalDeviceProperties p;
            vkGetPhysicalDeviceProperties(devs[i], &p);
            add(out, "\n[%u] %s\n    type %d, vendor 0x%x, device 0x%x, driver 0x%x\n    api %u.%u.%u\n", i, p.deviceName, (int)p.deviceType, p.vendorID, p.deviceID, p.driverVersion,
                VK_VERSION_MAJOR(p.apiVersion), VK_VERSION_MINOR(p.apiVersion), VK_VERSION_PATCH(p.apiVersion));
            uint32_t ne = 0;
            vkEnumerateDeviceExtensionProperties(devs[i], NULL, &ne, NULL);
            VkExtensionProperties *ex = ne ? calloc(ne, sizeof *ex) : NULL;
            if (ex && vkEnumerateDeviceExtensionProperties(devs[i], NULL, &ne, ex) <= VK_INCOMPLETE) {
                add(out, "    extensions: %u; dynamic_rendering %d, buffer_device_address %d, external_memory_fd %d, external_memory_dma_buf %d, android_hardware_buffer %d, timeline_semaphore %d\n", ne,
                    has_ext(ex, ne, "VK_KHR_dynamic_rendering"), has_ext(ex, ne, "VK_KHR_buffer_device_address"), has_ext(ex, ne, "VK_KHR_external_memory_fd"),
                    has_ext(ex, ne, "VK_EXT_external_memory_dma_buf"), has_ext(ex, ne, "VK_ANDROID_external_memory_android_hardware_buffer"), has_ext(ex, ne, "VK_KHR_timeline_semaphore"));
            }
            free(ex);
            if (inst_ver >= VK_API_VERSION_1_1 && p.apiVersion >= VK_API_VERSION_1_1) {
                VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
                VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &f13};
                VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = p.apiVersion >= VK_API_VERSION_1_3 ? (void *)&f12 : NULL};
                if (p.apiVersion < VK_API_VERSION_1_3) { f12.pNext = NULL; f2.pNext = p.apiVersion >= VK_API_VERSION_1_2 ? (void *)&f12 : NULL; }
                vkGetPhysicalDeviceFeatures2(devs[i], &f2);
                if (p.apiVersion >= VK_API_VERSION_1_2) add(out, "    features: bufferDeviceAddress %u, timelineSemaphore %u, descriptorIndexing %u\n", f12.bufferDeviceAddress, f12.timelineSemaphore, f12.descriptorIndexing);
                if (p.apiVersion >= VK_API_VERSION_1_3) add(out, "    features: dynamicRendering %u, synchronization2 %u, maintenance4 %u\n", f13.dynamicRendering, f13.synchronization2, f13.maintenance4);
                else add(out, "    Vulkan 1.3 features not reported (device API below 1.3)\n");
            } else add(out, "    feature query skipped (needs Vulkan 1.1)\n");
        }
    }
    vkDestroyInstance(inst, NULL);
}

JNIEXPORT jstring JNICALL Java_dev_muns_hengeprobe_VulkanProbe_probe(JNIEnv *env, jclass cls) {
    (void)cls;
    char *out = calloc(OUT_MAX, 1);
    if (!out) return (*env)->NewStringUTF(env, "out of memory");
    probe(out);
    jstring s = (*env)->NewStringUTF(env, out);
    free(out);
    return s;
}
