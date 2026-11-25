/* Android Vulkan loader compatibility: resolve core 1.2/1.3 device commands
 * through vkGetDeviceProcAddr. Fail loudly rather than silently skipping draws.
 * One device per isolated test process. GNU ld --wrap keeps upstream sources intact.
 */
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
static VkDevice active_device;
VKAPI_ATTR VkResult VKAPI_CALL __real_vkCreateDevice(VkPhysicalDevice,const VkDeviceCreateInfo*,const VkAllocationCallbacks*,VkDevice*);
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkCreateDevice(VkPhysicalDevice physical,const VkDeviceCreateInfo *ci,const VkAllocationCallbacks *alloc,VkDevice *device){
    VkResult r=__real_vkCreateDevice(physical,ci,alloc,device);
    if(r==VK_SUCCESS)active_device=*device;
    return r;
}
static PFN_vkVoidFunction resolve(VkDevice device,const char *name){
    PFN_vkVoidFunction p=device?vkGetDeviceProcAddr(device,name):NULL;
    if(!p){fprintf(stderr,"Vulkan required device function unavailable: %s\n",name);exit(2);}
    return p;
}
VKAPI_ATTR void VKAPI_CALL __wrap_vkCmdBeginRendering(VkCommandBuffer cb,const VkRenderingInfo *info){
    ((PFN_vkCmdBeginRendering)resolve(active_device,"vkCmdBeginRendering"))(cb,info);
}
VKAPI_ATTR void VKAPI_CALL __wrap_vkCmdEndRendering(VkCommandBuffer cb){
    ((PFN_vkCmdEndRendering)resolve(active_device,"vkCmdEndRendering"))(cb);
}
VKAPI_ATTR VkDeviceAddress VKAPI_CALL __wrap_vkGetBufferDeviceAddress(VkDevice device,const VkBufferDeviceAddressInfo *info){
    return ((PFN_vkGetBufferDeviceAddress)resolve(device,"vkGetBufferDeviceAddress"))(device,info);
}
