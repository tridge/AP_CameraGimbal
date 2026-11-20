#!/usr/bin/env python3
"""Check ZT30 ARMv7 ELF and version requirements; optionally resolve its runtime."""
import argparse, json, os, re, subprocess
from pathlib import Path


def elf_info(path,readelf):
    return subprocess.check_output([readelf,'-h','-l','-d','-A','--version-info',str(path)],text=True)


def needed_versions(info):
    section=info.split("Version needs section",1)
    if len(section)==1:return {}
    result={};library=None
    for line in section[1].splitlines():
        file=re.search(r'File: (\S+)',line)
        if file:library=file.group(1);result.setdefault(library,set())
        version=re.search(r'Name: (\S+)',line)
        if version and library:result[library].add(version.group(1))
    return result


def captured_path(root,path):
    # Resolve every component using device-root semantics; absolute symlinks
    # belong inside the captured root and never refer to the host filesystem.
    pending=list(Path(path.lstrip('/')).parts);parts=[];links=0
    while pending:
        component=pending.pop(0)
        if component in ('','.'):continue
        if component=='..':
            if not parts:raise ValueError('runtime symlink escapes captured root')
            parts.pop();continue
        candidate=root.joinpath(*parts,component)
        if candidate.is_symlink():
            links+=1
            if links>40:raise ValueError('runtime symlink loop')
            target=os.readlink(candidate)
            if target.startswith('/'):parts=[]
            pending=list(Path(target.lstrip('/')).parts)+pending
        else:parts.append(component)
    return root.joinpath(*parts)


def runtime_library(root,name,library_path):
    if '/' in name:raise ValueError(f'non-soname dependency not supported: {name}')
    for directory in library_path:
        if not directory.startswith('/') or '$' in directory:raise ValueError(f'unsupported device library path: {directory}')
        candidate=captured_path(root,directory+'/'+name)
        if candidate.is_file():return candidate
    raise ValueError(f'runtime library missing: {name}')


def version_exports(info):
    section=info.split('Version definition section',1)
    return set(re.findall(r'Name: (\S+)',section[1].split('Version needs section')[0])) if len(section)>1 else set()


def check(path,readelf='readelf',runtime_root=None,library_path=('/lib','/usr/lib'),load_libraries=(),shared_library=False):
    info=elf_info(path,readelf)
    required={'ELF class':r'Class:\s+ELF32\b','machine':r'Machine:\s+ARM\b',
              'byte order':r'Data:.*little endian','hard-float ABI':r'Flags:.*hard-float ABI',
              'interpreter':r'Requesting program interpreter: /lib/ld-linux-armhf\.so\.3\]',
              'CPU architecture':r'Tag_CPU_arch:\s+v7\b',
              'CPU profile':r'Tag_CPU_arch_profile:\s+Application',
              'VFP calling convention':r'Tag_ABI_VFP_args:\s+VFP registers'}
    if shared_library:
        required.pop('interpreter')
        required['shared library type']=r'Type:\s+DYN\b'
        if 'Requesting program interpreter:' in info:raise ValueError('shared library must not have an interpreter')
    missing=[key for key,pattern in required.items() if not re.search(pattern,info)]
    if missing:raise ValueError(f'{path}: incompatible ARM ELF: {missing}')
    if re.search(r'\((?:RUNPATH|RPATH)\)',info):raise ValueError('RPATH/RUNPATH requires separate loader qualification')
    needed=re.findall(r'Shared library: \[([^\]]+)\]',info)
    versions=needed_versions(info)
    # Ceilings are exports of the captured glibc2.30/GCC9 runtime, not the compiler.
    ceilings={'GLIBC':(2,30),'GLIBCXX':(3,4,26),'CXXABI':(1,3,12),'GCC':(7,0,0)}
    for library,requirements in versions.items():
        for version in requirements:
            match=re.fullmatch(r'(GLIBC|GLIBCXX|CXXABI|GCC)_([0-9.]+)',version)
            if match and tuple(map(int,match.group(2).split('.')))>ceilings[match.group(1)]:
                raise ValueError(f'{path}: exceeds {match.group(1)} runtime ceiling: {version}')
            if version.endswith('_PRIVATE'):raise ValueError(f'{path}: private runtime version: {version}')
    if load_libraries and not runtime_root:raise ValueError('dlopen library qualification requires runtime-root')
    dependencies=[]
    if runtime_root:
        root=Path(os.path.abspath(runtime_root))
        pending=list(needed)+list(load_libraries);seen=set()
        # Verify transitive DT_NEEDED and exact version definitions in captured libs.
        while pending:
            name=pending.pop()
            if name in seen:continue
            seen.add(name);dep=runtime_library(root,name,library_path);data=elf_info(dep,readelf)
            if re.search(r"\((?:RUNPATH|RPATH)\)",data):raise ValueError(f"{name}: RPATH/RUNPATH requires separate qualification")
            missing=[key for key in ('ELF class','machine','byte order','hard-float ABI') if not re.search(required[key],data)]
            if missing:raise ValueError(f'{name}: incompatible runtime ELF: {missing}')
            exports=version_exports(data)
            absent=versions.get(name,set())-exports
            if absent:raise ValueError(f'{name}: missing runtime versions: {sorted(absent)}')
            dependencies.append({'name':name,'path':str(dep),'checked_versions':sorted(versions.get(name,set()))})
            for child,requirements in needed_versions(data).items():
                versions.setdefault(child,set()).update(requirements)
            pending+=re.findall(r'Shared library: \[([^\]]+)\]',data)
        unresolved=set(versions)-seen
        if unresolved:raise ValueError(f'unqualified version-needs library aliases: {sorted(unresolved)}')
        # Requirements discovered after a dependency was visited must also be checked.
        for dep in dependencies:
            data=elf_info(Path(dep['path']),readelf)
            exports=version_exports(data)
            absent=versions.get(dep['name'],set())-exports
            if absent:raise ValueError(f'{dep["name"]}: missing transitive runtime versions: {sorted(absent)}')
    glibc=[tuple(map(int,v.split('_',1)[1].split('.'))) for reqs in needed_versions(info).values() for v in reqs if re.fullmatch(r'GLIBC_[0-9.]+',v)]
    return {'path':str(path),'elf':'ARMv7 ELF32 EABI hard-float little endian',
            'interpreter':None if shared_library else '/lib/ld-linux-armhf.so.3','glibc_max':'.'.join(map(str,max(glibc))) if glibc else None,
            'needed':needed,'load_libraries':list(load_libraries),'runtime_dependencies':dependencies,
            'library_path':list(library_path),'runtime_checked':bool(runtime_root),'limitation':'MI signatures/field meanings and live hardware behavior require separate qualification'}

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('binary',type=Path);p.add_argument('--readelf',default='readelf');p.add_argument('--runtime-root',type=Path);p.add_argument('--library-path',default='/lib:/usr/lib',help='absolute device search directories in loader order');p.add_argument('--load-library',action='append',default=[],help='additional dlopen soname to qualify');p.add_argument('--shared-library',action='store_true',help='require DYN without a program interpreter');a=p.parse_args()
    try:print(json.dumps(check(a.binary,a.readelf,a.runtime_root,tuple(a.library_path.split(':')),a.load_library,a.shared_library),indent=2))
    except (ValueError,subprocess.CalledProcessError) as e:raise SystemExit(str(e))
