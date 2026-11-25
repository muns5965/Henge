#!/usr/bin/env python3
"""Inventory upstream test targets; build separately so failures stay visible."""
import concurrent.futures, pathlib, re, subprocess, sys, zipfile
root=pathlib.Path('/tmp/lagfx')
archive=pathlib.Path(sys.argv[3]) if len(sys.argv)>3 else root/'android-build/libapplegfx-vulkan.a'
app=pathlib.Path(sys.argv[1])
cc=sys.argv[2]
here=pathlib.Path(__file__).resolve().parent
assets=app/'src/main/assets'
native=app/'src/main/jniLibs/arm64-v8a'
logs=pathlib.Path('/tmp/unit-build-logs'); logs.mkdir(exist_ok=True)
meson=(root/'tests/meson.build').read_text()
pairs=re.findall(r"executable\('([^']+)',\s*'([^']+\.c)'",meson)
assert len(pairs)==76 and len({p[0] for p in pairs})==76
# Also inventory top-level tests not registered as executable targets.
extra=sorted(p for p in (root/'tests').glob('*.c') if p.name not in {v for _,v in pairs})
entries=[(n,f,True) for n,f in pairs]+[(p.stem,p.name,False) for p in extra]
manual={'skylight-lavapipe-render':'requires explicit shader arguments; covered by stages 7–8',
        'translator-microbench':'benchmark, not a unit-test assertion gate',
        'lavapipe-smoke':'upstream Linux lavapipe-specific manual test'}
# Extract the exact upstream triangle AIR, without substituting a GLSL shader.
subprocess.run(['gcc','-std=c11','-D_GNU_SOURCE','-I'+str(root/'include'),'-I'+str(root/'src'),
    str(root/'examples/triangle/extract_only.c'),str(root/'build/libapplegfx-vulkan.a'),
    '-lvulkan','-lm','-o','/tmp/unit-extract'],check=True)
subprocess.run(['/tmp/unit-extract',str(root/'tests/fixtures/triangle.metallib'),str(root/'tests/fixtures')],check=True)
assert (root/'tests/fixtures/triangle_vertex.air.bc').is_file()
# Keep original fixture names only inside an opaque ZIP asset; restore at runtime.
with zipfile.ZipFile(assets/'unit-fixtures.zip','w',zipfile.ZIP_DEFLATED) as z:
    for rel in ('tests/fixtures','examples/triangle/out','src/shaders/spv-apple'):
        for p in sorted((root/rel).rglob('*')):
            if p.is_file(): z.write(p,p.relative_to(root))
    for p in sorted((root/'src/shaders/spv-apple').glob('*.spv')):
        z.write(p,'src/shaders/spv-apple/apple_'+p.name)
    for name in ('reference_vert.spv','reference_frag.spv'):
        z.write(root/'examples/triangle/out'/name,'unit-triangle/'+name)
    for p in sorted(pathlib.Path('/tmp/gpu-assets').glob('triangle_*.spv')):
        z.write(p,'unit-triangle/'+p.name)
