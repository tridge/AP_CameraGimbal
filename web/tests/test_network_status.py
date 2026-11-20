#!/usr/bin/env python3
"""Check the web restart acknowledgement against current/stale app status."""
from pathlib import Path
import subprocess
import tempfile

WEB = Path(__file__).resolve().parents[1]
SOURCE = r'''
#define main camera_web_main
#include "mt11-web.cpp"
#undef main
#include <assert.h>

static void status(const char *state, long pid)
{
    const char *config = "[network]\ninterface=eth0\n";
    FILE *f = fopen(REPLACEMENT_CONFIG_PATH, "w");
    assert(f); fputs(config, f); fclose(f);
    f = fopen(CAMERA_READY_PATH, "w");
    assert(f); fprintf(f, "backend=mt11\npid=%ld\n", (long)getpid()); fclose(f);
    f = fopen(CAMERA_READY_PATH ".config", "w");
    assert(f);
    fprintf(f, "%016llx %ld %s\n%s\n",
        (unsigned long long)apcam_config_hash(config, strlen(config), APCAM_CONFIG_HASH_INITIAL), pid, state,
        !strcmp(state, "error") ? "Network configuration failed on eth0: Network is unreachable. Restart to retry." : "All saved settings applied.");
    fclose(f);
}
int main(void)
{
    char error[512];
    status("error", getpid());
    assert(!restarted_config_ok(error, sizeof(error)));
    assert(strstr(error, "Network configuration failed on eth0"));
    status("applied", getpid());
    assert(restarted_config_ok(error, sizeof(error)));
    /* A stale success must not hide the new process's missing acknowledgement. */
    status("applied", 1);
    assert(!restarted_config_ok(error, sizeof(error)));
    assert(strstr(error, "not acknowledged"));
    puts("PASS web restart reports network failure and rejects stale acknowledgement");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix='web-network-status-') as directory:
    root = Path(directory)
    source = root / 'test.c'
    source.write_text(SOURCE)
    binary = root / 'test'
    subprocess.run(['c++', '-std=gnu++17', '-Wno-missing-field-initializers', '-O2', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-std=gnu++17',
                    '-DMT11_WEB_TEST', '-DAPCAM_TARGET=APCAM_TARGET_MT11', f'-I{WEB}', f'-DREPLACEMENT_CONFIG_PATH="{root / "camera.ini"}"',
                    f'-DCAMERA_READY_PATH="{root / "ready"}"', str(source), '-o', str(binary), '-lm'], check=True)
    subprocess.run([str(binary)], check=True)
