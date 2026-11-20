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

/** Shows and exports vm/snapshot.bmp: the one-shot screen capture every G0-G6 run, and the normal/installer boot
 *  test, attempt whenever --snapshot is given (host/vmrun.c's g_capture_screen() for G0-G6, the normal path's own
 *  snap block otherwise; both go through host/vm.c's hv_vm_capture_screen()). All of them write to the same
 *  shared path (MainActivity's `snap` field), so this screen does not need to know which mode produced it.
 *
 *  Decodes the BMP itself with a small reader for EXACTLY the layout host/vmrun.c's write_bmp() produces (a
 *  54-byte BITMAPFILEHEADER+BITMAPINFOHEADER, top-down, 32 bits/pixel, BI_RGB, no color table) rather than
 *  BitmapFactory, whose built-in BMP support for a top-down 32bpp file is not guaranteed consistent across
 *  Android versions and OEM skins. This is not a general-purpose BMP reader and rejects anything that does not
 *  match that exact shape.
 *
 *  Export uses ACTION_CREATE_DOCUMENT, the standard Android API for an app to hand a copy of a file it already
 *  owns to a folder the user picks (Downloads, a cloud-backed folder, etc.) without requesting any storage
 *  permission. This does not work around or bypass the Android/data folder's own visibility restriction in any
 *  way -- it is the normal, supported route for an app to share a file it owns, not a way into another app's or
 *  the system's restricted storage.
 *
 *  Touches no vendored source, no G0-G6 code, and no part of the virtualization/restore engine: Android UI and
 *  file I/O on an already-produced file only. */
public class SnapshotViewActivity extends Activity {
    private static final int REQ_CREATE_DOCUMENT = 1;
    private File snap;
    private android.widget.TextView status;

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        snap = new File(getExternalFilesDir(null), "vm/snapshot.bmp");

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(Color.BLACK);

        status = new android.widget.TextView(this);
        status.setTextColor(Color.WHITE);
        status.setPadding(24, 24, 24, 12);
        root.addView(status);

        final ImageView img = new ImageView(this);
        LinearLayout.LayoutParams ip = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f);
        img.setLayoutParams(ip);
        img.setAdjustViewBounds(true);
        root.addView(img);

        LinearLayout buttons = new LinearLayout(this);
        buttons.setOrientation(LinearLayout.HORIZONTAL);
        buttons.setGravity(Gravity.CENTER);
        buttons.setPadding(0, 0, 0, 24);
        final Button exportBtn = new Button(this);
        exportBtn.setText("내보내기 (다운로드 폴더 등으로 저장)");
        exportBtn.setEnabled(false);
        buttons.addView(exportBtn);
        root.addView(buttons);

        setContentView(root);

        if (!snap.isFile()) {
            status.setText("스냅샷 파일이 없습니다: " + snap.getAbsolutePath()
                    + "\nG0~G6 또는 일반/설치기 부팅 시험을 먼저 한 번 실행하세요(모두 --snapshot과 함께 이 경로에 씁니다).");
            return;
        }

        final Bitmap bmp;
        try {
            bmp = decodeHengeBmp(snap);
        } catch (IOException e) {
            status.setText("스냅샷을 읽는 중 오류: " + e.getMessage());
            return;
        } catch (IllegalArgumentException e) {
            status.setText("스냅샷 형식을 인식하지 못했습니다(" + e.getMessage() + "). host/vmrun.c의 write_bmp() 출력이 아닌 것 같습니다.");
            return;
        }
        img.setImageBitmap(bmp);
        status.setText(snap.getAbsolutePath() + "\n" + bmp.getWidth() + "x" + bmp.getHeight() + ", " + snap.length() + " bytes, " + new Date(snap.lastModified()));
        exportBtn.setEnabled(true);
        exportBtn.setOnClickListener(v -> {
            Intent i = new Intent(Intent.ACTION_CREATE_DOCUMENT);
            i.addCategory(Intent.CATEGORY_OPENABLE);
            i.setType("image/bmp");
            i.putExtra(Intent.EXTRA_TITLE, "henge_snapshot.bmp");
            startActivityForResult(i, REQ_CREATE_DOCUMENT);
        });
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != REQ_CREATE_DOCUMENT || resultCode != RESULT_OK || data == null) return;
        Uri uri = data.getData();
        if (uri == null) return;
        try (InputStream in = new FileInputStream(snap); OutputStream out = getContentResolver().openOutputStream(uri)) {
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