# Adapt paths and stale test assumptions; production translator untouched.
adapted=pathlib.Path('/tmp/unit-adapted'); adapted.mkdir(exist_ok=True)
def adapt(src):
    p=root/'tests'/src
    text=p.read_text()
    text=text.replace('"/opt/homebrew/bin/spirv-val",','getenv("LAGFX_ANDROID_SPIRV_VAL") ? getenv("LAGFX_ANDROID_SPIRV_VAL") : "/opt/homebrew/bin/spirv-val",')
    # Android /tmp is not a writable POSIX temp directory in app sandbox.
    text=text.replace('"/tmp/lagfx_', '"./lagfx_')
    text=text.replace('"/tmp/scoping/triangle_vertex.air.bc"','"tests/fixtures/triangle_vertex.air.bc"')
    text=text.replace('"/Users/mjackson/Developer/staging/triangle_vertex.air.bc"','"tests/fixtures/triangle_vertex.air.bc"')
    text=text.replace('"/tmp/air-bc-dump/ViewportToNDC.air.bc"','"tests/fixtures/login_viewporttondc_vert.air.bc"')
    text=text.replace('"/Users/mjackson/Developer/libapplegfx-vulkan/scratch/phase2_4_diagnosis/air-bc/Vfx.air.bc"','"tests/fixtures/login_vfx_vert.air.bc"')
    # Vulkan validation must use a target environment accepting SPIR-V 1.4.
    text=re.sub(r'execl\((\w+),\s*"spirv-val",\s*(tmpl),',r'execl(\1, "spirv-val", "--target-env", "vulkan1.2", \2,',text)
    if src in ('handler-task.c','handler-memory.c'):
        text=text.replace('((uint64_t*)payload)[1][0]','((uint64_t*)payload)[1]').replace('((uint64_t*)payload)[1][1]','((uint64_t*)payload)[2]')
    # Bind the real disassembler rather than relying on Android shell PATH.
    text=text.replace('"/opt/homebrew/bin/spirv-dis",','getenv("LAGFX_ANDROID_SPIRV_DIS") ? getenv("LAGFX_ANDROID_SPIRV_DIS") : "/opt/homebrew/bin/spirv-dis",')
    if src=='air2spv-translate-function-binop-unop-unit.c':
        text=text.replace('#include "air2spv/translate.h"', '#include "air2spv/translate.h"\n#include "air2spv/translate_function.h"')
        a=text.index('static int test_unop_vfx_emits_fnegate(void)')
        b=text.index('/* CAST dispatch test:',a)
        text=text[:a]+r'''
static int test_unop_vfx_emits_fnegate(void) {
    size_t n=0; uint8_t *air=slurp("tests/fixtures/login_xgc_frag.air.bc",&n);
    lagfx_air_module_t *m=NULL;
    if(!air || lagfx_air_module_open(air,n,&m)!=LAGFX_OK || !m){free(air);return 1;}
    const unsigned indices[]={21,32,44}, expected[]={1,1,13};
    uint32_t nf=0; const lagfx_air_function_t *fn=lagfx_air_module_functions(m,&nf);
    int bad=0;
    for(unsigned k=0;k<3;k++){
        unsigned i=indices[k], in=0,out=0; uint32_t ni=0;
        lagfx_air_function_body_t *body=NULL;
        if(!fn || i>=nf || fn[i].is_proto || lagfx_air_function_body_open(m,i,&body)!=LAGFX_OK || !body){bad=1;continue;}
        const lagfx_air_inst_t *ins=lagfx_air_function_body_instructions(body,&ni);
        for(uint32_t j=0;ins && j<ni;j++)
            if(ins[j].raw_code==56 && ins[j].num_ops>=2 && ins[j].ops[1]==0)in++;
        lagfx_air_function_body_free(body);
        uint8_t *spv=NULL;size_t sz=0;
        int ok=lagfx_air2spv_translate_function(m,i,LAGFX_XLATE_STAGE_FRAGMENT,&spv,&sz)==LAGFX_OK && spv && sz>=20 && sz%4==0;
        if(ok){
            const uint32_t *w=(const uint32_t*)spv;
            ok=w[0]==SPV_MAGIC;
            for(size_t j=5;ok && j<sz/4;){
                unsigned wc=w[j]>>16;
                if(!wc || wc>sz/4-j){ok=0;break;}
                if((w[j]&65535)==127)out++;
                j+=wc;
            }
        }
        ok=ok && in==expected[k] && out==in && spirv_val(spv,sz)==1;
        printf("%s: Xgc fn=%u AIR FNeg=%u SPIR-V FNegate=%u expected=%u\n",ok?"PASS":"FAIL",i,in,out,expected[k]);
        bad|=!ok;free(spv);
    }
    lagfx_air_module_free(m);free(air);return bad;
}
'''+text[b:]
        text=text.replace('BINOP/UNOP handlers exercised;','triangle translation smoke;')
        # A real math fixture is already independently tested for lowering.
        # Require structural validation as well, rather than silently skipping.
        a=text.index('static int test_call_dispatch_emits_extinst(void)')
        b=text.index('/* SHUFFLEVEC mask resolution test:',a)
        text=text[:a]+r'''
static int test_call_dispatch_emits_extinst(void){
    const char *paths[]={"tests/fixtures/mathclean_vertex.air.bc","tests/fixtures/scalarmath_vertex.air.bc"};
    unsigned verified=0;
    for(unsigned k=0;k<2;k++){
        size_t n=0;uint8_t *air=slurp(paths[k],&n);lagfx_air_module_t *m=NULL;
        if(!air||lagfx_air_module_open(air,n,&m)!=LAGFX_OK||!m){free(air);return 1;}
        uint8_t *spv=NULL;size_t sz=0;
        int ok=lagfx_air2spv_translate_module(m,&spv,&sz)==LAGFX_OK&&spv&&sz>=20&&sz%4==0;
        unsigned ext=0;
        if(ok){
            const uint32_t *w=(const uint32_t*)spv;ok=w[0]==SPV_MAGIC;
            for(size_t j=5;ok&&j<sz/4;){
                unsigned wc=w[j]>>16;
                if(!wc||wc>sz/4-j){ok=0;break;}
                if((w[j]&65535)==12)ext++;
                j+=wc;
            }
        }
        int val=ok?spirv_val(spv,sz):-1;
        printf("CALL fixture %s extinst=%u validation=%d\n",paths[k],ext,val);
        if(ok&&ext&&val==1)verified++;
        free(spv);lagfx_air_module_free(m);free(air);
    }
    printf("%s: real math AIR CALL emits validated OpExtInst\n",verified?"PASS":"FAIL");
    return !verified;
}
'''+text[b:]

    if src in ('handler-memory.c','handler-task.c','handler-sync-utility.c'):
        # Exercise real ring drainers, never the acknowledge-only test_api.c.
        text=text.replace('#include <stdbool.h>', '#include "dispatchers/channel_0_dispatcher.h"\n#include "dispatchers/channel_compute_dispatcher.h"\n#include "dispatchers/ring_common.h"\n#include <stdbool.h>')
        a=text.index('static bool mock_read(')
        b=text.index('static int test_',a)
        text=text[:a]+r'''
static uint8_t ram[0x5000];
static bool mock_read(void *op,uint64_t gpa,uint64_t len,void *out){
    (void)op;
    if(gpa>sizeof(ram)||len>sizeof(ram)-gpa)return false;
    memcpy(out,ram+gpa,len);return true;
}
static bool mock_write(void *op,uint64_t gpa,uint64_t len,const void *in){
    (void)op;
    if(gpa>sizeof(ram)||len>sizeof(ram)-gpa)return false;
    memcpy(ram+gpa,in,len);return true;
}
static struct lagfx_device mock_dev={.desc.shell.read_memory=mock_read,.desc.shell.write_memory=mock_write};
static void put32(uint8_t *p,uint32_t v){memcpy(p,&v,4);}
static uint32_t get32(const uint8_t *p){uint32_t v;memcpy(&v,p,4);return v;}
static void packet(uint8_t *p,uint32_t op,const uint8_t *data,uint32_t n){
    put32(p,op);put32(p+4,12+n);put32(p+8,1);memcpy(p+12,data,n);
}
static int root_send(struct lagfx_protocol *p,uint32_t op,const uint8_t *data,uint32_t n){
    memset(ram,0,sizeof(ram));
    packet(ram+0x1000,op,data,n);
    p->magic=LAGFX_PROTOCOL_MAGIC;p->dev=&mock_dev;
    p->ring_armed=true;p->ring_base_gpa=0x1000;p->ring_size=4096;
    p->read_ptr=0;p->write_ptr=12+n;p->ring_shared_page_pfn=0;
    return channel_0_dispatcher_drain(p)==1 && p->read_ptr==12+n;
}
static int compute_send(struct lagfx_protocol *p,uint32_t op,const uint8_t *data,uint32_t n){
    memset(ram,0,sizeof(ram));
    uint8_t *desc=ram+0x1000+LAGFX_RING_FIFO_DESC_BASE_OFFSET;
    put32(desc,12+n);put32(desc+0x0c,1);put32(desc+0x10,2);
    put32(ram+0x2000,3);packet(ram+0x3000,op,data,n);
    p->magic=LAGFX_PROTOCOL_MAGIC;p->dev=&mock_dev;p->current_chan_id=1;p->ring_shared_page_pfn=1;
    return channel_compute_dispatcher_drain(p,1)==1 && get32(desc+4)==12+n;
}
'''+text[b:]
        # Retain other upstream subtests; replace only obsolete references.
        text=text.replace('p->dev = &mock_dev;', 'p->dev = &mock_dev; p->magic=LAGFX_PROTOCOL_MAGIC;')
        def replace_test(name,body):
            a=text.index('static int '+name+'(void)')
            b=text.find('\nstatic int ',a+1)
            if b<0:b=text.index('\nint main(void)',a)
            return text[:a]+body+'\n'+text[b:]
        if src=='handler-memory.c':
            text=replace_test('test_define_child_fifo',r'''
static int test_define_child_fifo(void){
    struct lagfx_protocol *p=calloc(1,sizeof(*p));if(!p)return 1;
    uint8_t data[4];put32(data,3);
    int ok=root_send(p,0x30,data,4);
    lagfx_fifo_entry_t *f=lagfx_protocol_find_fifo(p,3);
    ok=ok&&f&&f->live&&f->id==3;
    ok=ok&&root_send(p,0x30,data,4)&&lagfx_protocol_find_fifo(p,3)==f;
    unsigned live=0;for(unsigned i=0;i<LAGFX_MAX_FIFOS;i++)live+=p->fifos[i].live;
    ok=ok&&live==1;
    put32(data,4);ok=ok&&root_send(p,0x30,data,2)&&!lagfx_protocol_find_fifo(p,4);
    ok=ok&&root_send(p,0x04,data,4)&&!lagfx_protocol_find_fifo(p,4);
    printf("%s: live root 0x30 registers/deduplicates FIFO; short payload rejected; legacy 0x04 log-ack only (geometry not tested)\n",ok?"PASS":"FAIL");
    free(p);return !ok;
}
''')
            text=replace_test('test_map_memory_immediate',r'''
static int test_map_memory_immediate(void){
    struct lagfx_protocol *p=calloc(1,sizeof(*p));if(!p)return 1;
    uint8_t data[20]={0};put32(data,1);
    uint64_t gpa=0x123456000ull,sz=0x500000;
    memcpy(data+4,&gpa,8);memcpy(data+12,&sz,8);
    int ok=compute_send(p,0x39,data,19)&&p->big_maps_n==0;
    ok=ok&&compute_send(p,0x39,data,20)&&p->big_maps_n==1&&p->big_maps[0].gpa==gpa&&p->big_maps[0].size==sz;
    ok=ok&&compute_send(p,0x39,data,20)&&p->big_maps_n==1;
    gpa+=0x100000;sz=4096;memcpy(data+4,&gpa,8);memcpy(data+12,&sz,8);
    ok=ok&&compute_send(p,0x39,data,20)&&p->big_maps_n==1;
    printf("%s: live compute 0x39 large-map tracking, dedup, short and small input (not actual mapping)\n",ok?"PASS":"FAIL");
    free(p);return !ok;
}
''')
            text=replace_test('test_unmap_memory',r'''
static int test_unmap_memory(void){
    struct lagfx_protocol *p=calloc(1,sizeof(*p));if(!p)return 1;
    p->magic=LAGFX_PROTOCOL_MAGIC;p->dev=&mock_dev;
    uint8_t data[20]={0};uint64_t off=0x2000,len=0x1000;
    memcpy(data+4,&off,8);memcpy(data+12,&len,8);
    lagfx_cmd_header_t h={.opcode=3,.length=16,.payload_size=4,.payload=data};
    int ok=lagfx_memory_unmap_memory(p,&h)==LAGFX_HANDLER_ERR_SIZE;
    h.length=32;h.payload_size=20;
    ok=ok&&lagfx_memory_unmap_memory(p,&h)==LAGFX_HANDLER_OK;
    printf("%s: unmap rejects short payload and accepts packed 20-byte input (no shell callback)\n",ok?"PASS":"FAIL");
    free(p);return !ok;
}
''')
        if src=='handler-task.c':
            text=text.replace('((uint64_t*)payload)[1] = 0x1000ull;', 'memcpy(payload+4, &(uint64_t){0x1000ull}, 8);').replace('((uint64_t*)payload)[2] = 0x1000ull;', 'memcpy(payload+12, &(uint64_t){0x1000ull}, 8);')
            text=text.replace('int rc = lagfx_task_define_task2(p, &hdr);','int rc = lagfx_task_define_task2(p, &hdr); lagfx_task_entry_t *t=lagfx_protocol_find_task(p,1); if(!t||!t->live||t->base_va!=0x1000||t->length!=0x1000)rc=LAGFX_HANDLER_ERR_STATE;')
            text=text.replace('lagfx_task_define_task2(p, &def_hdr);','if(lagfx_task_define_task2(p, &def_hdr)!=LAGFX_HANDLER_OK||!lagfx_protocol_find_task(p,5)){free(p);return 1;}')
            text=text.replace('int rc = lagfx_task_delete_task(p, &del_hdr);','int rc = lagfx_task_delete_task(p, &del_hdr); if(lagfx_protocol_find_task(p,5))rc=LAGFX_HANDLER_ERR_STATE;')
            text=replace_test('test_define_host_task2',r'''
static int test_define_host_task2(void){
    struct lagfx_protocol *p=calloc(1,sizeof(*p));if(!p)return 1;
    uint8_t data[16]={0};put32(data,2);put32(data+12,0x30000);
    int ok=root_send(p,0x38,data,16);
    lagfx_task_entry_t *t=lagfx_protocol_find_task(p,1);
    ok=ok&&t&&t->live&&t->root_page_pfn==0x30000;
    put32(data+12,0x40000);
    ok=ok&&root_send(p,0x38,data,16)&&lagfx_protocol_find_task(p,1)==t&&t->root_page_pfn==0x40000;
    put32(data,4);ok=ok&&root_send(p,0x38,data,12)&&!lagfx_protocol_find_task(p,2);
    printf("%s: live root 0x38 registers task slot/PFN, updates and rejects short payload\n",ok?"PASS":"FAIL");
    free(p);return !ok;
}
''')
        if src=='handler-sync-utility.c':
            text=replace_test('test_util_device_info',r'''
static int test_util_device_info(void){
    struct lagfx_protocol *p=calloc(1,sizeof(*p));if(!p)return 1;
    p->magic=LAGFX_PROTOCOL_MAGIC;p->dev=&mock_dev;
    uint8_t data[16]={0};put32(data,1);put32(data+4,0x4000);
    memset(ram+0x4000,0xa5,8);
    lagfx_cmd_header_t h={.opcode=0x0a,.length=16,.payload_size=4,.payload=data};
    int ok=lagfx_op_get_device_info(p,&h)==LAGFX_HANDLER_ERR_SIZE&&get32(ram+0x4000)==0xa5a5a5a5u;
    h.length=28;h.payload_size=16;
    ok=ok&&lagfx_op_get_device_info(p,&h)==LAGFX_HANDLER_OK&&get32(ram+0x4000)==LAGFX_MAX_TASKS&&get32(ram+0x4004)==0xa5a5a5a5u;
    printf("%s: legacy DeviceInfo packed 16-byte query, DMA reply and short-payload rejection\n",ok?"PASS":"FAIL");
    free(p);return !ok;
}
''')
            text=replace_test('test_resource_set_placement',r'''
static int test_resource_set_placement(void){
    struct lagfx_protocol *p=calloc(1,sizeof(*p));if(!p)return 1;
    p->tasks[0].live=true;p->tasks[0].id=7;
    uint8_t data[8];put32(data,1);put32(data+4,0x1234);
    int ok=compute_send(p,0x25,data,8);
    ok=ok&&p->tasks[0].active_objects.count==1&&p->tasks[0].active_objects.object_ids[0]==0x1234;
    ok=ok&&compute_send(p,0x25,data,8)&&p->tasks[0].active_objects.count==1;
    put32(data+4,0x5678);ok=ok&&compute_send(p,0x25,data,4)&&p->tasks[0].active_objects.count==1;
    ok=ok&&compute_send(p,0x25,data,8)&&p->tasks[0].active_objects.count==2&&p->tasks[0].active_objects.object_ids[1]==0x5678;
    printf("%s: live compute 0x25 active-object registration/dedup/short payload; Henge 0x44 placement tables not covered\n",ok?"PASS":"FAIL");
    free(p);return !ok;
}
''')
            text=text.replace('#include <stdbool.h>', '#include "handlers/iosurface/iosurface.h"\n#include <stdbool.h>')
            for old,new in [('lagfx_resource_iosurface_create','lagfx_iosurface_create_backing2'),('lagfx_resource_iosurface_lookup','lagfx_iosurface_lookup'),('lagfx_util_device_info','lagfx_op_get_device_info')]:
                text=text.replace(old+'(',new+'(')
            for old,new in [('0x22','0x42'),('0x29','0x0e'),('0x2a','0x0a')]:
                text=text.replace('.opcode = '+old+',','.opcode = '+new+',')
    if src=='handler-display-vchan.c':
        # Preserve negative short-payload checks and add real metadata assertions.
        a=text.index('static int test_cursor_show(void)')
        b=text.index('int main(void)',a)
        text=text[:a]+r'''
static int test_cursor_show(void) {
    struct lagfx_protocol *p = calloc(1,sizeof(*p));
    p->dev=&mock_dev;
    uint8_t payload[16]={0};
    vchan_store_le32(payload+4, ((uint32_t)20<<16)|10);
    vchan_store_le32(payload+8,1);
    vchan_store_le32(payload+12, ((uint32_t)3<<16)|2);
    lagfx_cmd_header_t h={.opcode=0x13,.length=28,.payload_size=4,.payload=payload};
    int short_ok=lagfx_display_cursor_show(p,&h)==LAGFX_HANDLER_ERR_SIZE;
    h.payload_size=16;
    int ok=lagfx_display_cursor_show(p,&h)==LAGFX_HANDLER_OK &&
        mock_dev.cursor_show.valid && mock_dev.cursor_show.x==10 &&
        mock_dev.cursor_show.y==20 && mock_dev.cursor_show.visible==1 &&
        mock_dev.cursor_show.hot_x==2 && mock_dev.cursor_show.hot_y==3;
    printf("%s: cursor show rejects short payload and preserves metadata\n",short_ok&&ok?"PASS":"FAIL");
    free(p);return !(short_ok&&ok);
}
static int test_cursor_glyph(void) {
    struct lagfx_protocol *p=calloc(1,sizeof(*p));
    p->dev=&mock_dev;
    uint8_t payload[32]={0};
    vchan_store_le32(payload+4,32);vchan_store_le32(payload+8,32);
    vchan_store_le32(payload+12,128);vchan_store_le32(payload+16,2);
    vchan_store_le32(payload+20,3);vchan_store_le32(payload+24,0x10000);
    lagfx_cmd_header_t h={.opcode=0x14,.length=44,.payload_size=4,.payload=payload};
    int short_ok=lagfx_display_cursor_glyph(p,&h)==LAGFX_HANDLER_ERR_SIZE;
    h.payload_size=32;
    int ok=lagfx_display_cursor_glyph(p,&h)==LAGFX_HANDLER_OK &&
        mock_dev.cursor_glyph.valid && mock_dev.cursor_glyph.width==32 &&
        mock_dev.cursor_glyph.height==32 && mock_dev.cursor_glyph.bytes_per_row==128 &&
        mock_dev.cursor_glyph.hot_x==2 && mock_dev.cursor_glyph.hot_y==3 &&
        mock_dev.cursor_glyph.glyph_va==0x10000;
    printf("%s: cursor glyph rejects short payload and preserves metadata (pixel capture not tested)\n",short_ok&&ok?"PASS":"FAIL");
    free(p);return !(short_ok&&ok);
}
'''+text[b:]
    if src=='handler-display-vchan.c':
        text=text.replace('display_define_child_fifo allocates ring','display_define_child_fifo acknowledges command (geometry not implemented)')
    if src=='triangle-lavapipe-e2e.c':
        text=text.replace('lavapipe readback:', 'Vulkan device readback:')
    dest=adapted/src;dest.write_text(text)
    return dest
