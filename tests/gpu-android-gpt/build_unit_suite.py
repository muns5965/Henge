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
# Keep original fixture names only inside an opaque ZIP asset; restore at runtime.
with zipfile.ZipFile(assets/'unit-fixtures.zip','w',zipfile.ZIP_DEFLATED) as z:
    for rel in ('tests/fixtures','examples/triangle/out','src/shaders/spv-apple'):
        for p in sorted((root/rel).rglob('*')):
            if p.is_file(): z.write(p,p.relative_to(root))
    for p in sorted(pathlib.Path('/tmp/gpu-assets').glob('triangle_*.spv')):
        z.write(p,'unit-triangle/'+p.name)
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
          str(root/'tests'/src),str(root/'android-build/libapplegfx-vulkan.a'),
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
