// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// A process whose /proc files are closed to its own user, as a program run with file
// capabilities is (Monado's monado-service, cap_sys_nice): for tests/launcher_core.cpp,
// which runs a copy named monado-service. Sleeps until it's killed. Not a test itself,
// so not named x4vr_test_*: the suite would wait on it forever.
#include <sys/prctl.h>
#include <unistd.h>

int main(void) {
    prctl(PR_SET_DUMPABLE, 0);
    for (;;)
        pause();
}
