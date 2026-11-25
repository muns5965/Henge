"""Adapt pinned upstream SkyLight smoke to full readback and negative controls."""
from pathlib import Path
import sys
s=Path(sys.argv[1]).read_text()
def rep(a,b):
    global s
    assert s.count(a)==1,(a,s.count(a))
    s=s.replace(a,b)
rep('    vkCmdDraw(cmd, 3, 1, 0, 0);','    if (!getenv("GPU_TEST_NO_DRAW")) vkCmdDraw(cmd, 3, 1, 0, 0);')
rep('    vkCmdEndRenderPass(cmd);','''    vkCmdEndRenderPass(cmd);
    VkImageMemoryBarrier barrier = { .sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
       .srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT,
       .oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
       .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
       .image=image,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&barrier);''')
rep('    int ok = (c.r == want.r && c.g == want.g && c.b == want.b && c.a == want.a);','''    unsigned mismatches=0;
    const char *out=getenv("GPU_TEST_OUTPUT"); FILE *fp=out?fopen(out,"wb"):NULL;
    if(out&&!fp)die("output open");
    for(unsigned i=0;i<W*H;i++){
        RGBA p=px[i];
        if(p.r!=want.r||p.g!=want.g||p.b!=want.b||p.a!=want.a)mismatches++;
        if(fp){uint8_t bgra[4]={p.b,p.g,p.r,p.a};if(fwrite(bgra,1,4,fp)!=4)die("output write");}
    }
    if(fp&&fclose(fp))die("output close");
    printf("SKYLIGHT pixels: %u checked, %u mismatched\\n",W*H,mismatches);
    int ok=mismatches==0;''')
# Explicitly require a graphics queue rather than assuming queue family zero.
rep('    float prio = 1.0f;','''    uint32_t nq=0,qf=UINT32_MAX;vkGetPhysicalDeviceQueueFamilyProperties(phys,&nq,NULL);
    VkQueueFamilyProperties *qp=calloc(nq,sizeof(*qp));if(!qp)die("queue alloc");
    vkGetPhysicalDeviceQueueFamilyProperties(phys,&nq,qp);
    for(uint32_t i=0;i<nq;i++)if(qp[i].queueCount&&(qp[i].queueFlags&VK_QUEUE_GRAPHICS_BIT)){qf=i;break;}
    free(qp);if(qf==UINT32_MAX)die("no graphics queue");
    float prio = 1.0f;''')
s=s.replace('.queueFamilyIndex = 0','.queueFamilyIndex = qf').replace('vkGetDeviceQueue(dev, 0, 0, &queue)','vkGetDeviceQueue(dev, qf, 0, &queue)')
s=s.replace('vkQueueSubmit(queue, 1, &usi, VK_NULL_HANDLE); vkQueueWaitIdle(queue);','if(vkQueueSubmit(queue,1,&usi,VK_NULL_HANDLE)!=VK_SUCCESS||vkQueueWaitIdle(queue)!=VK_SUCCESS)die("texture submit");')
s=s.replace('vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE); vkQueueWaitIdle(queue);','if(vkQueueSubmit(queue,1,&si,VK_NULL_HANDLE)!=VK_SUCCESS||vkQueueWaitIdle(queue)!=VK_SUCCESS)die("submit");')
s=s.replace('vkMapMemory(dev, bmem, 0, VK_WHOLE_SIZE, 0, &mapped);','if(vkMapMemory(dev,bmem,0,VK_WHOLE_SIZE,0,&mapped)!=VK_SUCCESS||!mapped)die("readback map");')
s=s.replace('    if (ok) fprintf(stdout, "PASS: real SkyLight fragment rendered the correct colour on lavapipe\\n");','    if (ok) fprintf(stdout, "PASS: real SkyLight fragment rendered the correct colour\\n");')
rep('    return ok ? 0 : 1;','    printf("SKYLIGHT RESULT: %s\\n",ok?"PASS":"FAIL"); return ok ? 0 : 1;')
Path(sys.argv[2]).write_text(s)
