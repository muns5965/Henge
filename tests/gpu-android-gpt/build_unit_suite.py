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
    dest=adapted/src;dest.write_text(text)
    return dest
def build(entry):
    name,src,registered=entry
    out='libut_'+name.replace('-','_')+'.so'
    group='MESON_TARGET' if registered else 'EXTRA_SOURCE'
    if name in manual:
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
        if p.returncode==0: return (name,group,'BUILT',out,'-')
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
