#!/usr/bin/env python3
"""Check UUID ABI, decimal formatting, retries and isolation from SDK stdout."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='siyi-uuid-') as directory:
    root = Path(directory)
    stub = root / 'sdk.c'
    stub.write_text(r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef A8
#define INIT_ARGS unsigned short soc
#define UUID_ARGS unsigned short soc, uint64_t *uuid
#define CHECK_SOC if (soc != 0) return -100;
#else
#define INIT_ARGS void
#define UUID_ARGS uint64_t *uuid
#define CHECK_SOC
#endif
int MI_SYS_Init(INIT_ARGS) {
    CHECK_SOC
    puts("SDK init stdout must not be hashed");
    return getenv("FAIL_INIT") ? -1 : 0;
}
int MI_SYS_ReadUuid(UUID_ARGS) {
    CHECK_SOC
    static unsigned calls;
    puts("SDK UUID stdout must not be hashed");
    if (getenv("FAIL_READ") || ++calls < 3) return -1;
    *uuid = strtoull(getenv("UUID"), 0, 0);
    return 0;
}
''')
    for target in ('a8', 'zr10'):
        sdk = root / target
        sdk.mkdir()
        defines = ['-DA8'] if target == 'a8' else []
        subprocess.run(['cc', '-shared', '-fPIC', *defines, str(stub),
                        '-o', str(sdk / 'libmi_sys.so')], check=True)
        # A harmless stand-in for the preloaded OS wrapper.
        (sdk / 'libcam_os_wrapper.so').symlink_to('libmi_sys.so')
        helper = sdk / 'uuid'
        subprocess.run(['c++', '-Wno-missing-field-initializers', '-Wall', '-Wextra', '-Werror', '-std=gnu++17',
                        str(ROOT / f'packaging/{target}/{target}-uuid.cpp'),
                        '-o', str(helper), '-ldl'], check=True)
        env = dict(os.environ, LD_LIBRARY_PATH=str(sdk))
        if target == 'zr10':
            boot = sdk / 'boot.sh'
            boot.write_text((ROOT / 'packaging/zr10/boot.sh').read_text().replace(
                '/customer/zr10-uuid', str(helper)))
            result = subprocess.run(['sh', str(boot), 'Uuid'],
                                    env=env | {'UUID': '123456789'},
                                    capture_output=True, text=True, timeout=5)
            assert result.returncode == 0 and result.stdout == '123456789', result
            result = subprocess.run(['sh', str(boot), 'unknown'], env=env,
                                    capture_output=True, text=True, timeout=5)
            assert result.returncode == 2 and result.stdout == '', result
        for value in (0x123456789ABCDEF, 0xFEDCBA9876543210):
            result = subprocess.run([str(helper)], env=env | {'UUID': hex(value)},
                                    capture_output=True, text=True, timeout=5)
            expected = value - (1 << 64) if target == 'zr10' and value >= (1 << 63) else value
            assert result.returncode == 0 and result.stdout == str(expected), result
        for failure in ('FAIL_INIT', 'FAIL_READ'):
            result = subprocess.run([str(helper)], env=env | {'UUID': '1', failure: '1'},
                                    capture_output=True, text=True, timeout=5)
            assert result.returncode != 0 and result.stdout == '', result
print('PASS A8/ZR10 UUID calling conventions, signed/unsigned formatting, retries and clean stdout')
