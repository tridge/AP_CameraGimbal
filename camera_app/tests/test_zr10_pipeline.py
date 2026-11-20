#!/usr/bin/env python3
"""Exercise production ZR10 pipeline resource acquisition and teardown with a fake SDK."""
import os
from pathlib import Path
import re
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
backend=root/'src/backends/zr10'

def run(args,**kw):
    return subprocess.run(args,text=True,capture_output=True,timeout=15,check=True,**kw)

with tempfile.TemporaryDirectory(prefix='zr10-pipeline-') as tmp:
    tmp=Path(tmp)
    code='''#define _GNU_SOURCE
#include "mi_api.h"
#include <fcntl.h>
#include <unistd.h>
static int serial,fd[4][2];static unsigned frame[4];
static int record(const char *name) {
 FILE *f=fopen(getenv("MOCK_LOG"),"a");fprintf(f,"%s\\n",name);fclose(f);
 return ++serial==atoi(getenv("MOCK_FAIL")?:"0") ? -123:0;
}
static unsigned char data[]={0,0,0,1,0x65,0xaa};
'''
    custom={
      'MI_SNR_QueryResCount':'*a1=1;',
      'MI_SNR_GetRes':'*a2=(i6_snr_res){.crop={0,0,2560,1440},.maxFps=30,.minFps=5};',
      'MI_SNR_GetPadInfo':'*a1=(i6_snr_pad){.intf=I6_INTF_MIPI,.planeCnt=2};',
      'MI_SNR_GetPlaneInfo':'*a2=(i6_snr_plane){.capt={0,0,2560,1440},.bayer=I6_BAYER_GR,.precision=I6_PREC_10BPP};',
      'MI_VIF_SetDevAttr':'if(sizeof(*a1)!=52 || a1->multiDevMap!=1)abort();',
      'MI_VIF_SetChnPortAttr':'if(a2->field!=3 || a2->frameLineCnt!=1440)abort();',
      'MI_SYS_BindChnPort2':'if(a1->module==I6_SYS_MOD_VENC && a1->channel==3 && a0->port!=0)abort();',
      'MI_SYS_UnBindChnPort':'if(a1->module==I6_SYS_MOD_VENC && a1->channel==3 && a0->port!=0)abort();',
      'MI_VPE_EnablePort':'if(a1>=3)abort();',
      'MI_VENC_GetChnDevid':'*a1=a0==3 ? 1:0;',
      'MI_VENC_GetFd':'if(pipe2(fd[a0],O_NONBLOCK))abort();write(fd[a0][1],"x",1);return fd[a0][0];',
      'MI_VENC_CloseFd':'close(fd[a0][0]);close(fd[a0][1]);',
      'MI_VENC_Query':'*a1=(i6_venc_stat){.curPacks=1};',
      'MI_VENC_GetStream':'''a1->count=1;a1->packet[0]=(i6_venc_pack){.data=data,.length=sizeof(data),.timestamp=0xe123000000000000ULL+(++frame[a0])*33333ULL};
        if(getenv("MOCK_BAD"))a1->packet[0].offset=100;''',
    }
    entries=re.findall(r'X\(\w+, (MI_\w+), \(([^)]*)\)\)',(backend/'mi_api.h').read_text())
    for name,args in entries:
        decl='void' if args=='void' else ','.join(f'{t} a{i}' for i,t in enumerate(args.split(',')))
        code+=f'int {name}({decl}){{ if(record("{name}"))return -123;{custom.get(name,"")}return 0;}}\n'
    code+="""int MI_ISP_CUS3A_GetAFStats(unsigned channel,void *buffer) {
      (void)channel;if(getenv("MOCK_AF_FAIL"))return -1;
      for(unsigned i=0;i<6912;i++)((unsigned char *)buffer)[i]=(i+7)%251;
      return 0;
    }\n"""
    (tmp/'sdk.c').write_text(code)
    run(['cc','-shared','-fPIC','-I'+str(root.parent/'include'),'-I'+str(backend),str(tmp/'sdk.c'),'-o',str(tmp/'sdk.so')])
    for name in ['cam_os_wrapper','mi_sys','mi_sensor','mi_vif','ispalgo','cus3a','mi_isp','mi_vpe','mi_venc']:
        (tmp/('lib'+name+'.so')).symlink_to(tmp/'sdk.so')
    (tmp/'main.cpp').write_text('''#define _POSIX_C_SOURCE 200809L
#include "pipeline.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
void ca_log(const char *fmt,...) { (void)fmt; }
int main(void) {
 struct ca_zr10_pipeline_config c={.jpeg_quality=85};
 for(unsigned i=0;i<3;i++)c.streams[i]=(struct ca_zr10_stream){1280,720,CA_VIDEO_H264,2000};
 int r=ca_zr10_pipeline_open(&c);
 if(!r) {
   for(unsigned ch=0;ch<3 && !r;ch++)for(unsigned n=0;n<2 && !r;n++) {
      unsigned char *p=NULL;size_t len;uint64_t pts;
      r=ca_zr10_venc_get(ch,&p,&len,&pts);
      if(r==1 && len==6 && pts==n*33333ULL)r=0;else r=-1;
      free(p);
   }
   if(!r)r=ca_zr10_load_isp_bin("fake.bin");
   if(!r) {
     unsigned char p[27];unsigned out=0;
     unsigned offset[]={0,80,160,224,304,384},length[]={5,5,4,5,5,3};
     if(ca_zr10_take_focus(p))abort();
     ca_zr10_sample_focus();if(!ca_zr10_take_focus(p))abort();
     for(unsigned i=0;i<6;i++)for(unsigned j=0;j<length[i];j++)
       if(p[out++]!=(offset[i]+j+7)%251)abort();
     if(ca_zr10_take_focus(p))abort();
     setenv("MOCK_AF_FAIL","1",1);ca_zr10_sample_focus();
     if(ca_zr10_take_focus(p))abort();unsetenv("MOCK_AF_FAIL");
     ca_zr10_sample_focus();
   }
   if(!r) { unsigned char *p=NULL;size_t len;r=ca_zr10_capture_jpeg(&p,&len);free(p); }
 }
 ca_zr10_pipeline_close();
 unsigned char stale[27];if(ca_zr10_take_focus(stale))abort();
 return r ? 1:0;
}
''')
    run(['c++', '-std=gnu++17', '-Wno-missing-field-initializers','-std=gnu++17','-DAPCAM_TARGET=APCAM_TARGET_ZR10','-I'+str(root.parent/'include'),'-DCA_ZR10_FAKE_SDK','-I'+str(backend),'-I'+str(root/'include'),str(tmp/'main.cpp'),str(backend/'pipeline.cpp'),'-ldl','-o',str(tmp/'test')])
    env=dict(os.environ,LD_LIBRARY_PATH=str(tmp),MOCK_LOG=str(tmp/'calls'))
    run([str(tmp/'test')],env=env)
    baseline=(tmp/'calls').read_text().splitlines()
    assert baseline.count('MI_VPE_EnablePort')==3
    assert baseline.count('MI_VENC_CreateChn')==4
    assert baseline.count('MI_VENC_DestroyChn')==4
    assert baseline.count('MI_VENC_ReleaseStream')==7
    assert baseline[-1]=='MI_SYS_Exit'
    # Fail every initialization/acquisition/IQ call before JPEG teardown.
    end=baseline.index('MI_VENC_StopRecvPic')
    paired={'MI_SNR_Enable':'MI_SNR_Disable','MI_VIF_EnableDev':'MI_VIF_DisableDev',
      'MI_VIF_EnableChnPort':'MI_VIF_DisableChnPort','MI_VPE_CreateChannel':'MI_VPE_DestroyChannel',
      'MI_VPE_StartChannel':'MI_VPE_StopChannel','MI_VENC_CreateChn':'MI_VENC_DestroyChn',
      'MI_VENC_StartRecvPic':'MI_VENC_StopRecvPic','MI_VPE_EnablePort':'MI_VPE_DisablePort',
      'MI_VENC_GetFd':'MI_VENC_CloseFd','MI_SYS_Init':'MI_SYS_Exit'}
    checked=0
    for index in range(1,end+1):
        # Release failure still must release everything else; IDR failure is
        # logged best-effort and does not abort pipeline operation.
        if baseline[index-1] in ('MI_VENC_ReleaseStream','MI_VENC_RequestIdr'):continue
        (tmp/'calls').unlink()
        result=subprocess.run([str(tmp/'test')],env=dict(env,MOCK_FAIL=str(index)),timeout=10)
        assert result.returncode==1,(index,baseline[index-1])
        calls=(tmp/'calls').read_text().splitlines()
        for acquire,release in paired.items():
            assert calls[:index-1].count(acquire)==calls[index:].count(release),(index,acquire,calls)
        checked+=1
    (tmp/'calls').unlink()
    result=subprocess.run([str(tmp/'test')],env=dict(env,MOCK_BAD='1'),timeout=10)
    assert result.returncode==1
    assert (tmp/'calls').read_text().splitlines().count('MI_VENC_ReleaseStream')==1
    print(f'PASS ZR10 production pipeline: 3 video encoders + transient JPEG, normalized PTS, malformed pack release and {checked} injected failures')
