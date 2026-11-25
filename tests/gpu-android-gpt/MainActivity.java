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
    private final ImageView[] images=new ImageView[4];
    private final TextView[] outcomes=new TextView[4];
    private Button run;
    private volatile Process child;
    private final ExecutorService worker = Executors.newSingleThreadExecutor();
    private volatile String report = "";
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(20,20,20,20);
        ScrollView scroll = new ScrollView(this); scroll.addView(root); setContentView(scroll);
        TextView heading = new TextView(this); heading.setText("GPT Vulkan GPU Test · 4 stages"); heading.setTextSize(22); root.addView(heading);
        TextView info = new TextView(this); info.setText("독립 GPU 시험 · Henge VM 데이터 접근 없음\n1단계: 실제 AIR 변환 셰이더. 2~4단계: GLSL 기준 셰이더로 Vulkan 기능 독립 검증. PVG 연동 시험은 아닙니다."); root.addView(info);
        run = new Button(this); run.setText("4단계 전체 GPU 시험 시작"); root.addView(run);
        String[] titles={"1 · AIR 삼각형","2 · 텍스처 샘플링","3 · 알파 블렌딩","4 · RGBA16Float 및 공통 픽셀 변환"};
        for(int i=0;i<4;i++){
            TextView title=new TextView(this);title.setText(titles[i]);title.setTextSize(18);root.addView(title);
            ImageView im=new ImageView(this);im.setAdjustViewBounds(true);im.setBackgroundColor(Color.DKGRAY);images[i]=im;
            root.addView(im,new LinearLayout.LayoutParams(-1,(int)(240*getResources().getDisplayMetrics().density)));
            outcomes[i]=new TextView(this);outcomes[i].setText("미실행");root.addView(outcomes[i]);
        }
        log = new TextView(this); log.setTextIsSelectable(true); root.addView(log);
        Button copy = new Button(this); copy.setText("시험 로그 복사"); root.addView(copy);
        copy.setOnClickListener(v -> ((ClipboardManager)getSystemService(Context.CLIPBOARD_SERVICE)).setPrimaryClip(ClipData.newPlainText("GPU test",report)));
        run.setOnClickListener(v -> {run.setEnabled(false); for(int i=0;i<4;i++){images[i].setImageDrawable(null);outcomes[i].setText("대기");} log.setText("실행 중…"); worker.submit(this::test);});
    }
    private File asset(String name) throws IOException {
        File f = new File(getFilesDir(),name);
        try(InputStream in=getAssets().open(name); OutputStream out=new FileOutputStream(f)) {
            byte[] b=new byte[8192]; int n; while((n=in.read(b))!=-1) out.write(b,0,n);
        } return f;
    }
    private int execute(int stage, File exe, File v, File f, File output, boolean negative, StringBuilder text) throws Exception {
        if(output.exists()&&!output.delete()) throw new IOException("old output");
        ProcessBuilder pb=stage==1?new ProcessBuilder(exe.getAbsolutePath(),v.getAbsolutePath(),f.getAbsolutePath()):new ProcessBuilder(exe.getAbsolutePath(),Integer.toString(stage),v.getAbsolutePath(),f.getAbsolutePath());
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
            for(int stage=1;stage<=4;stage++){
                final int idx=stage-1;Bitmap bitmap=null;boolean pass=false;
                runOnUiThread(()->outcomes[idx].setText("실행 중"));
                try {
                    File v=asset(stage==1?"triangle_vertex.spv":"full.spv");
                    File f=asset(new String[]{"triangle_fragment.spv","texture.spv","blend.spv","half.spv"}[idx]);
                    File exe=new File(getApplicationInfo().nativeLibraryDir,stage==1?"libgputest.so":"libgpustages.so");
                    File pixels=new File(getFilesDir(),"stage"+stage+".bgra");
                    t.append("\nSTAGE ").append(stage).append(" POSITIVE\n");int posStart=t.length();
                    int p=execute(stage,exe,v,f,pixels,false,t);String positive=t.substring(posStart);
                    bitmap=readPixels(pixels);
                    t.append("\nNEGATIVE: draw omitted\n");int negStart=t.length();
                    int n=execute(stage,exe,v,f,new File(getFilesDir(),"negative"+stage+".bgra"),true,t);String neg=t.substring(negStart);
                    String marker=stage==1?"A5 RESULT: ":"STAGE "+stage+" RESULT: ";
                    String pipe=stage==1?"A4 RESULT: PASS":"PIPELINE PASS";
                    pass=p==0&&bitmap!=null&&positive.contains(marker+"PASS")&&n==1&&neg.contains(marker+"FAIL")&&neg.contains(pipe);
                }catch(Exception e){t.append("ERROR: ").append(e).append('\n');}
                if(pass)passes++;
                t.append("STAGE ").append(stage).append(" OVERALL: ").append(pass?"PASS":"FAIL / incomplete").append('\n');
                final Bitmap b=bitmap;final boolean ok=pass;final String snapshot=t.toString();
                runOnUiThread(()->{report=snapshot;log.setText(snapshot);outcomes[idx].setText(ok?"PASS · 픽셀 비교 및 음성 대조군 통과":"FAIL / 미완료 · 로그 확인");if(b!=null){android.graphics.drawable.BitmapDrawable d=new android.graphics.drawable.BitmapDrawable(getResources(),b);d.setFilterBitmap(false);images[idx].setImageDrawable(d);}});
            }
        }catch(Exception e){t.append("ERROR: ").append(e).append('\n');}
        t.append("\nOVERALL: ").append(passes).append("/4 PASS\n");final String s=t.toString();
        runOnUiThread(()->{report=s;log.setText(s);run.setEnabled(true);});
    }
    @Override protected void onDestroy(){Process p=child;if(p!=null)p.destroyForcibly();worker.shutdownNow();super.onDestroy();}
}
