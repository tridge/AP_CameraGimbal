#!/usr/bin/env python3
"""Shared INI validation and Network tab rendering on MT11 and native ZT30."""
from pathlib import Path
import subprocess
import tempfile
WEB=Path(__file__).resolve().parents[1]
SOURCE=r'''
#define main camera_web_main
#include "mt11-web.cpp"
#undef main
#include <cassert>
int main() {
    ca_config config{};
    auto enabled=APC_Config::find("network","vpn_enabled");
    auto provider=APC_Config::find("network","vpn_provider");
    auto id=APC_Config::find("network","vpn_network_id");
    assert(enabled && provider && id);
    assert(!config.vpn.enabled);
    assert(APC_Config::assign(config,*provider,"zerotier"));
    assert(!APC_Config::assign(config,*provider,"tailscale"));
    assert(APC_Config::assign(config,*id,"0123456789ABCDEF"));
    assert(!strcmp(config.vpn.network_id,"0123456789ABCDEF"));
    for (const char *bad : {"1234", "abfd31bd474e3f2g", "0000000000000000", "a; reboot", "0123456789abcdef0"})
        assert(!APC_Config::assign(config,*id,bad));
    assert(APC_Config::assign(config,*id,""));
    for (size_t i=0;i<sizeof(replacement_parameters)/sizeof(replacement_parameters[0]);i++) {
        auto &p=replacement_parameters[i];
        if (strncmp(p.key,"vpn_",4)) continue;
        assert(parameter_shown(&p));assert(parameter_tab(&p)==TAB_NETWORK);
        assert(APC_Config::validate(*APC_Config::find(p.section,p.key),replacement_defaults[i]));
        if (!strcmp(p.key,"vpn_network_id")) {
            assert(valid_text_parameter(&p,"abfd31bd474e3f28"));
            assert(!valid_text_parameter(&p,"abfd31bd474e3f2z"));
        }
    }
    puts("PASS VPN config validation, defaults and Network tab");
}
'''
with tempfile.TemporaryDirectory(prefix='web-vpn-') as directory:
    root=Path(directory)
    source=root/'test.cpp';source.write_text(SOURCE)
    for target,extra in [('MT11',[]),('ZT30',['-DCAMERA_APP_ZT30_DEV','-DZT30_WEB_NATIVE'])]:
        binary=root/target
        subprocess.run(['c++','-std=gnu++17','-O2','-Wall','-Wextra','-Werror','-Wno-unused-function',
            '-Wno-missing-field-initializers','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-DMT11_WEB_TEST',f'-DAPCAM_TARGET=APCAM_TARGET_{target}',
            *extra,f'-I{WEB}',str(source),'-o',str(binary),'-lm'],check=True)
        subprocess.run([str(binary)],check=True)
