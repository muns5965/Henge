package dev.muns.hengeprobe;

/** S5 of docs/gpu-research: the read-only Vulkan device report of this phone. Loads its own library only when asked; never throws. */
final class VulkanProbe {
    private VulkanProbe() { }
    private static native String probe();

    static String report() {
        try {
            System.loadLibrary("henge_vkprobe");
            return probe();
        } catch (UnsatisfiedLinkError e) {
            return "Vulkan 보고를 만들 수 없습니다(라이브러리를 불러오지 못함): " + e.getMessage();
        } catch (RuntimeException e) {
            return "Vulkan 보고 실패: " + e.getClass().getSimpleName() + " " + e.getMessage();
        }
    }
}
