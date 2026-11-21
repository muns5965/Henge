package dev.muns.hengeprobe;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Bitmap;
import android.graphics.Color;
import android.net.Uri;
import android.os.Bundle;
import android.view.Gravity;
import android.widget.Button;
import android.widget.ImageView;
import android.widget.LinearLayout;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.Date;

/** Shows and exports vm/snapshot.bmp (the one-shot screen capture every G0-G6 run, and the normal/installer boot
 *  test, writes whenever --snapshot is given -- host/vmrun.c's g_capture_screen() for G0-G6, the normal path's
 *  own snap block otherwise; both go through host/vm.c's hv_vm_capture_screen()) and, alongside it,
 *  vm/snapshot_pvgsurface.bmp (G6-only, host/vmrun.c's g_capture_pvg_surface(): the actual PVG-surface pixels
 *  decoded by devices/pvg_lite.c's hv_pvg_lite_last_surface_bgra(), written right after the plain capture).
 *  Both are produced under MainActivity's `snap` path (one is `<snap>` itself, the other `<snap>` with
 *  "_pvgsurface" inserted before the extension), so this screen does not need to know which run produced them --
 *  it only needs their paths.
 *
 *  A tab pair lets the person pick "PVG 화면" or "기존 화면"; PVG is the default selection whenever its file
 *  decodes to a genuinely valid image (not merely File.exists()) -- falling back to the plain capture otherwise.
 *  Both files are decoded on background threads (the PVG surface is roughly 8.3 MB at 1920x1080, too large to
 *  decode on the main thread without visibly blocking it), and every UI update from a decode result is posted
 *  back with runOnUiThread() -- which Android guarantees to run in posting order on the main thread -- so the
 *  two decode threads never race each other for the screen's own state; a decode that finishes after the person
 *  has already switched tabs away from it just updates that tab's cached result quietly, and is shown only if
 *  the person switches back to it.
 *
 *  Decodes each BMP itself with a small reader for EXACTLY the layout host/vmrun.c's write_bmp() produces (a
 *  54-byte BITMAPFILEHEADER+BITMAPINFOHEADER, top-down, 32 bits/pixel, BI_RGB, no color table) rather than
 *  BitmapFactory, whose built-in BMP support for a top-down 32bpp file is not guaranteed consistent across
 *  Android versions and OEM skins. This is not a general-purpose BMP reader and rejects anything that does not
 *  match that exact shape -- which is exactly the validity check the PVG-default decision above relies on.
 *
 *  Export uses ACTION_CREATE_DOCUMENT, the standard Android API for an app to hand a copy of a file it already
 *  owns to a folder the user picks (Downloads, a cloud-backed folder, etc.) without requesting any storage
 *  permission, and always exports whichever of the two files is currently selected. This does not work around
 *  or bypass the Android/data folder's own visibility restriction in any way -- it is the normal, supported
 *  route for an app to share a file it owns, not a way into another app's or the system's restricted storage.
 *
 *  Touches no vendored source, no G0-G6 engine code, and no part of the virtualization/restore engine or of
 *  VmDisplayActivity/[A]'s live VM display: Android UI and file I/O on already-produced files only. */
public class SnapshotViewActivity extends Activity {
    private static final int REQ_CREATE_DOCUMENT = 1;
    private enum Source { PVG, LEGACY }

    private File pvgFile;
    private File legacyFile;

    private android.widget.TextView status;
    private ImageView img;
    private Button pvgTab;
    private Button legacyTab;
    private Button exportBtn;

    /** null until the first default-selection decision is made (see onDecodeDone()). */
    private Source selected;
    private boolean destroyed;

    /** Per-source decode state, written only on the main thread (every background decode result arrives via
     *  runOnUiThread(), so these never need their own lock). done=true once that source's decode has been
     *  attempted (successfully or not); bitmap/error hold the outcome. */
    private boolean pvgDone, legacyDone;
    private Bitmap pvgBitmap, legacyBitmap;
    private String pvgError, legacyError;

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        legacyFile = new File(getExternalFilesDir(null), "vm/snapshot.bmp");
        pvgFile = pvgSiblingPath(legacyFile);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(Color.BLACK);

        LinearLayout tabs = new LinearLayout(this);
        tabs.setOrientation(LinearLayout.HORIZONTAL);
        tabs.setGravity(Gravity.CENTER);
        tabs.setPadding(0, 16, 0, 0);
        pvgTab = new Button(this);
        pvgTab.setText("PVG 화면");
        legacyTab = new Button(this);
        legacyTab.setText("기존 화면");
        tabs.addView(pvgTab);
        tabs.addView(legacyTab);
        root.addView(tabs);

