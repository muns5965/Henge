package dev.muns.hengegputest;

import android.app.Activity;
import android.os.Bundle;
import android.graphics.Bitmap;
import android.graphics.Color;
import android.widget.*;
import android.view.View;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import java.io.*;
import java.util.concurrent.*;
import java.util.Locale;

public class MainActivity extends Activity {
    private TextView log;
    private final ImageView[] images=new ImageView[6];
    private final TextView[] outcomes=new TextView[6];
    private Button run, corpus, render, units, compare;
    private final ImageView[] compareImages=new ImageView[3];
    private final TextView[] compareStatus=new TextView[3];
    private final ImageView[] metalImages=new ImageView[2];
    private final TextView[] metalStatus=new TextView[2];
    private volatile Process child;
    private final ExecutorService worker = Executors.newSingleThreadExecutor();
    private volatile String report = "";
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(20,20,20,20);
        LinearLayout screen = new LinearLayout(this);
        screen.setOrientation(LinearLayout.VERTICAL);
        Button topCopy = new Button(this);
        topCopy.setText("시험 로그 복사");
        topCopy.setOnClickListener(v -> copyReport());
        screen.addView(topCopy, new LinearLayout.LayoutParams(-1,-2));
        ScrollView scroll = new ScrollView(this);
        scroll.addView(root);
        screen.addView(scroll, new LinearLayout.LayoutParams(-1,0,1f));
        setContentView(screen);
        TextView heading = new TextView(this); heading.setText("GPT Metal 5.6.1 · upstream test suite"); heading.setTextSize(22); root.addView(heading);
        TextView info = new TextView(this); info.setText("독립 GPU 시험 · Henge VM 데이터 접근 없음\n1단계: 실제 AIR 변환 셰이더. 2~4단계: GLSL 기준 셰이더로 Vulkan 기능 독립 검증. 5단계: 기기 내부 AIR 변환. 6단계: 외부 엔진 clear/readback. PVG 연동 시험은 아닙니다."); root.addView(info);
        run = new Button(this); run.setText("6단계 전체 GPU 시험 시작"); root.addView(run);
        corpus=new Button(this);corpus.setText("macOS 로그인 Metal 셰이더 11종 변환 시험");root.addView(corpus);
        corpus.setOnClickListener(v->{run.setEnabled(false);if(compare!=null)compare.setEnabled(false);if(units!=null)units.setEnabled(false);corpus.setEnabled(false);if(render!=null)render.setEnabled(false);log.setText("AIR corpus 실행 중");worker.submit(this::testCorpus);});
        units=new Button(this);units.setText("5.6.1 라이브러리 전체 단위시험");root.addView(units);
        units.setOnClickListener(v->{run.setEnabled(false);if(compare!=null)compare.setEnabled(false);if(units!=null)units.setEnabled(false);corpus.setEnabled(false);render.setEnabled(false);units.setEnabled(false);log.setText("단위시험 준비 중");worker.submit(this::testUnits);});
        compare=new Button(this);compare.setText("O0 / O3 / LTO · AIR 11종 정밀 비교");root.addView(compare);
        LinearLayout previews=new LinearLayout(this);previews.setOrientation(LinearLayout.HORIZONTAL);root.addView(previews);
        String[] labels={"Debug O0","Release O3","Release + LTO"};
        for(int i=0;i<3;i++){
            LinearLayout cell=new LinearLayout(this);cell.setOrientation(LinearLayout.VERTICAL);
            previews.addView(cell,new LinearLayout.LayoutParams(0,-2,1f));
            TextView title=new TextView(this);title.setText(labels[i]);cell.addView(title);
            ImageView im=new ImageView(this);im.setBackgroundColor(Color.DKGRAY);compareImages[i]=im;
            cell.addView(im,new LinearLayout.LayoutParams(-1,(int)(112*getResources().getDisplayMetrics().density)));
            compareStatus[i]=new TextView(this);compareStatus[i].setText("미실행");cell.addView(compareStatus[i]);
        }
        compare.setOnClickListener(v->{run.setEnabled(false);if(compare!=null)compare.setEnabled(false);units.setEnabled(false);corpus.setEnabled(false);render.setEnabled(false);compare.setEnabled(false);worker.submit(this::testCompare);});
        String[] titles={"1 · AIR 삼각형","2 · 텍스처 샘플링","3 · 알파 블렌딩","4 · RGBA16Float 및 공통 픽셀 변환","5 · 기기 내부 AIR 변환 및 그리기","6 · 외부 엔진 렌더 타겟 및 읽기"};
        for(int i=0;i<6;i++){
            TextView title=new TextView(this);title.setText(titles[i]);title.setTextSize(18);root.addView(title);
            ImageView im=new ImageView(this);im.setAdjustViewBounds(true);im.setBackgroundColor(Color.DKGRAY);images[i]=im;
            root.addView(im,new LinearLayout.LayoutParams(-1,(int)(240*getResources().getDisplayMetrics().density)));
            outcomes[i]=new TextView(this);outcomes[i].setText("미실행");root.addView(outcomes[i]);
        }
        render=new Button(this);render.setText("실제 Metal ColorFill / TextureCopy GPU 출력 시험");root.addView(render);
        for(int i=0;i<2;i++){
            TextView title=new TextView(this);title.setText(i==0?"7 · 실제 Metal ColorFill":"8 · 실제 Metal TextureCopy");root.addView(title);
            ImageView im=new ImageView(this);im.setBackgroundColor(Color.DKGRAY);im.setAdjustViewBounds(true);metalImages[i]=im;
            root.addView(im,new LinearLayout.LayoutParams(-1,(int)(240*getResources().getDisplayMetrics().density)));
            metalStatus[i]=new TextView(this);metalStatus[i].setText("미실행");root.addView(metalStatus[i]);
        }
        render.setOnClickListener(v->{run.setEnabled(false);if(compare!=null)compare.setEnabled(false);if(units!=null)units.setEnabled(false);corpus.setEnabled(false);if(render!=null)render.setEnabled(false);render.setEnabled(false);worker.submit(this::testMetalRender);});
        log = new TextView(this); log.setTextIsSelectable(true); root.addView(log);
        Button copy = new Button(this); copy.setText("시험 로그 복사"); root.addView(copy);
        copy.setOnClickListener(v -> copyReport());
        run.setOnClickListener(v -> {run.setEnabled(false);if(compare!=null)compare.setEnabled(false);if(units!=null)units.setEnabled(false);corpus.setEnabled(false);if(render!=null)render.setEnabled(false); for(int i=0;i<6;i++){images[i].setImageDrawable(null);outcomes[i].setText("대기");} log.setText("실행 중…"); worker.submit(this::test);});
    }
    private void testCompare(){
        StringBuilder t=new StringBuilder("5.6.1 BUILD COMPARISON — identical AIR, real GPU readback; elapsed render includes process/setup/readback, not GPU timestamp or FPS\n");
        String[] modes={"debug","release","lto"};
        int passed=0;String baseline=null;
        java.util.Map<String,String> corpusBaseline=new java.util.HashMap<>();
        try(BufferedReader br=new BufferedReader(new InputStreamReader(getAssets().open("build-flags.txt")))){
            String line;while((line=br.readLine())!=null)t.append(line).append('\n');
        }catch(Exception e){t.append("FLAGS ERROR ").append(e).append('\n');}
        for(int i=0;i<3;i++){
            final int idx=i;Bitmap preview=null;boolean ok=false;String label="FAIL / incomplete";
            runOnUiThread(()->{compareImages[idx].setImageDrawable(null);compareStatus[idx].setText("실행 중");});
            try{
                File nativeDir=new File(getApplicationInfo().nativeLibraryDir);
                File dir=new File(getFilesDir(),"compare-"+modes[i]);if(!dir.isDirectory()&&!dir.mkdirs())throw new IOException("mkdir");
                File air=asset("triangle.air"),v=new File(dir,"triangle_vertex.spv"),f=new File(dir,"triangle_fragment.spv");
                double[] translate=new double[7],renderMs=new double[7];
                String digest=null;boolean same=true;
                for(int rep=-1;rep<7;rep++){
                    for(File old:new File[]{v,f,new File(dir,"module_path.spv")})if(old.exists()&&!old.delete())throw new IOException("stale SPIR-V");
                    long start=System.nanoTime();
                    int rc=auxiliary(new String[]{new File(nativeDir,"libcompare_"+modes[i]+".so").getAbsolutePath(),air.getAbsolutePath(),dir.getAbsolutePath(),"triangle_vertex:vertex","triangle_fragment:fragment"},t);
                    double tr=(System.nanoTime()-start)/1e6;
                    if(rc!=0||!v.isFile()||!f.isFile())throw new IOException("translation failed "+rc);
                    for(File spv:new File[]{v,f})if(auxiliary(new String[]{new File(nativeDir,"libspirvval.so").getAbsolutePath(),"--target-env","vulkan1.2",spv.getAbsolutePath()},t)!=0)throw new IOException("SPIR-V validation failed");
                    File pixels=new File(dir,"positive.bgra");int off=t.length();start=System.nanoTime();
                    int pr=execute(5,new File(nativeDir,"libgputest.so"),v,f,pixels,false,t);
                    double rm=(System.nanoTime()-start)/1e6;
                    if(pr!=0||!t.substring(off).contains("A5 RESULT: PASS")||readPixels(pixels)==null)throw new IOException("GPU positive failed");
                    String h=hash(v)+":"+hash(f)+":"+hash(pixels);
                    if(digest!=null&&!digest.equals(h))same=false;digest=h;
                    preview=readPixels(pixels);
                    if(rep>=0){translate[rep]=tr;renderMs[rep]=rm;}
                }
                int off=t.length();int neg=execute(5,new File(nativeDir,"libgputest.so"),v,f,new File(dir,"negative.bgra"),true,t);
                boolean negative=neg==1&&t.substring(off).contains("A5 RESULT: FAIL")&&t.substring(off).contains("A4 RESULT: PASS");
                boolean host=hash(v).equals(hash(asset("triangle_vertex.spv")))&&hash(f).equals(hash(asset("triangle_fragment.spv")));
                if(i==0)baseline=digest;
                boolean equal=baseline!=null&&baseline.equals(digest);
                ok=same&&negative&&host&&equal;
                java.util.Arrays.sort(translate);java.util.Arrays.sort(renderMs);
                t.append(String.format(Locale.ROOT,"COMPARE %s translation_process_ms median=%.3f max=%.3f render_process_ms median=%.3f max=%.3f repeat_equal=%b baseline_equal=%b host_equal=%b negative=%b%n",modes[i],translate[3],translate[6],renderMs[3],renderMs[6],same,equal,host,negative));
                int functions=0;boolean corpusOk=true;double sumWarm=0;
                try(BufferedReader manifest=new BufferedReader(new InputStreamReader(getAssets().open("manifest.txt")))){
                    String name;while((name=manifest.readLine())!=null){
                        if(!name.matches("[a-z0-9_]+"))throw new IOException("bad fixture");
                        File cd=new File(dir,name);if(!cd.isDirectory()&&!cd.mkdirs())throw new IOException("mkdir");
                        for(File old:cd.listFiles())if(!old.delete())throw new IOException("stale");
                        t.append("\nCASE ").append(name).append(" MODE ").append(modes[i]).append('\n');
                        int begin=t.length();
                        int rc=auxiliary(new String[]{new File(nativeDir,"libbench_"+modes[i]+".so").getAbsolutePath(),asset(name+".airdata").getAbsolutePath(),name.endsWith("_vert")?"vertex":"fragment",cd.getAbsolutePath(),name.equals("login_xgc_frag")?"10":"100"},t);
                        corpusOk &= rc==0;
                        java.util.regex.Matcher metric=java.util.regex.Pattern.compile("NATIVE_BENCH fn=\\d+ first_ms=[0-9.]+ warm_n=\\d+ median_ms=([0-9.]+)").matcher(t.substring(begin));
                        int metrics=0;while(metric.find()){sumWarm+=Double.parseDouble(metric.group(1));metrics++;}
                        File[] outputs=cd.listFiles((d,n)->n.endsWith(".spv"));if(outputs==null||outputs.length==0){corpusOk=false;continue;}
                        corpusOk &= metrics==outputs.length;
                        for(File spv:outputs){
                            functions++;String key=name+"/"+spv.getName(),sha=hash(spv);
                            if(i==0)corpusBaseline.put(key,sha);
                            boolean equalHash=sha.equals(corpusBaseline.get(key));
                            int vr=auxiliary(new String[]{new File(nativeDir,"libspirvval.so").getAbsolutePath(),"--target-env","vulkan1.2",spv.getAbsolutePath()},t);
                            corpusOk &= equalHash&&vr==0;
                            t.append("CORPUS_SHA ").append(key).append(' ').append(sha).append(" baseline_equal=").append(equalHash).append(" val=").append(vr).append('\n');
                        }
                    }
                }
                corpusOk &= functions==66&&corpusBaseline.size()==66;
                ok &= corpusOk;
                t.append(String.format(Locale.ROOT,"CORPUS_COMPARE %s functions=%d sum_function_medians_ms=%.6f timing_not_comparable result=%s%n",modes[i],functions,sumWarm,corpusOk?"PASS":"FAIL"));
                label=String.format(Locale.ROOT,"%s\n%d/66 함수\n합산 %.3f ms",ok?"PASS":"FAIL",functions,sumWarm);
            }catch(Exception e){t.append("COMPARE ERROR ").append(modes[i]).append(' ').append(e).append('\n');}
            if(ok)passed++;
            final Bitmap b=preview;final String status=label;
            runOnUiThread(()->{if(b!=null){android.graphics.drawable.BitmapDrawable d=new android.graphics.drawable.BitmapDrawable(getResources(),b);d.setFilterBitmap(false);compareImages[idx].setImageDrawable(d);}compareStatus[idx].setText(status);});
        }
        t.append("COMPARE OVERALL ").append(passed).append("/3 PASS\n");
        final String snapshot=t.toString();runOnUiThread(()->{report=snapshot;log.setText(snapshot);run.setEnabled(true);units.setEnabled(true);corpus.setEnabled(true);render.setEnabled(true);compare.setEnabled(true);});
    }
    private void copyReport() {
        String snapshot=report;
        if(snapshot.isEmpty()){
            Toast.makeText(this,"완료된 시험 로그가 없습니다.",Toast.LENGTH_SHORT).show();
            return;
        }
        ((ClipboardManager)getSystemService(Context.CLIPBOARD_SERVICE)).setPrimaryClip(ClipData.newPlainText("GPU test",snapshot));
        Toast.makeText(this,"최근 완료 시험 로그 복사 완료",Toast.LENGTH_SHORT).show();
    }
    private File asset(String name) throws IOException {
        File f = new File(getFilesDir(),name);
        try(InputStream in=getAssets().open(name); OutputStream out=new FileOutputStream(f)) {
            byte[] b=new byte[8192]; int n; while((n=in.read(b))!=-1) out.write(b,0,n);
        } return f;
    }
    private int execute(int stage, File exe, File v, File f, File output, boolean negative, StringBuilder text) throws Exception {
        if(output.exists()&&!output.delete()) throw new IOException("old output");
        ProcessBuilder pb=stage==6?new ProcessBuilder(exe.getAbsolutePath()):stage==1||stage==5?new ProcessBuilder(exe.getAbsolutePath(),v.getAbsolutePath(),f.getAbsolutePath()):new ProcessBuilder(exe.getAbsolutePath(),Integer.toString(stage),v.getAbsolutePath(),f.getAbsolutePath());
        pb.directory(getFilesDir()); pb.redirectErrorStream(true);
        File stdout=new File(getFilesDir(),negative?"negative.log":"positive.log");
        pb.redirectOutput(stdout);
        pb.environment().put("GPU_TEST_OUTPUT",output.getAbsolutePath());
        pb.environment().remove("HV_PVG_HALF");
        if(negative) pb.environment().put("GPU_TEST_NO_DRAW","1");
        else pb.environment().remove("GPU_TEST_NO_DRAW");
        child=pb.start();
        boolean done=child.waitFor(45,TimeUnit.SECONDS);
        if(!done){child.destroyForcibly(); child.waitFor(5,TimeUnit.SECONDS);}
        try(BufferedReader br=new BufferedReader(new FileReader(stdout))){String s;while((s=br.readLine())!=null)text.append(s).append('\n');}
        int rc=done?child.exitValue():124; child=null;
        text.append("exit=").append(rc).append('\n'); return rc;
    }
    private int auxiliary(String[] args,StringBuilder t) throws Exception {
        ProcessBuilder pb=new ProcessBuilder(args);pb.directory(getFilesDir());pb.redirectErrorStream(true);
        File out=new File(getFilesDir(),"aux.log");pb.redirectOutput(out);
        pb.environment().remove("GPU_TEST_NO_DRAW");
        child=pb.start();boolean done=child.waitFor(45,TimeUnit.SECONDS);
        if(!done){child.destroyForcibly();child.waitFor(5,TimeUnit.SECONDS);}
        try(BufferedReader r=new BufferedReader(new FileReader(out))){String line;while((line=r.readLine())!=null)t.append(line).append('\n');}
        int rc=done?child.exitValue():124;child=null;return rc;
    }
    private String hash(File file) throws Exception {
        java.security.MessageDigest md=java.security.MessageDigest.getInstance("SHA-256");
        try(InputStream in=new FileInputStream(file)){byte[] buf=new byte[8192];int n;while((n=in.read(buf))!=-1)md.update(buf,0,n);}
        StringBuilder h=new StringBuilder();for(byte b:md.digest())h.append(String.format(Locale.ROOT,"%02x",b&255));return h.toString();
    }
    private void testCorpus(){
        StringBuilder t=new StringBuilder();int passed=0,total=0;
        try{
            File host=asset("report.txt"),manifest=asset("manifest.txt");
            java.util.Map<String,String> expected=new java.util.HashMap<>();
            java.util.Set<String> valid=new java.util.HashSet<>();
            try(BufferedReader br=new BufferedReader(new FileReader(host))){String line;while((line=br.readLine())!=null){
                String[] p=line.trim().split("\\s+");
                if(p.length==2&&p[0].matches("[0-9a-f]{64}")&&p[1].startsWith("/tmp/corpus/"))expected.put(p[1].substring(12),p[0]);
                if(p.length==3&&p[0].equals("VALID")&&p[2].equals("PASS"))valid.add(p[1].substring(12));
            }}
            File nativeDir=new File(getApplicationInfo().nativeLibraryDir);
            try(BufferedReader br=new BufferedReader(new FileReader(manifest))){String name;while((name=br.readLine())!=null){
                if(!name.matches("[a-z0-9_]+"))throw new IOException("manifest invalid");
                total++;t.append("\nCASE ").append(name).append('\n');
                try{
                    File dir=new File(getFilesDir(),"corpus-"+name);if(!dir.isDirectory()&&!dir.mkdir())throw new IOException("mkdir");
                    File[] old=dir.listFiles();if(old==null)throw new IOException("list");
                    for(File f:old)if(!f.delete())throw new IOException("stale output");
                    File air=asset(name+".airdata");
                    t.append("AIR input SHA256: ").append(hash(air)).append('\n');
                    int start=t.length();int rc=auxiliary(new String[]{new File(nativeDir,"libairprobe.so").getAbsolutePath(),"--corpus",air.getAbsolutePath(),name.endsWith("_vert")?"vertex":"fragment",dir.getAbsolutePath()},t);
                    String result=t.substring(start);File[] files=dir.listFiles((d,n)->n.endsWith(".spv"));
                    boolean ok=rc==0&&files!=null&&files.length>0;int count=0;
                    if(files!=null)for(File f:files){
                        String key=name+"/"+f.getName();String h=hash(f);boolean match=h.equals(expected.get(key));boolean val=valid.contains(key);
                        t.append(f.getName()).append(" SHA256 ").append(h).append(" hostMatch=").append(match).append(" hostSpirvVal=").append(val).append('\n');ok&=match&&val;count++;
                        // Separate optimization evidence from the original translation result.
                        File opt=new File(dir,f.getName()+".optimized");
                        File optExe=new File(nativeDir,"libspirvopt.so"), valExe=new File(nativeDir,"libspirvval.so");
                        if(opt.exists()&&!opt.delete())throw new IOException("stale optimized output");
                        long started=System.nanoTime();
                        int optRc=auxiliary(new String[]{optExe.getAbsolutePath(),"--target-env=vulkan1.2","-O",f.getAbsolutePath(),"-o",opt.getAbsolutePath()},t);
                        double optMs=(System.nanoTime()-started)/1e6;
                        int valRc=optRc==0&&opt.isFile()?auxiliary(new String[]{valExe.getAbsolutePath(),"--target-env","vulkan1.2",opt.getAbsolutePath()},t):-1;
                        t.append(String.format(Locale.ROOT,"OPT_COMPARE %s original=%d optimized=%d wall_ms=%.3f opt_exit=%d val_exit=%d validation=%s; original retained; GPU semantics not tested%n",f.getName(),f.length(),opt.length(),optMs,optRc,valRc,valRc==0?"PASS":"FAIL"));

                    }
                    int hostCount=0;for(String key:expected.keySet())if(key.startsWith(name+"/"))hostCount++;
                    ok&=count==hostCount;
                    // Structural gates from upstream login corpus: position or image sample.
                    boolean role=name.endsWith("_vert")?result.matches("(?s).*position=[1-9][0-9]* .*"):name.equals("login_colorfill_frag")||result.matches("(?s).*image=[1-9][0-9]* sample=[1-9][0-9]* .*");
                    ok&=role;t.append("role opcode check=").append(role).append('\n');
                    if(ok)passed++;t.append("CASE RESULT: ").append(ok?"PASS":"FAIL / incomplete").append('\n');
                }catch(Exception e){t.append("ERROR: ").append(e).append('\n');}
                final String snapshot=t.toString();runOnUiThread(()->{report=snapshot;log.setText(snapshot);});
            }}
        }catch(Exception e){t.append("ERROR: ").append(e).append('\n');}
        t.append("\nCORPUS OVERALL: ").append(passed).append('/').append(total).append(" PASS; translation/structure only, no render claim\n");
        final String snapshot=t.toString();runOnUiThread(()->{report=snapshot;log.setText(snapshot);run.setEnabled(true);if(compare!=null)compare.setEnabled(true);if(units!=null)units.setEnabled(true);corpus.setEnabled(true);if(render!=null)render.setEnabled(true);});
    }
    private void testMetalRender(){
        StringBuilder t=new StringBuilder("Actual translated Metal fragments; GLSL reference vertex only.\n");
        int passed=0;
        for(int i=0;i<2;i++){
            final int idx=i;
            final String name=i==0?"login_colorfill_frag":"login_texturecopy_frag";
            Bitmap bitmap=null;boolean ok=false;
            runOnUiThread(()->{metalStatus[idx].setText("실행 중");metalImages[idx].setImageDrawable(null);});
            try{
                File nativeDir=new File(getApplicationInfo().nativeLibraryDir);
                File dir=new File(getFilesDir(),"render-"+name);if(!dir.isDirectory()&&!dir.mkdir())throw new IOException("mkdir");
                File[] olds=dir.listFiles();if(olds==null)throw new IOException("list");
                for(File old:olds)if(!old.delete())throw new IOException("stale output");
                File air=asset(name+".airdata");
                int tr=auxiliary(new String[]{new File(nativeDir,"libairprobe.so").getAbsolutePath(),"--corpus",air.getAbsolutePath(),"fragment",dir.getAbsolutePath()},t);
                File fs=new File(dir,"0.spv");if(tr!=0||!fs.isFile())throw new IOException("translation failed "+tr);
                File vs=asset(name+"-ref.spv");
                String h=hash(fs);boolean validated=false;
                try(BufferedReader br=new BufferedReader(new FileReader(asset("report.txt")))){
                    String line;boolean match=false,val=false;while((line=br.readLine())!=null){
                        String path="/tmp/corpus/"+name+"/0.spv";
                        if(line.equals(h+"  "+path))match=true;
                        if(line.equals("VALID "+path+" PASS"))val=true;
                    }validated=match&&val;
                }
                if(!validated)throw new IOException("fresh SPIR-V differs from validated host reference");
                String color=i==0?"00ff00ff":"4080ffff", mode=i==0?"--buf":"--tex";
                String[] args={new File(nativeDir,"libskylight.so").getAbsolutePath(),vs.getAbsolutePath(),fs.getAbsolutePath(),color,mode};
                File pixels=new File(dir,"positive.bgra"),negative=new File(dir,"negative.bgra");
                t.append("\nCASE ").append(name).append(" POSITIVE\n");int st=t.length();
                int p=renderProcess(args,pixels,false,t);String pos=t.substring(st);bitmap=readPixels(pixels);
                t.append("\nNEGATIVE (draw omitted)\n");st=t.length();
                int n=renderProcess(args,negative,true,t);String neg=t.substring(st);
                ok=p==0&&bitmap!=null&&pos.contains("SKYLIGHT pixels: 4096 checked, 0 mismatched")&&pos.contains("SKYLIGHT RESULT: PASS")&&n==1&&neg.contains("SKYLIGHT pixels: 4096 checked, 4096 mismatched")&&neg.contains("SKYLIGHT RESULT: FAIL");
            }catch(Exception e){t.append("ERROR: ").append(e).append('\n');}
            if(ok)passed++;t.append("CASE RESULT: ").append(ok?"PASS":"FAIL / incomplete").append('\n');
            final Bitmap b=bitmap;final boolean pass=ok;final String snapshot=t.toString();
            runOnUiThread(()->{if(b!=null){android.graphics.drawable.BitmapDrawable d=new android.graphics.drawable.BitmapDrawable(getResources(),b);d.setFilterBitmap(false);metalImages[idx].setImageDrawable(d);}metalStatus[idx].setText(pass?"PASS · 실제 GPU 픽셀":"FAIL / 미완료");report=snapshot;log.setText(snapshot);});
        }
        t.append("METAL RENDER OVERALL: ").append(passed).append("/2 PASS\n");
        final String snapshot=t.toString();runOnUiThread(()->{report=snapshot;log.setText(snapshot);run.setEnabled(true);if(compare!=null)compare.setEnabled(true);if(units!=null)units.setEnabled(true);corpus.setEnabled(true);render.setEnabled(true);});
    }
    private int renderProcess(String[] args,File out,boolean negative,StringBuilder t)throws Exception{
        if(out.exists()&&!out.delete())throw new IOException("stale pixels");
        ProcessBuilder pb=new ProcessBuilder(args);pb.directory(getFilesDir());pb.redirectErrorStream(true);
        File stdout=new File(getFilesDir(),"metal-render.log");pb.redirectOutput(stdout);
        pb.environment().put("GPU_TEST_OUTPUT",out.getAbsolutePath());pb.environment().remove("LAGFX_FORCE_LAVAPIPE");
        if(negative)pb.environment().put("GPU_TEST_NO_DRAW","1");else pb.environment().remove("GPU_TEST_NO_DRAW");
        child=pb.start();boolean done=child.waitFor(60,TimeUnit.SECONDS);
        if(!done){child.destroyForcibly();child.waitFor(5,TimeUnit.SECONDS);}
        try(BufferedReader br=new BufferedReader(new FileReader(stdout))){String line;while((line=br.readLine())!=null)t.append(line).append('\n');}
        int rc=done?child.exitValue():124;child=null;t.append("exit=").append(rc).append('\n');return rc;
    }
    private Bitmap readPixels(File pixels) throws IOException {
        if(pixels.length()!=64*64*4)return null;
        byte[] raw=new byte[64*64*4];try(DataInputStream in=new DataInputStream(new FileInputStream(pixels))){in.readFully(raw);}
        int[] argb=new int[64*64];
        for(int i=0;i<argb.length;i++)argb[i]=Color.argb(raw[i*4+3]&255,raw[i*4+2]&255,raw[i*4+1]&255,raw[i*4]&255);
        return Bitmap.createBitmap(argb,64,64,Bitmap.Config.ARGB_8888);
    }
    private void test() {
        StringBuilder t=new StringBuilder();int passes=0;
        try {
            File provenance=asset("provenance.txt");
            try(BufferedReader b=new BufferedReader(new FileReader(provenance))){String line;while((line=b.readLine())!=null)t.append(line).append('\n');}
            t.append("\nStages 2–4 use GLSL reference shaders, not translated Metal shaders.\n");
            for(int stage=1;stage<=6;stage++){
                final int idx=stage-1;Bitmap bitmap=null;boolean pass=false;
                runOnUiThread(()->outcomes[idx].setText("실행 중"));
                try {
                    File v,f;
                    File nativeDir=new File(getApplicationInfo().nativeLibraryDir);
                    if(stage==5){
                        File dir=new File(getFilesDir(),"fresh-air");if(!dir.isDirectory()&&!dir.mkdir())throw new IOException("mkdir");
                        v=new File(dir,"triangle_vertex.spv");f=new File(dir,"triangle_fragment.spv");
                        for(File old:new File[]{v,f,new File(dir,"module_path.spv")})if(old.exists()&&!old.delete())throw new IOException("stale output");
                        File air=asset("triangle.air");
                        int tr=auxiliary(new String[]{new File(nativeDir,"libairprobe.so").getAbsolutePath(),air.getAbsolutePath(),dir.getAbsolutePath(),"triangle_vertex:vertex","triangle_fragment:fragment"},t);
                        if(tr!=0||!v.isFile()||!f.isFile())throw new IOException("AIR translation failed: "+tr);
                        int st=t.length();int cmp=auxiliary(new String[]{new File(nativeDir,"libstubcompare.so").getAbsolutePath(),v.getAbsolutePath(),f.getAbsolutePath()},t);
                        String checks=t.substring(st);if(cmp!=0||checks.matches("(?s).*equals_[^=\\s]+\\s*=1.*")||!checks.contains("equals_position_stub=0")||!checks.contains("equals_render_target_stub=0"))throw new IOException("stub comparison failed");
                        String vh=hash(v),fh=hash(f);t.append("device AIR vertex SHA256: ").append(vh).append('\n').append("device AIR fragment SHA256: ").append(fh).append('\n');
                        if(!vh.equals(hash(asset("triangle_vertex.spv")))||!fh.equals(hash(asset("triangle_fragment.spv"))))throw new IOException("device output differs from host reference");
                        t.append("ON-DEVICE AIR TRANSLATION PASS (fresh outputs match host)\n");
                    }else if(stage==6){v=f=null;}
                    else{v=asset(stage==1?"triangle_vertex.spv":"full.spv");f=asset(new String[]{"triangle_fragment.spv","texture.spv","blend.spv","half.spv"}[idx]);}
                    File exe=new File(nativeDir,stage==6?"libenginetest.so":stage==1||stage==5?"libgputest.so":"libgpustages.so");
                    File pixels=new File(getFilesDir(),"stage"+stage+".bgra");
                    t.append("\nSTAGE ").append(stage).append(" POSITIVE\n");int posStart=t.length();
                    int p=execute(stage,exe,v,f,pixels,false,t);String positive=t.substring(posStart);
                    bitmap=readPixels(pixels);
                    t.append(stage==6?"\nNEGATIVE: clear changed to black\n":"\nNEGATIVE: draw omitted\n");int negStart=t.length();
                    int n=execute(stage,exe,v,f,new File(getFilesDir(),"negative"+stage+".bgra"),true,t);String neg=t.substring(negStart);
                    String marker=stage==6?"ENGINE RESULT: ":stage==1||stage==5?"A5 RESULT: ":"STAGE "+stage+" RESULT: ";
                    String pipe=stage==6?"ENGINE pixels: 4096 checked, 4096 mismatched":stage==1||stage==5?"A4 RESULT: PASS":"PIPELINE PASS";
                    pass=p==0&&bitmap!=null&&positive.contains(marker+"PASS")&&n==1&&neg.contains(marker+"FAIL")&&neg.contains(pipe);
                }catch(Exception e){t.append("ERROR: ").append(e).append('\n');}
                if(pass)passes++;
                t.append("STAGE ").append(stage).append(" OVERALL: ").append(pass?"PASS":"FAIL / incomplete").append('\n');
                final Bitmap b=bitmap;final boolean ok=pass;final String snapshot=t.toString();
                runOnUiThread(()->{report=snapshot;log.setText(snapshot);outcomes[idx].setText(ok?"PASS · 픽셀 비교 및 음성 대조군 통과":"FAIL / 미완료 · 로그 확인");if(b!=null){android.graphics.drawable.BitmapDrawable d=new android.graphics.drawable.BitmapDrawable(getResources(),b);d.setFilterBitmap(false);images[idx].setImageDrawable(d);}});
            }
        }catch(Exception e){t.append("ERROR: ").append(e).append('\n');}
        t.append("\nOVERALL: ").append(passes).append("/6 PASS\n");final String s=t.toString();
        runOnUiThread(()->{report=s;log.setText(s);run.setEnabled(true);if(compare!=null)compare.setEnabled(true);if(units!=null)units.setEnabled(true);corpus.setEnabled(true);if(render!=null)render.setEnabled(true);});
    }
    private void testUnits(){
        StringBuilder t=new StringBuilder("UPSTREAM TEST INVENTORY — 5.6.1\n");
        int pass=0,fail=0,skip=0,buildFail=0,notRun=0,total=0;
        try{
            File work=new File(getFilesDir(),"unit-work");if(!work.isDirectory()&&!work.mkdirs())throw new IOException("workdir");
            try(java.util.zip.ZipInputStream zip=new java.util.zip.ZipInputStream(getAssets().open("unit-fixtures.zip"))){
                java.util.zip.ZipEntry e;byte[] buf=new byte[8192];long size=0;
                while((e=zip.getNextEntry())!=null){
                    File target=new File(work,e.getName());if(!target.getCanonicalPath().startsWith(work.getCanonicalPath()+File.separator))throw new IOException("invalid fixture path");
                    if(e.isDirectory()){if(!target.isDirectory()&&!target.mkdirs())throw new IOException("mkdir");}
                    else {File parent=target.getParentFile();if(!parent.isDirectory()&&!parent.mkdirs())throw new IOException("mkdir");
                        try(FileOutputStream out=new FileOutputStream(target)){int n;while((n=zip.read(buf))!=-1){size+=n;if(size>64L*1024*1024)throw new IOException("fixture size limit");out.write(buf,0,n);}}
                    }zip.closeEntry();
                }
            }
            File nativeDir=new File(getApplicationInfo().nativeLibraryDir);
            try(BufferedReader br=new BufferedReader(new InputStreamReader(getAssets().open("unit-manifest.tsv")))){
                String line;while((line=br.readLine())!=null){
                    String[] p=line.split("\t",5);if(p.length!=5)throw new IOException("manifest row");
                    total++;String state=p[2],detail=p[4];
                    if(state.equals("BUILT")){
                        try{
                            if(!p[3].matches("libut_[a-zA-Z0-9_]+\\.so"))throw new IOException("bad executable");
                            File stdout=new File(work,"unit-"+total+".log");
                            ProcessBuilder pb=new ProcessBuilder(new File(nativeDir,p[3]).getAbsolutePath());
                            pb.directory(work);pb.redirectErrorStream(true);pb.redirectOutput(stdout);
                            pb.environment().put("LAGFX_TRIANGLE_SPV_DIR",new File(work,"unit-triangle").getAbsolutePath());
                            pb.environment().put("LAGFX_APPLE_SPV_DIR",new File(work,"src/shaders/spv-apple").getAbsolutePath());
                            pb.environment().put("TMPDIR",work.getAbsolutePath());
                            pb.environment().put("LAGFX_ANDROID_SPIRV_VAL",new File(nativeDir,"libspirvval.so").getAbsolutePath());
                            pb.environment().put("LAGFX_ANDROID_SPIRV_DIS",new File(nativeDir,"libspirvdis.so").getAbsolutePath());
                            pb.environment().remove("LAGFX_FORCE_LAVAPIPE");pb.environment().remove("VK_ICD_FILENAMES");
                            child=pb.start();boolean done=child.waitFor(30,TimeUnit.SECONDS);
                            if(!done){child.destroyForcibly();child.waitFor(5,TimeUnit.SECONDS);}
                            int rc=done?child.exitValue():124;child=null;
                            StringBuilder output=new StringBuilder();
                            try(BufferedReader lr=new BufferedReader(new FileReader(stdout))){String l;while((l=lr.readLine())!=null){if(output.length()<24000)output.append(l).append('\n');}}
                            String raw=output.toString();
                            boolean skipped=false;
                            for(String l:raw.split("\\R")){
                                String q=l.toLowerCase(Locale.ROOT).trim();
                                if(q.matches(".*\\b0 skipped\\b.*")||q.contains("zero skipped"))continue;
                                if(q.startsWith("skip:")||q.startsWith("skipped:")||q.contains("spirv-val unavailable")||q.contains("spirv-val not available")||q.contains("spirv-val=n/a")||q.contains("spirv-val skipped")||q.contains("tests skipped")||q.contains("— skipping")||q.matches(".*\\b[1-9][0-9]* skipped\\b.*")||q.contains("validation skipped"))skipped=true;
                            }
                            if(rc==77||(rc==0&&skipped)){state="SKIP";detail="exit="+rc+"; skip or partial-skip marker";}
                            else if(rc==0){state="PASS";detail="exit=0";}
                            else {state="FAIL";detail="exit="+rc;}
                            t.append("\n--- ").append(p[0]).append(" ---\n").append(raw);
                        }catch(Exception e){state="FAIL";detail=e.toString();if(child!=null){child.destroyForcibly();child=null;}}
                    }
                    switch(state){case "PASS":pass++;break;case "FAIL":fail++;break;case "SKIP":skip++;break;case "BUILD_FAIL":buildFail++;break;default:notRun++;}
                    t.append("UNIT RESULT\t").append(p[0]).append('\t').append(p[1]).append('\t').append(state).append('\t').append(detail).append('\n');
                    final String status="시험 "+total+" · PASS "+pass+" · FAIL "+fail+" · SKIP "+skip+" · BUILD_FAIL "+buildFail+" · NOT_RUN "+notRun;
                    runOnUiThread(()->log.setText(status));
                }
            }
        }catch(Exception e){t.append("SUITE ERROR: ").append(e).append('\n');}
        try{
            t.append("\n--- protocol microbenchmark (decoder only; not FPS) ---\n");
            File exe=new File(getApplicationInfo().nativeLibraryDir,"libut_translator_microbench.so");
            if(exe.isFile())t.append("BENCH exit=").append(auxiliary(new String[]{exe.getAbsolutePath(),"10000","--no-display"},t)).append('\n');
            else t.append("BENCH unavailable (see build manifest)\n");
        }catch(Exception e){t.append("BENCH ERROR ").append(e).append('\n');}
        try{
            t.append("\n--- memory correctness (MOCK, no real Vulkan driver) ---\n");
            File exe=new File(getApplicationInfo().nativeLibraryDir,"libmemorymock.so");
            int rc=auxiliary(new String[]{exe.getAbsolutePath()},t);
            t.append("MEMORY MOCK RESULT ").append(rc==0?"PASS":"FAIL").append(" exit=").append(rc).append('\n');
            t.append("\n--- patched library real Vulkan render smoke ---\n");
            rc=auxiliary(new String[]{new File(getApplicationInfo().nativeLibraryDir,"libfixedsmoke.so").getAbsolutePath()},t);
            t.append("PATCHED SMOKE exit=").append(rc).append("; inspect PASS/SKIP markers above\n");
        }catch(Exception e){t.append("MEMORY TEST ERROR ").append(e).append('\n');}
        t.append("\nUNIT SUMMARY total=").append(total).append(" PASS=").append(pass).append(" FAIL=").append(fail).append(" SKIP=").append(skip).append(" BUILD_FAIL=").append(buildFail).append(" NOT_RUN=").append(notRun).append('\n');
        t.append("Scope: original standalone tests; PASS is not full Metal API or GPU semantic coverage.\n");
        final String snapshot=t.toString();
        runOnUiThread(()->{report=snapshot;log.setText(snapshot);run.setEnabled(true);if(compare!=null)compare.setEnabled(true);corpus.setEnabled(true);render.setEnabled(true);units.setEnabled(true);});
    }
    @Override protected void onDestroy(){Process p=child;if(p!=null)p.destroyForcibly();worker.shutdownNow();super.onDestroy();}
}
