package dev.muns.hengeprobe;

import java.io.File;
import java.nio.file.Files;

/** Compiled against the g7start stand-in of BuildConfig (switch ON): the public start() still refuses before any process or file is made unless EVERYTHING is right.
 *  No ROM/ECID exists in these directories, so even a fully typed request stops at the preflight. No process is ever started (the REAL spawner is only reached after the preflight passes). */
public class G7ExecStartOnTest {
    static int fails;
    static void check(String n, boolean ok) { System.out.println((ok ? "PASS " : "FAIL ") + n); if (!ok) fails++; }
    
    public static void main(String[] a) throws Exception {
        File lib = Files.createTempDirectory("lib").toFile(), priv = Files.createTempDirectory("vm").toFile(), ext = Files.createTempDirectory("ext").toFile(), files = Files.createTempDirectory("files").toFile();
        check("this build has the switch ON and says its kind", G7ExecLauncher.START_ENABLED && "G7-BUILD-KIND:START-ON".equals(G7ExecLauncher.BUILD_KIND));
        G7ExecLauncher.Start s = G7ExecLauncher.start(lib, ext, priv, files, true);
        check("no runner file packaged -> refused", !s.started && s.message.contains("libg7runner.so"));
        Files.write(new File(lib, "libg7runner.so").toPath(), new byte[] {1});
        s = G7ExecLauncher.start(lib, ext, priv, files, false);
        check("consent box not checked -> refused", !s.started && s.message.contains("동의"));
        s = G7ExecLauncher.start(lib, ext, priv, files, true);
        check("consent given but no ROM/ECID/IPSW -> refused at the preflight, no run", !s.started && s.run == null && s.message.contains("사전 확인"));
        check("nothing was created: no log, no work dir", files.list().length == 0);
        check("no current run", G7ExecLauncher.current() == null);
        System.out.println(fails == 0 ? "G7EXEC START-ON OK (0 failed)" : "G7EXEC START-ON FAILED " + fails);
        System.exit(fails == 0 ? 0 : 1);
    }
}
