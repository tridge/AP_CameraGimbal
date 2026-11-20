#!/usr/bin/env python3
"""Build a device-specific ZT30 development customer image and SD candidate.

This is not a release installer. Loader failure handling and interrupted-volume
recovery still need qualification. Output must end in .candidate so stock boot
cannot discover it as SIYI_ZT30_UpgradeSD.bin accidentally.
"""
import argparse, hashlib, json, os, shutil, struct, subprocess, tempfile, zlib, tarfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
SCRIPT_BYTES=0x4000
MIN_IO=2048
LEB_BYTES=126976
CUSTOMER_LEBS=659  # UBI reserved volume size
CUSTOMER_FS_MAX_LEBS=636  # Original customer UBIFS superblock limit
CUSTOMER_BYTES=LEB_BYTES*CUSTOMER_LEBS
MAGIC=0x06101831


def sha(data):return hashlib.sha256(data).hexdigest()


def check_ubifs(data):
    if len(data)<4096 or len(data)>CUSTOMER_BYTES or len(data)%LEB_BYTES:
        raise ValueError('customer UBIFS length/volume bounds invalid')
    magic,crc=struct.unpack_from('<II',data)
    length=struct.unpack_from('<I',data,16)[0]
    if magic!=MAGIC or data[20]!=6 or length!=4096:
        raise ValueError('UBIFS superblock header invalid')
    if crc!=(zlib.crc32(data[8:length])^0xffffffff)&0xffffffff:
        raise ValueError('UBIFS superblock CRC invalid')
    minimum,leb,count,maximum=struct.unpack_from('<IIII',data,32)
    if minimum!=MIN_IO or leb!=LEB_BYTES or not 3<=count<=maximum or maximum!=CUSTOMER_FS_MAX_LEBS:
        raise ValueError('customer UBIFS geometry differs from captured ZT30')
    if len(data)>count*LEB_BYTES:
        raise ValueError('customer UBIFS length exceeds filesystem LEB count')


def script_for(data):
    # Use only operations present in the captured vendor U-Boot. Do not assume
    # crc32 -v, test, itest, setexpr or hush conditionals: support is not verified.
    # The checksum is independently verified by inspect() before SD deployment;
    # boot-loader failure handling remains an explicit qualification gap.
    return (f'\n# AP ZT30 customer-only development candidate\n'
            f'# payload_sha256 {sha(data)}\n'
            f'# payload_crc32 {zlib.crc32(data)&0xffffffff:08x}\n'
            '# Loader failure handling NOT QUALIFIED: do not deploy this candidate\n'
            f'fatload mmc 0 0x21000000 $(SdUpgradeImage) 0x{len(data):x} 0x4000\n'
            'ubi part ubi0\n'
            'ubi check customer\n'
            f'ubi write 0x21000000 customer 0x{len(data):x}\n'
            'run bootcmd\n'
            '% <- this is end of script symbol\n').encode('ascii')


def pack(data):
    check_ubifs(data)
    script=script_for(data)
    if len(script)>=SCRIPT_BYTES:raise ValueError('script exceeds stock header')
    header=script.ljust(SCRIPT_BYTES,b'\0')
    return header+data+b'12345678'+header[:16]


def inspect(image):
    if len(image)<SCRIPT_BYTES+4096+24:raise ValueError('truncated SD container')
    header=image[:SCRIPT_BYTES];data=image[SCRIPT_BYTES:-24]
    if image[-24:]!=b'12345678'+header[:16]:raise ValueError('stock trailer mismatch')
    check_ubifs(data)
    if header!=script_for(data).ljust(SCRIPT_BYTES,b'\0'):
        raise ValueError('script, payload checksum or permitted write scope mismatch')
    return {'format':'ZT30 plaintext U-Boot/UBIFS development candidate',
        'installable':False,'loader_qualified':False,'unit_specific':True,
        'write_scope':['ubi0:customer'],'preserves':['rootfs','MISC','miservice','kernel','bootloader','environment'],
        'ubi_volume_lebs':CUSTOMER_LEBS,'ubifs_max_lebs':CUSTOMER_FS_MAX_LEBS,
        'payload_offset':SCRIPT_BYTES,'payload_bytes':len(data),'payload_sha256':sha(data),
        'container_bytes':len(image),'container_sha256':sha(image)}


def install_file(source,dest,mode):
    # Replace directory entries; never follow an existing destination symlink.
    temporary=dest.with_name(dest.name+'.apcg-new')
    with temporary.open('xb') as f:f.write(source.read_bytes())
    temporary.chmod(mode);os.replace(temporary,dest)


