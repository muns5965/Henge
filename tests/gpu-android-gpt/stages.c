/* Standalone Vulkan tests, no VM or guest memory. Process exits after each case. */
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "pvg_half.h"
#define W 64
#define H 64
#define N (W*H)
#define CK(x) do { VkResult r=(x); if(r!=VK_SUCCESS){fprintf(stderr,"%s: VkResult=%d\n",#x,r);return 2;} } while(0)
static VkPhysicalDevice pd;
static VkDevice d;
static uint32_t memtype(uint32_t bits,VkMemoryPropertyFlags flags){
 VkPhysicalDeviceMemoryProperties p;vkGetPhysicalDeviceMemoryProperties(pd,&p);
 for(uint32_t i=0;i<p.memoryTypeCount;i++)if((bits&(1u<<i))&&(p.memoryTypes[i].propertyFlags&flags)==flags)return i;
 fprintf(stderr,"no memory type\n");exit(2);
}
static int buffer(VkDeviceSize size,VkBufferUsageFlags usage,VkBuffer *b,VkDeviceMemory *m){
 VkBufferCreateInfo c={VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};c.size=size;c.usage=usage;c.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
 CK(vkCreateBuffer(d,&c,0,b));VkMemoryRequirements r;vkGetBufferMemoryRequirements(d,*b,&r);
 VkMemoryAllocateInfo a={VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};a.allocationSize=r.size;a.memoryTypeIndex=memtype(r.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
 CK(vkAllocateMemory(d,&a,0,m));CK(vkBindBufferMemory(d,*b,*m,0));return 0;
}
static int image(VkFormat fmt,uint32_t w,uint32_t h,VkImageUsageFlags usage,VkImage *im,VkDeviceMemory *m,VkImageView *v){
 VkImageCreateInfo c={VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};c.imageType=VK_IMAGE_TYPE_2D;c.format=fmt;c.extent=(VkExtent3D){w,h,1};c.mipLevels=1;c.arrayLayers=1;c.samples=VK_SAMPLE_COUNT_1_BIT;c.tiling=VK_IMAGE_TILING_OPTIMAL;c.usage=usage;c.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
 CK(vkCreateImage(d,&c,0,im));VkMemoryRequirements r;vkGetImageMemoryRequirements(d,*im,&r);
 VkMemoryAllocateInfo a={VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};a.allocationSize=r.size;a.memoryTypeIndex=memtype(r.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);CK(vkAllocateMemory(d,&a,0,m));CK(vkBindImageMemory(d,*im,*m,0));
 VkImageViewCreateInfo vi={VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=*im;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=fmt;vi.subresourceRange=(VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};CK(vkCreateImageView(d,&vi,0,v));return 0;
}
static void barrier(VkCommandBuffer cb,VkImage im,VkImageLayout old,VkImageLayout next,VkAccessFlags a,VkAccessFlags b,VkPipelineStageFlags src,VkPipelineStageFlags dst){
 VkImageMemoryBarrier v={VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};v.image=im;v.oldLayout=old;v.newLayout=next;v.srcAccessMask=a;v.dstAccessMask=b;v.srcQueueFamilyIndex=v.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;v.subresourceRange=(VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
 vkCmdPipelineBarrier(cb,src,dst,0,0,0,0,0,1,&v);
}
static int shader(const char *path,VkShaderModule *s){
 FILE *f=fopen(path,"rb");if(!f)return 2;fseek(f,0,SEEK_END);long n=ftell(f);rewind(f);if(n<20||n>1048576||n%4){fclose(f);return 2;}uint32_t *p=malloc(n);if(!p){fclose(f);return 2;}size_t z=fread(p,1,n,f);fclose(f);if(z!=(size_t)n||p[0]!=0x07230203){free(p);return 2;}
 VkShaderModuleCreateInfo c={VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};c.codeSize=n;c.pCode=p;VkResult r=vkCreateShaderModule(d,&c,0,s);free(p);if(r){fprintf(stderr,"shader VkResult=%d\n",r);return 2;}return 0;
}
int main(int argc,char **argv){
 if(argc!=4)return 2;
 int stage=atoi(argv[1]);if(stage<2||stage>4)return 2;
 int negative=getenv("GPU_TEST_NO_DRAW")!=NULL;
 VkApplicationInfo app={VK_STRUCTURE_TYPE_APPLICATION_INFO};app.apiVersion=VK_API_VERSION_1_0;
 VkInstanceCreateInfo ic={VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};ic.pApplicationInfo=&app;VkInstance inst;CK(vkCreateInstance(&ic,0,&inst));
 uint32_t n=0;CK(vkEnumeratePhysicalDevices(inst,&n,0));if(!n)return 2;
 VkPhysicalDevice *list=calloc(n,sizeof(*list));if(!list)return 2;CK(vkEnumeratePhysicalDevices(inst,&n,list));pd=list[0];free(list);
 VkPhysicalDeviceProperties props;vkGetPhysicalDeviceProperties(pd,&props);printf("device: %s (type=%d), stage=%d\n",props.deviceName,props.deviceType,stage);
 uint32_t qn=0;vkGetPhysicalDeviceQueueFamilyProperties(pd,&qn,0);VkQueueFamilyProperties *qs=calloc(qn,sizeof(*qs));if(!qs)return 2;vkGetPhysicalDeviceQueueFamilyProperties(pd,&qn,qs);
 uint32_t qf=UINT32_MAX;for(uint32_t i=0;i<qn;i++)if(qs[i].queueFlags&VK_QUEUE_GRAPHICS_BIT){qf=i;break;}free(qs);if(qf==UINT32_MAX)return 2;
 float priority=1;VkDeviceQueueCreateInfo qc={VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};qc.queueFamilyIndex=qf;qc.queueCount=1;qc.pQueuePriorities=&priority;
 VkDeviceCreateInfo dc={VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};dc.queueCreateInfoCount=1;dc.pQueueCreateInfos=&qc;CK(vkCreateDevice(pd,&dc,0,&d));VkQueue q;vkGetDeviceQueue(d,qf,0,&q);
 VkFormat fmt=stage==4?VK_FORMAT_R16G16B16A16_SFLOAT:VK_FORMAT_R8G8B8A8_UNORM;
 VkFormatProperties fp;vkGetPhysicalDeviceFormatProperties(pd,fmt,&fp);if(!(fp.optimalTilingFeatures&VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)||(stage==3&&!(fp.optimalTilingFeatures&VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT))){printf("STAGE %d UNSUPPORTED format\n",stage);return 2;}
 VkImage target,tex=VK_NULL_HANDLE;VkDeviceMemory tm,texm,uploadm,rbm;VkImageView tv,texv;VkBuffer upload,rb;
 unsigned bytes=stage==4?8:4;if(image(fmt,W,H,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,&target,&tm,&tv)||buffer(N*bytes,VK_BUFFER_USAGE_TRANSFER_DST_BIT,&rb,&rbm))return 2;
 VkDescriptorSetLayout dl=VK_NULL_HANDLE;VkDescriptorSet ds=VK_NULL_HANDLE;VkSampler sampler;
 if(stage==2){
  if(image(VK_FORMAT_R8G8B8A8_UNORM,8,8,VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,&tex,&texm,&texv)||buffer(256,VK_BUFFER_USAGE_TRANSFER_SRC_BIT,&upload,&uploadm))return 2;
  uint8_t *p;CK(vkMapMemory(d,uploadm,0,256,0,(void**)&p));for(int y=0;y<8;y++)for(int x=0;x<8;x++){int i=(y*8+x)*4,c=(x+y)&1;p[i]=c?230:20;p[i+1]=c?210:60;p[i+2]=c?40:180;p[i+3]=255;}vkUnmapMemory(d,uploadm);
  VkSamplerCreateInfo sc={VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};sc.magFilter=sc.minFilter=VK_FILTER_NEAREST;sc.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;sc.addressModeU=sc.addressModeV=sc.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;CK(vkCreateSampler(d,&sc,0,&sampler));
  VkDescriptorSetLayoutBinding bind={0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,0};VkDescriptorSetLayoutCreateInfo lc={VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};lc.bindingCount=1;lc.pBindings=&bind;CK(vkCreateDescriptorSetLayout(d,&lc,0,&dl));
  VkDescriptorPoolSize ps={VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1};VkDescriptorPoolCreateInfo pc={VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pc.maxSets=1;pc.poolSizeCount=1;pc.pPoolSizes=&ps;VkDescriptorPool dp;CK(vkCreateDescriptorPool(d,&pc,0,&dp));
  VkDescriptorSetAllocateInfo da={VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};da.descriptorPool=dp;da.descriptorSetCount=1;da.pSetLayouts=&dl;CK(vkAllocateDescriptorSets(d,&da,&ds));
  VkDescriptorImageInfo di={sampler,texv,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};VkWriteDescriptorSet wr={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};wr.dstSet=ds;wr.descriptorCount=1;wr.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;wr.pImageInfo=&di;vkUpdateDescriptorSets(d,1,&wr,0,0);
 }
 VkAttachmentDescription ad={0};ad.format=fmt;ad.samples=VK_SAMPLE_COUNT_1_BIT;ad.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;ad.storeOp=VK_ATTACHMENT_STORE_OP_STORE;ad.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;ad.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;ad.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;ad.finalLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
 VkAttachmentReference ar={0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};VkSubpassDescription sub={0};sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;sub.colorAttachmentCount=1;sub.pColorAttachments=&ar;
 VkRenderPassCreateInfo rc={VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};rc.attachmentCount=1;rc.pAttachments=&ad;rc.subpassCount=1;rc.pSubpasses=&sub;VkRenderPass rp;CK(vkCreateRenderPass(d,&rc,0,&rp));
 VkFramebufferCreateInfo fc={VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};fc.renderPass=rp;fc.attachmentCount=1;fc.pAttachments=&tv;fc.width=W;fc.height=H;fc.layers=1;VkFramebuffer fb;CK(vkCreateFramebuffer(d,&fc,0,&fb));
 VkPipelineLayoutCreateInfo lc={VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};if(stage==2){lc.setLayoutCount=1;lc.pSetLayouts=&dl;}VkPushConstantRange pr={VK_SHADER_STAGE_FRAGMENT_BIT,0,16};if(stage==3){lc.pushConstantRangeCount=1;lc.pPushConstantRanges=&pr;}VkPipelineLayout pl;CK(vkCreatePipelineLayout(d,&lc,0,&pl));
 VkShaderModule vs,fs;if(shader(argv[2],&vs)||shader(argv[3],&fs))return 2;
 VkPipelineShaderStageCreateInfo ss[2]={{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};ss[0].stage=VK_SHADER_STAGE_VERTEX_BIT;ss[0].module=vs;ss[0].pName="main";ss[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;ss[1].module=fs;ss[1].pName="main";
 VkPipelineVertexInputStateCreateInfo vi={VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};VkPipelineInputAssemblyStateCreateInfo ia={VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
 VkViewport vp={0,0,W,H,0,1};VkRect2D rect={{0,0},{W,H}};VkPipelineViewportStateCreateInfo vc={VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};vc.viewportCount=1;vc.pViewports=&vp;vc.scissorCount=1;vc.pScissors=&rect;
 VkPipelineRasterizationStateCreateInfo rs={VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};rs.polygonMode=VK_POLYGON_MODE_FILL;rs.cullMode=VK_CULL_MODE_NONE;rs.lineWidth=1;
 VkPipelineMultisampleStateCreateInfo ms={VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
 VkPipelineColorBlendAttachmentState ba={0};ba.colorWriteMask=15;if(stage==3){ba.blendEnable=VK_TRUE;ba.srcColorBlendFactor=VK_BLEND_FACTOR_SRC_ALPHA;ba.dstColorBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;ba.colorBlendOp=VK_BLEND_OP_ADD;ba.srcAlphaBlendFactor=VK_BLEND_FACTOR_ONE;ba.dstAlphaBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;ba.alphaBlendOp=VK_BLEND_OP_ADD;}
 VkPipelineColorBlendStateCreateInfo bc={VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};bc.attachmentCount=1;bc.pAttachments=&ba;VkDynamicState dyn=VK_DYNAMIC_STATE_SCISSOR;VkPipelineDynamicStateCreateInfo dy={VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};dy.dynamicStateCount=1;dy.pDynamicStates=&dyn;
 VkGraphicsPipelineCreateInfo gc={VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};gc.stageCount=2;gc.pStages=ss;gc.pVertexInputState=&vi;gc.pInputAssemblyState=&ia;gc.pViewportState=&vc;gc.pRasterizationState=&rs;gc.pMultisampleState=&ms;gc.pColorBlendState=&bc;gc.pDynamicState=&dy;gc.layout=pl;gc.renderPass=rp;VkPipeline pipe;CK(vkCreateGraphicsPipelines(d,VK_NULL_HANDLE,1,&gc,0,&pipe));printf("PIPELINE PASS\n");
 VkCommandPoolCreateInfo pc={VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pc.queueFamilyIndex=qf;VkCommandPool pool;CK(vkCreateCommandPool(d,&pc,0,&pool));VkCommandBufferAllocateInfo ca={VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ca.commandPool=pool;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ca.commandBufferCount=1;VkCommandBuffer cb;CK(vkAllocateCommandBuffers(d,&ca,&cb));
 VkCommandBufferBeginInfo begin={VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};CK(vkBeginCommandBuffer(cb,&begin));
 if(stage==2){barrier(cb,tex,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);VkBufferImageCopy cp={0};cp.imageSubresource=(VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};cp.imageExtent=(VkExtent3D){8,8,1};vkCmdCopyBufferToImage(cb,upload,tex,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&cp);barrier(cb,tex,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);}
 VkClearValue clear={0};clear.color.float32[3]=1;VkRenderPassBeginInfo bi={VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};bi.renderPass=rp;bi.framebuffer=fb;bi.renderArea=rect;bi.clearValueCount=1;bi.pClearValues=&clear;vkCmdBeginRenderPass(cb,&bi,VK_SUBPASS_CONTENTS_INLINE);vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,pipe);vkCmdSetScissor(cb,0,1,&rect);
 if(stage==2)vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,pl,0,1,&ds,0,0);
 if(!negative){
  if(stage==3){float red[4]={1,0,0,.5f},green[4]={0,1,0,.5f};VkRect2D a={{8,8},{32,32}},b={{24,24},{32,32}};vkCmdSetScissor(cb,0,1,&a);vkCmdPushConstants(cb,pl,VK_SHADER_STAGE_FRAGMENT_BIT,0,16,red);vkCmdDraw(cb,3,1,0,0);vkCmdSetScissor(cb,0,1,&b);vkCmdPushConstants(cb,pl,VK_SHADER_STAGE_FRAGMENT_BIT,0,16,green);vkCmdDraw(cb,3,1,0,0);}
  else vkCmdDraw(cb,3,1,0,0);
 }
 vkCmdEndRenderPass(cb);barrier(cb,target,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
 VkBufferImageCopy cp={0};cp.imageSubresource=(VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};cp.imageExtent=(VkExtent3D){W,H,1};vkCmdCopyImageToBuffer(cb,target,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,rb,1,&cp);CK(vkEndCommandBuffer(cb));VkSubmitInfo si={VK_STRUCTURE_TYPE_SUBMIT_INFO};si.commandBufferCount=1;si.pCommandBuffers=&cb;CK(vkQueueSubmit(q,1,&si,VK_NULL_HANDLE));CK(vkQueueWaitIdle(q));
 uint8_t *raw;CK(vkMapMemory(d,rbm,0,N*bytes,0,(void**)&raw));uint8_t bgra[N*4];unsigned bad=0, converted_bad=0;
 for(int y=0;y<H;y++)for(int x=0;x<W;x++){
  int i=y*W+x;int expected[4]={0,0,0,255};uint8_t *o=bgra+i*4;
  if(stage==4){uint16_t *h=(uint16_t*)raw+i*4;float want[4]={(float)(x/16)*.25f,(float)(y/16)*.25f,1.5f,1.f};for(int k=0;k<4;k++){float a=hv_half_to_float(h[k]);if(!isfinite(a)||fabsf(a-want[k])>.001f){bad++;break;}}}
  else {uint8_t *p=raw+i*4;if(stage==2){int c=(x/8+y/8)&1;expected[0]=c?230:20;expected[1]=c?210:60;expected[2]=c?40:180;}
   else {int a=x>=8&&x<40&&y>=8&&y<40,b=x>=24&&x<56&&y>=24&&y<56;expected[0]=a?(b?64:128):0;expected[1]=b?128:0;}
   for(int k=0;k<4;k++)if(abs((int)p[k]-expected[k])>(stage==3?1:0)){bad++;break;}
   o[0]=p[2];o[1]=p[1];o[2]=p[0];o[3]=p[3];
  }
 }
 if(stage==4){const uint8_t *lut=hv_half_to_byte_table();if(!lut)return 2;for(int y=0;y<H;y++)hv_half_row_to_bgra((const uint16_t*)raw+y*W*4,bgra+y*W*4,W,lut);for(int y=0;y<H;y++)for(int x=0;x<W;x++){uint8_t *p=bgra+(y*W+x)*4;if(p[0]!=255||p[1]!=(uint8_t)((y/16)*.25f*255+.5f)||p[2]!=(uint8_t)((x/16)*.25f*255+.5f)||p[3]!=255)converted_bad++;}}
 const char *out=getenv("GPU_TEST_OUTPUT");if(out){FILE *f=fopen(out,"wb");if(!f)return 2;size_t z=fwrite(bgra,1,sizeof bgra,f);int e=fclose(f);if(z!=sizeof bgra||e)return 2;}
 if(stage==4){printf("FP16 raw: %d checked, %u mismatched\n",N,bad);printf("BGRA converted: %d checked, %u mismatched\n",N,converted_bad);}else printf("STAGE %d pixels: %d checked, %u mismatched\n",stage,N,bad);bad+=converted_bad;printf("STAGE %d RESULT: %s\n",stage,bad?"FAIL":"PASS");vkUnmapMemory(d,rbm);return bad?1:0;
}
