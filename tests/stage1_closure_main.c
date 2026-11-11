/* STAGE 1: link-closure probe. References the host's public install API so that the linker pulls exactly what the Android host build would pull from the restore stack archive. Never run. */
#include "install.h"
int main(void) {
    volatile void *refs[] = { (void *)hv_install_start, (void *)hv_install_result, (void *)hv_install_cancel };
    return refs[0] != 0;
}
