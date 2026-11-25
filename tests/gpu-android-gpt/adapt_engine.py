"""Adapt pinned MIT upstream render-target smoke; no guest command claims."""
import sys
from pathlib import Path
s=Path(sys.argv[1]).read_text()
def replace(a,b):
    global s
    assert s.count(a)==1,a
    s=s.replace(a,b)
replace('float red[4] = { 1.0f, 0.0f, 0.0f, 1.0f };','float red[4] = { 1.0f, 0.0f, 0.0f, 1.0f }; if(getenv("GPU_TEST_NO_DRAW")) red[0]=0;')
replace('    /* Pixel at (32, 32):','''    if(rb_st!=LAGFX_OK || stride!=256) return 2;
    unsigned bad=0;
    for(unsigned i=0;i<4096;i++){const uint8_t *p=pixels+i*4;if(p[0]!=0||p[1]!=0||p[2]!=255||p[3]!=255)bad++;}
    const char *out=getenv("GPU_TEST_OUTPUT");
    if(out){FILE *f=fopen(out,"wb");if(!f)exit(2);size_t z=fwrite(pixels,1,buf_bytes,f);int e=fclose(f);if(z!=buf_bytes||e)exit(2);}
    printf("ENGINE pixels: 4096 checked, %u mismatched\n",bad);
    if(bad)g_fail++;
    /* Pixel at (32, 32):''')
# Python triple strings above need literal C escape.
s=s.replace('mismatched\n",bad)', 'mismatched\\n",bad)')
replace('        (void)render_and_readback(dev->vk);','''        int rc=render_and_readback(dev->vk);
        if(rc){fprintf(stderr,"ENGINE operation error %d\\n",rc);return 2;}''')
replace('    return g_fail ? 1 : 0;', '''    if(g_skipped)return 2;
    printf("ENGINE RESULT: %s\\n",g_fail?"FAIL":"PASS");
    return g_fail ? 1 : 0;''')
replace('        return 0;\n    }\n\n#ifdef LAGFX_HAVE_VULKAN','        return 2;\n    }\n\n#ifdef LAGFX_HAVE_VULKAN')
# A failed setup/submit is not a valid negative pixel result.
s=s.replace('        g_fail++; \\\n','        g_fail++; \\\n')
Path(sys.argv[2]).write_text(s)