def build(entry):
    name,src,registered=entry
    out='libut_'+name.replace('-','_')+'.so'
    group='MESON_TARGET' if registered else 'EXTRA_SOURCE'
    if name in manual and name!='translator-microbench':
        return (name,group,'NOT_RUN','-',manual[name])
    args=[cc,'-std=c11','-O2','-fPIE','-pie','-D_GNU_SOURCE','-DLAGFX_HAVE_VULKAN=1',
          '-DSRCDIR="tests"','-DLAGFX_TEST_FIXTURE_DIR="tests/fixtures"',
          '-DLAGFX_HAVE_REAL_METALLIB=1','-DLAGFX_APPLE_SPV_BUILD_DIR="src/shaders/spv-apple"',
          '-I'+str(root/'include'),'-I'+str(root/'src'),'-I'+str(root/'android-build/src/shaders'),
          str(adapt(src)),'-I'+str(root/'tests'),str(archive),
          str(here/'vulkan_android_dispatch.c'),
          '-Wl,--wrap=vkCreateDevice','-Wl,--wrap=vkCmdBeginRendering','-Wl,--wrap=vkCmdEndRendering',
          '-Wl,--wrap=vkGetBufferDeviceAddress','-lvulkan','-lm','-o',str(native/out)]
    try:
        p=subprocess.run(args,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=90)
        (logs/(name+'.txt')).write_text(p.stdout)
        if p.returncode==0: return (name,group,'NOT_RUN' if name=='translator-microbench' else 'BUILT',out,'separate decoder benchmark' if name=='translator-microbench' else '-')
        (native/out).unlink(missing_ok=True)
        reason=next((x.strip() for x in p.stdout.splitlines() if 'error:' in x or 'undefined symbol' in x),'compiler exit '+str(p.returncode))
    except subprocess.TimeoutExpired:
        reason='compiler timeout'
        (native/out).unlink(missing_ok=True)
    return (name,group,'BUILD_FAIL','-',reason.replace('\t',' ')[:400])
with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool: results=list(pool.map(build,entries))
manifest='\n'.join('\t'.join(row) for row in results)+'\n'
(assets/'unit-manifest.tsv').write_text(manifest)
print(manifest)
print('INVENTORY',len(entries),'BUILT',sum(r[2]=='BUILT' for r in results),'BUILD_FAIL',sum(r[2]=='BUILD_FAIL' for r in results),'NOT_RUN',sum(r[2]=='NOT_RUN' for r in results))
assert any(r[2]=='BUILT' for r in results)
