package android.content.res;

/** Test stand-in for the Android class (G7PipeRepro.stageRedAssets takes one); the JVM test has no assets, so list() gives nothing. */
public class AssetManager {
    public String[] list(String path) throws java.io.IOException { return null; }
    public java.io.InputStream open(String name) throws java.io.IOException { throw new java.io.IOException("no assets in the JVM test"); }
}
