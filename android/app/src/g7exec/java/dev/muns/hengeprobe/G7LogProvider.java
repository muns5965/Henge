package dev.muns.hengeprobe;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;

import java.io.File;
import java.io.FileNotFoundException;

/**
 * [G7-EXEC] Only in the g7exec build. Hands out the run logs (g7_run_<13 digits>.txt in the app's private files) READ-ONLY so the share sheet can send the whole file. Not exported:
 * access exists only through the grant that the share intent carries. Nothing else of the app can be reached (the name is checked by G7ExecLauncher.logFile), nothing can be written.
 */
public final class G7LogProvider extends ContentProvider {
    static final String AUTHORITY = "dev.muns.hengeprobe.g7logs";

    @Override public boolean onCreate() { return true; }

    private File file(Uri uri) {
        if (getContext() == null) return null;
        return G7ExecLauncher.logFile(getContext().getFilesDir(), uri.getLastPathSegment());
    }

    @Override public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        if (!"r".equals(mode)) throw new SecurityException("read only");
        File f = file(uri);
        if (f == null) throw new FileNotFoundException("not a run log");
        return ParcelFileDescriptor.open(f, ParcelFileDescriptor.MODE_READ_ONLY);
    }

    @Override public Cursor query(Uri uri, String[] projection, String selection, String[] selectionArgs, String sortOrder) {
        File f = file(uri);
        if (f == null) return null;
        MatrixCursor c = new MatrixCursor(new String[] { OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE });
        c.addRow(new Object[] { f.getName(), f.length() });
        return c;
    }

    @Override public String getType(Uri uri) { String n = uri.getLastPathSegment(); return n != null && n.endsWith(".zip") ? "application/zip" : "text/plain"; }
    @Override public Uri insert(Uri uri, ContentValues values) { throw new UnsupportedOperationException("read only"); }
    @Override public int delete(Uri uri, String selection, String[] selectionArgs) { throw new UnsupportedOperationException("read only"); }
    @Override public int update(Uri uri, ContentValues values, String selection, String[] selectionArgs) { throw new UnsupportedOperationException("read only"); }
}
