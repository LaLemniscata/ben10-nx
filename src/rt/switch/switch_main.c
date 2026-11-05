/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Entry point of the Nintendo Switch build. It sets up the folder on the SD card, then enters the
 * runtime's driver the way the desktop launcher starts a package.
 *
 * sdmc:/switch/ben10-psp/ holds game.iso, <title>_image.bin, assets/ and font/; the runtime writes
 * its saves to memstick/ and its log to log.txt.
 *
 * Empty files in that folder change how it runs: debug.txt adds measurements to the log,
 * profile.txt samples where the CPU goes into profile.bin, and scale1.txt, scale3.txt or scale4.txt
 * replace the default render scale of 2.
 */
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <switch.h>

#ifndef NK_SWITCH_TITLE
#error "NK_SWITCH_TITLE names the package, for example experimental-ulus10307"
#endif
#ifndef NK_SWITCH_ENTRY
#error "NK_SWITCH_ENTRY is the guest entry address from the package"
#endif

#define NK_SWITCH_ROOT "sdmc:/switch/ben10-psp"
#define NK_SWITCH_BASE "0x08804000"

int sr_driver_main(int argc, char **argv);

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    /* The guest's floating-point helpers run directly under the host's default environment: round
     * to nearest, no flush, IEEE NaNs. fp_convert.h relies on this. */
    __asm__ __volatile__("msr fpcr, xzr");
    mkdir(NK_SWITCH_ROOT, 0777);
    if (chdir(NK_SWITCH_ROOT) != 0) return 1;
    mkdir("memstick", 0777);
    mkdir("fs", 0777);
    freopen("log.txt", "w", stderr);
    freopen("log.txt", "a", stdout);
    setvbuf(stderr, NULL, _IOLBF, 0);

    setenv("PSP_ISO", "game.iso", 1);
    setenv("SR_MEMSTICK", "memstick", 1);
    setenv("SR_FSDIR", "fs", 1);
    setenv("SR_FPS_CAP", "native", 1);
    setenv("SR_GPU_GE", "0", 1);
    setenv("SR_BOOT_PATH", "", 1);
    /* SR_DEBUG keeps every other logging category off. */
    setenv("SR_DEBUG", "0", 1);
    if (access("debug.txt", F_OK) == 0) {
        setenv("SR_GE_CPU_PROFILE", "1", 1);
        setenv("SR_DLGLOG", "1", 1);
    }

    static char *driver_argv[] = {
        "ben10-psp", "--image", NK_SWITCH_TITLE "_image.bin", NK_SWITCH_BASE, NK_SWITCH_ENTRY,
        "none", "none", "--gui", NULL,
    };
    const int rc = sr_driver_main(8, driver_argv);
    fflush(stderr);
    return rc;
}
