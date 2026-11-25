import re, subprocess, pathlib
for name in ('login_colorfill_frag','login_texturecopy_frag'):
    dis=subprocess.check_output(['spirv-dis','/tmp/corpus/'+name+'/0.spv'],text=True)
    types={}
    for line in dis.splitlines():
        m=re.search(r'(%\S+) = OpTypeVector (%\S+) ([234])$',line)
        if m: types[m[1]]='vec'+m[3]
        m=re.search(r'(%\S+) = OpTypeFloat 32$',line)
        if m: types[m[1]]='float'
    ptr=dict(re.findall(r'(%\S+) = OpTypePointer Input (%\S+)',dis))
    variables=dict(re.findall(r'(%\S+) = OpVariable (%\S+) Input',dis))
    outs=[]; stores=[]
    for var,loc in re.findall(r'OpDecorate (%\S+) Location (\d+)',dis):
        if var not in variables: continue
        typ=types[ptr[variables[var]]]
        outs.append(f'layout(location={loc}) out {typ} v{loc};')
        stores.append(f'v{loc}={typ}(0.5);')
    src='#version 450\n'+'\n'.join(outs)+'\nvoid main(){vec2 p=vec2((gl_VertexIndex<<1)&2,gl_VertexIndex&2);gl_Position=vec4(p*2.0-1.0,0,1);' + ''.join(stores)+'}\n'
    pathlib.Path('/tmp/'+name+'.vert').write_text(src)
    print(name,src)
