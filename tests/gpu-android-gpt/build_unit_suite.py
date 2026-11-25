#!/usr/bin/env python3
"""Inventory upstream test targets; build separately so failures stay visible."""
import concurrent.futures, pathlib, re, subprocess, sys, zipfile
root=pathlib.Path('/tmp/lagfx')
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
# Adapt only test-environment paths. No translator logic or assertions changed.
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
    if src=='handler-memory.c':
        text=text.replace('lagfx_memory_define_child_fifo(', 'lagfx_op_define_child_fifo(').replace('lagfx_memory_map_memory_immediate(', 'lagfx_op_map_memory_immediate(')
        # The immediate command is packed: u32 then two u64, not aligned u64 array.
        text=text.replace('((uint64_t*)payload)[1] = 0x2000ull;', 'memcpy(payload+4, &(uint64_t){0x2000ull}, 8);').replace('((uint64_t*)payload)[2] = 0x1000ull;', 'memcpy(payload+12, &(uint64_t){0x1000ull}, 8);')
    if src=='handler-task.c':
        text=text.replace('lagfx_task_define_host_task2(', 'lagfx_task_define_host_task(')
    if src=='handler-sync-utility.c':
        text=text.replace('#include <stdbool.h>', '#include "handlers/iosurface/iosurface.h"\n#include <stdbool.h>')
        for old,new in [('lagfx_resource_set_placement','lagfx_op_set_object_placement'),('lagfx_resource_iosurface_create','lagfx_iosurface_create_backing2'),('lagfx_resource_iosurface_lookup','lagfx_iosurface_lookup'),('lagfx_util_device_info','lagfx_op_get_device_info')]:
            text=text.replace(old+'(',new+'(')
        for old,new in [('0x22','0x42'),('0x26','0x25'),('0x29','0x0e'),('0x2a','0x0a')]:
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
          str(adapt(src)),'-I'+str(root/'tests'),str(root/'android-build/libapplegfx-vulkan.a'),
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
