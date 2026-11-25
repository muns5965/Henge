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
    private ImageView image;
    private Button run;
    private volatile Process child;
    private final ExecutorService worker = Executors.newSingleThreadExecutor();
    private String report = "";
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(20,20,20,20);
        ScrollView scroll = new ScrollView(this); scroll.addView(root); setContentView(scroll);
        TextView heading = new TextView(this); heading.setText("GPT Vulkan / AIR rendering test"); heading.setTextSize(22); root.addView(heading);
        TextView info = new TextView(this); info.setText("독립 GPU 시험 · Henge VM 데이터 접근 없음\nCI에서 실제 AIR 변환기로 생성한 SPIR-V를 기기 GPU에서 실행합니다. PVG 연동 시험은 아닙니다."); root.addView(info);
        run = new Button(this); run.setText("실제 GPU 삼각형 그리기"); root.addView(run);
        image = new ImageView(this); image.setAdjustViewBounds(true); image.setBackgroundColor(Color.DKGRAY);
        root.addView(image, new LinearLayout.LayoutParams(-1, 600));
        log = new TextView(this); log.setTextIsSelectable(true); root.addView(log);
        Button copy = new Button(this); copy.setText("시험 로그 복사"); root.addView(copy);
        copy.setOnClickListener(v -> ((ClipboardManager)getSystemService(Context.CLIPBOARD_SERVICE)).setPrimaryClip(ClipData.newPlainText("GPU test",report)));
        run.setOnClickListener(v -> {run.setEnabled(false); image.setImageDrawable(null); log.setText("실행 중…"); worker.submit(this::test);});
    }
    private File asset(String name) throws IOException {
        File f = new File(getFilesDir(),name);
        try(InputStream in=getAssets().open(name); OutputStream out=new FileOutputStream(f)) {
            byte[] b=new byte[8192]; int n; while((n=in.read(b))!=-1) out.write(b,0,n);
        } return f;
    }
    private int execute(File exe, File v, File f, File output, boolean negative, StringBuilder text) throws Exception {
        if(output.exists()&&!output.delete()) throw new IOException("old output");
        ProcessBuilder pb=new ProcessBuilder(exe.getAbsolutePath(),v.getAbsolutePath(),f.getAbsolutePath());
        pb.directory(getFilesDir()); pb.redirectErrorStream(true);
        File stdout=new File(getFilesDir(),negative?"negative.log":"positive.log");
        pb.redirectOutput(stdout);
        pb.environment().put("GPU_TEST_OUTPUT",output.getAbsolutePath());
        if(negative) pb.environment().put("GPU_TEST_NO_DRAW","1");
        else pb.environment().remove("GPU_TEST_NO_DRAW");
        child=pb.start();
        boolean done=child.waitFor(45,TimeUnit.SECONDS);
        if(!done){child.destroyForcibly(); child.waitFor(5,TimeUnit.SECONDS);}
        try(BufferedReader br=new BufferedReader(new FileReader(stdout))){String s;while((s=br.readLine())!=null)text.append(s).append('\n');}
        int rc=done?child.exitValue():124; child=null;
        text.append("exit=").append(rc).append('\n'); return rc;
    }
    private void test() {
        StringBuilder t=new StringBuilder(); Bitmap bitmap=null;
        try {
            File provenance=asset("provenance.txt");
            try(BufferedReader b=new BufferedReader(new FileReader(provenance))){String s;while((s=b.readLine())!=null)t.append(s).append('\n');}
            File v=asset("triangle_vertex.spv"), f=asset("triangle_fragment.spv");
            File exe=new File(getApplicationInfo().nativeLibraryDir,"libgputest.so");
            File pixels=new File(getFilesDir(),"positive.bgra");
            t.append("\nPOSITIVE: actual draw\n");
            int p=execute(exe,v,f,pixels,false,t);
            String positive=t.toString();
            if(pixels.length()==64*64*4){
                byte[] raw=new byte[64*64*4]; try(DataInputStream in=new DataInputStream(new FileInputStream(pixels))){in.readFully(raw);}
                int[] argb=new int[64*64];
                for(int i=0;i<argb.length;i++)argb[i]=Color.argb(raw[i*4+3]&255,raw[i*4+2]&255,raw[i*4+1]&255,raw[i*4]&255);
                bitmap=Bitmap.createBitmap(argb,64,64,Bitmap.Config.ARGB_8888);
            }
            t.append("\nNEGATIVE: draw omitted, pixel comparison must fail\n");
            int start=t.length();
            int n=execute(exe,v,f,new File(getFilesDir(),"negative.bgra"),true,t);
            String neg=t.substring(start);
            boolean pass=p==0 && bitmap!=null && positive.contains("A5 RESULT: PASS") && n==1 && neg.contains("A5 RESULT: FAIL") && neg.contains("A4 RESULT: PASS");
            t.append("\nOVERALL: ").append(pass?"PASS (Vulkan pixel test; inspect device type)":"FAIL / incomplete").append('\n');
        } catch(Exception e){t.append("\nERROR: ").append(e).append('\n');}
        final Bitmap b=bitmap; final String s=t.toString();
        runOnUiThread(()->{report=s;log.setText(s); if(b!=null){android.graphics.drawable.BitmapDrawable d=new android.graphics.drawable.BitmapDrawable(getResources(),b);d.setFilterBitmap(false);image.setImageDrawable(d);}run.setEnabled(true);});
    }
    @Override protected void onDestroy(){Process p=child;if(p!=null)p.destroyForcibly();worker.shutdownNow();super.onDestroy();}
}