        status = new android.widget.TextView(this);
        status.setTextColor(Color.WHITE);
        status.setPadding(24, 16, 24, 12);
        root.addView(status);

        img = new ImageView(this);
        LinearLayout.LayoutParams ip = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f);
        img.setLayoutParams(ip);
        img.setAdjustViewBounds(true);
        img.setScaleType(ImageView.ScaleType.FIT_CENTER);
        root.addView(img);

        LinearLayout buttons = new LinearLayout(this);
        buttons.setOrientation(LinearLayout.HORIZONTAL);
        buttons.setGravity(Gravity.CENTER);
        buttons.setPadding(0, 0, 0, 24);
        exportBtn = new Button(this);
        exportBtn.setText("내보내기 (다운로드 폴더 등으로 저장)");
        exportBtn.setEnabled(false);
        buttons.addView(exportBtn);
        root.addView(buttons);

        setContentView(root);

        pvgTab.setOnClickListener(v -> { selected = Source.PVG; updateUi(); });
        legacyTab.setOnClickListener(v -> { selected = Source.LEGACY; updateUi(); });
        exportBtn.setOnClickListener(v -> {
            Intent i = new Intent(Intent.ACTION_CREATE_DOCUMENT);
            i.addCategory(Intent.CATEGORY_OPENABLE);
            i.setType("image/bmp");
            i.putExtra(Intent.EXTRA_TITLE, selected == Source.PVG ? "henge_snapshot_pvgsurface.bmp" : "henge_snapshot.bmp");
            startActivityForResult(i, REQ_CREATE_DOCUMENT);
        });

        updateUi(); // shows "로딩 중" until the decodes below land
        startDecode(pvgFile, Source.PVG);
        startDecode(legacyFile, Source.LEGACY);
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        destroyed = true; // background threads still finish; their runOnUiThread callback just becomes a no-op
    }

    /** vm/snapshot.bmp -> vm/snapshot_pvgsurface.bmp, matching host/vmrun.c's g_capture_pvg_surface() naming. */
    private static File pvgSiblingPath(File legacy) {
        String name = legacy.getName();
        int dot = name.lastIndexOf('.');
        String pvgName = dot < 0 ? name + "_pvgsurface" : name.substring(0, dot) + "_pvgsurface" + name.substring(dot);
        return new File(legacy.getParentFile(), pvgName);
    }

    private void startDecode(File f, Source which) {
        new Thread(() -> {
            Bitmap bmp = null;
            String err = null;
            if (!f.isFile()) {
                err = "파일이 없습니다: " + f.getAbsolutePath();
            } else {
                try {
                    bmp = decodeHengeBmp(f);
                } catch (IOException e) {
                    err = "읽는 중 오류: " + e.getMessage();
                } catch (IllegalArgumentException e) {
                    err = "형식을 인식하지 못했습니다(" + e.getMessage() + ")";
                }
            }
            final Bitmap fb = bmp;
            final String fe = err;
            runOnUiThread(() -> {
                if (destroyed) return;
                if (which == Source.PVG) { pvgBitmap = fb; pvgError = fe; pvgDone = true; }
                else { legacyBitmap = fb; legacyError = fe; legacyDone = true; }
                onDecodeDone(which);
            });
        }, "snapshot-decode-" + which).start();
    }

    /** Called on the main thread after either decode lands. Picks the default tab the first time PVG's outcome
     *  is known (PVG if it decoded to a real image, else the legacy capture -- never File.exists() alone), and
     *  otherwise just refreshes the screen if the tab that just finished is the one currently showing. */
    private void onDecodeDone(Source which) {
        if (selected == null && pvgDone) {
            selected = (pvgBitmap != null) ? Source.PVG : Source.LEGACY;
        }
        if (which == selected || selected == null) updateUi();
    }

    private void updateUi() {
        pvgTab.setEnabled(selected != Source.PVG);
        legacyTab.setEnabled(selected != Source.LEGACY);

        if (selected == null) {
            status.setText("로딩 중...");
            img.setImageBitmap(null);
            exportBtn.setEnabled(false);
            return;
        }
        boolean isPvg = selected == Source.PVG;
        File f = isPvg ? pvgFile : legacyFile;
        boolean done = isPvg ? pvgDone : legacyDone;
        Bitmap bmp = isPvg ? pvgBitmap : legacyBitmap;
        String err = isPvg ? pvgError : legacyError;

        if (!done) {
            status.setText((isPvg ? "PVG 화면" : "기존 화면") + " 디코딩 중...\n" + f.getAbsolutePath());
            img.setImageBitmap(null);
            exportBtn.setEnabled(false);
        } else if (bmp == null) {
            status.setText((isPvg ? "PVG 화면" : "기존 화면") + ": " + err
                    + (isPvg ? "\nG6 시험을 먼저 실행하세요(PVG 표면 추출은 G6 전용입니다)."
                              : "\nG0~G6 또는 일반/설치기 부팅 시험을 먼저 한 번 실행하세요."));
            img.setImageBitmap(null);
            exportBtn.setEnabled(false);
        } else {
            status.setText(f.getAbsolutePath() + "\n" + bmp.getWidth() + "x" + bmp.getHeight()
                    + ", " + f.length() + " bytes, " + new Date(f.lastModified()));
            img.setImageBitmap(bmp);
            exportBtn.setEnabled(true);
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != REQ_CREATE_DOCUMENT || resultCode != RESULT_OK || data == null) return;
        Uri uri = data.getData();
        if (uri == null) return;
        File src = selected == Source.PVG ? pvgFile : legacyFile;
        try (InputStream in = new FileInputStream(src); OutputStream out = getContentResolver().openOutputStream(uri)) {
            if (out == null) throw new IOException("출력 스트림을 열 수 없습니다");
            byte[] buf = new byte[65536];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
            status.append("\n내보내기 완료: " + uri);
        } catch (IOException e) {
            status.append("\n내보내기 실패: " + e.getMessage());
        }
    }

    /** Decodes exactly the format host/vmrun.c's write_bmp() writes: 54-byte BITMAPFILEHEADER+BITMAPINFOHEADER,
     *  top-down (negative height), 32 bits/pixel, BI_RGB (no compression), no color table, pixel data immediately
     *  after the header, row-major, B/G/R/A per pixel (host/vm.h: "32-bit BGRA"). Rejects anything not matching
     *  that exact shape rather than guessing; this is not a general BMP reader. */
    private static Bitmap decodeHengeBmp(File f) throws IOException {
        byte[] all;
        try (InputStream in = new FileInputStream(f)) {
            all = readAll(in);
        }
        if (all.length < 54 || all[0] != 'B' || all[1] != 'M') throw new IllegalArgumentException("BM 시그니처 없음");
        int dataOffset = le32(all, 10);
        int dibSize = le32(all, 14);
        int width = le32(all, 18);
        int heightRaw = le32(all, 22);
        int bpp = le16(all, 28);
        int compression = le32(all, 30);
        if (dibSize < 40 || bpp != 32 || compression != 0)
            throw new IllegalArgumentException("dib=" + dibSize + " bpp=" + bpp + " comp=" + compression);
        boolean topDown = heightRaw < 0;
        int height = Math.abs(heightRaw);
        if (width <= 0 || height <= 0 || width > 16384 || height > 16384)
            throw new IllegalArgumentException(width + "x" + height);
        long need = (long) dataOffset + (long) width * height * 4;
        if (all.length < need) throw new IllegalArgumentException("파일이 선언된 크기보다 짧음");
        int[] px = new int[width * height];
        for (int row = 0; row < height; row++) {
            int srcRow = topDown ? row : (height - 1 - row);
            int srcOff = dataOffset + srcRow * width * 4;
            int dstOff = row * width;
            for (int x = 0; x < width; x++) {
                int o = srcOff + x * 4;
                int bB = all[o] & 0xff, g = all[o + 1] & 0xff, r = all[o + 2] & 0xff;
                // alpha forced opaque: the capture's own alpha byte is not meaningful display data
                px[dstOff + x] = 0xff000000 | (r << 16) | (g << 8) | bB;
            }
        }
        return Bitmap.createBitmap(px, width, height, Bitmap.Config.ARGB_8888);
    }

    private static int le16(byte[] a, int off) { return (a[off] & 0xff) | ((a[off + 1] & 0xff) << 8); }
    private static int le32(byte[] a, int off) {
        return (a[off] & 0xff) | ((a[off + 1] & 0xff) << 8) | ((a[off + 2] & 0xff) << 16) | ((a[off + 3] & 0xff) << 24);
    }

    private static byte[] readAll(InputStream in) throws IOException {
        ByteArrayOutputStream bos = new ByteArrayOutputStream();
        byte[] buf = new byte[65536];
        int n;
        while ((n = in.read(buf)) > 0) bos.write(buf, 0, n);
        return bos.toByteArray();
    }
}