def build(args):
    if not args.output.name.endswith('.candidate'):
        raise ValueError('unqualified output must end in .candidate')
    baseline=args.customer_root.resolve()
    if not baseline.is_dir() or not (baseline/'demo.sh').is_file():raise ValueError('complete captured customer root required')
    old_demo=(baseline/'demo.sh').read_text()
    marker='chmod a+x /customer/siyi_app_359'
    if old_demo.count(marker)!=1:raise ValueError('baseline platform/startup split is ambiguous')
    platform=old_demo.split(marker,1)[0]
    if 'siyi_app' in platform or 'app_restore' in platform:raise ValueError('vendor app present in platform setup')
    args.output.parent.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='zt30-package-',dir=args.output.parent) as temporary:
        temporary=Path(temporary);customer=temporary/'customer'
        shutil.copytree(baseline,customer,symlinks=True)
        app=customer/'camera-app'
        if app.is_symlink():raise ValueError('baseline app directory is a symlink')
        app.mkdir(exist_ok=True)
        app.chmod(0o755)
        sources={'camera-app':args.camera_app,'zt30-web':args.web_app,'encoder-sdk.so':args.sdk,
            'dropbear':args.dropbear,'dropbearkey':args.dropbearkey,
            'zt30-slot':getattr(args,'slot_tool',ROOT/'build/zt30/zt30-slot'),
            'app-init.sh':ROOT/'packaging/zt30/app-init.sh',
            'app-supervisor.sh':ROOT/'packaging/zt30/app-supervisor.sh',
            'web-supervisor.sh':ROOT/'packaging/zt30/web-supervisor.sh',
            'start-dropbear.sh':ROOT/'packaging/zt30/start-dropbear.sh',
            'camera.ini.default':ROOT/'packaging/zt30/camera.ini.dev'}
        for name,source in sources.items():
            if not source.is_file():raise ValueError('missing input: '+str(source))
            install_file(source,app/name,0o755 if name!='camera.ini.default' else 0o644)
        # The common web server serves external assets, just like MT11/A8/ZR10.
        assets=app/'webroot'
        if assets.is_symlink():raise ValueError('webroot directory is a symlink')
        assets.mkdir(exist_ok=True)
        for source in sorted((ROOT/'web/webroot').rglob('*')):
            if source.is_file():
                dest=assets/source.relative_to(ROOT/'web/webroot')
                dest.parent.mkdir(parents=True,exist_ok=True)
                install_file(source,dest,0o644)
        for directory in [assets] + [p for p in assets.rglob('*') if p.is_dir()]:
            directory.chmod(0o755)
        # Initial credential only; existing user-chosen credentials are retained.
        default=app/'web.pass.default'
        if default.is_symlink():raise ValueError('credential default is a symlink')
        default.write_text('ardupilot\n');default.chmod(0o600)
        if args.authorized_keys:
            auth=app/'auth'
            if auth.is_symlink():raise ValueError('auth directory is a symlink')
            auth.mkdir(exist_ok=True);auth.chmod(0o700)
            install_file(args.authorized_keys,auth/'authorized_keys',0o600)
        # SDK and calibration/platform files remain available, but no vendor
        # app, vendor restart loop, BOA or thermal helper is launched.
        demo=customer/'demo.sh'
        demo.unlink()
        demo.write_text('#!/bin/sh\n'+platform+'\nexec /customer/camera-app/app-init.sh\n');demo.chmod(0o755)
        flag=customer/'.sys_upgrade_finish_flag'
        if flag.is_symlink():raise ValueError('startup flag is a symlink')
        flag.touch()
        if args.overlay_output:
            args.overlay_output.parent.mkdir(parents=True,exist_ok=True)
            def root_owned(info):
                info.uid=info.gid=0;info.uname=info.gname='root'
                return info
            with tarfile.open(args.overlay_output,'w:gz') as archive:
                archive.add(app,arcname='camera-app',filter=root_owned)
                archive.add(demo,arcname='demo.sh',filter=root_owned)
            manifest={str(f.relative_to(customer)):{'sha256':sha(f.read_bytes()),'mode':f.stat().st_mode&0o777}
                for f in [demo]+sorted(app.rglob('*')) if f.is_file()}
            args.overlay_output.with_suffix(args.overlay_output.suffix+'.json').write_text(json.dumps({
                'original_demo_sha256':sha(old_demo.encode()),'overlay_sha256':sha(args.overlay_output.read_bytes()),
                'files':manifest,'scope':'Unit-specific AP-only customer filesystem development overlay'},indent=2)+'\n')
        payload=temporary/'customer.ubifs'
        subprocess.run([args.mkfs,'-r',str(customer),'-m',str(MIN_IO),'-e',str(LEB_BYTES),
            '-c',str(CUSTOMER_FS_MAX_LEBS),'-U','-o',str(payload)],check=True)
        data=payload.read_bytes();image=pack(data);report=inspect(image)
        report['inputs']={name:{'path':str(path),'sha256':sha(path.read_bytes())} for name,path in sources.items()}
        report['baseline_demo_sha256']=sha(old_demo.encode())
        report['baseline_customer_root']=str(baseline)
        report['limitations']=['Device-specific image, not a distributable release',
            'Custom-script acceptance and fail-fast loading/write behavior not qualified',
            'Boot-time payload verification not implemented: U-Boot verification and failure handling not qualified',
            'Interrupted customer-volume recovery and vendor reinstall not tested',
            'Initial app supports one RGB stream; other native features remain incomplete']
        args.output.write_bytes(image)
        args.output.with_suffix(args.output.suffix+'.json').write_text(json.dumps(report,indent=2)+'\n')
        args.output.with_suffix(args.output.suffix+'.script.txt').write_bytes(script_for(data))
        print(json.dumps(report,indent=2))


def main():
    p=argparse.ArgumentParser(description=__doc__);sub=p.add_subparsers(dest='command',required=True)
    check=sub.add_parser('inspect');check.add_argument('image',type=Path)
    b=sub.add_parser('build')
    for name in ('customer-root','camera-app','web-app','sdk','dropbear','dropbearkey','output'):
        b.add_argument('--'+name,type=Path,required=True)
    b.add_argument('--overlay-output',type=Path)
    b.add_argument('--authorized-keys',type=Path);b.add_argument('--mkfs',default='mkfs.ubifs')
    args=p.parse_args()
    if args.command=='inspect':print(json.dumps(inspect(args.image.read_bytes()),indent=2))
    else:build(args)
if __name__=='__main__':main()
