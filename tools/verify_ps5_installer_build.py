"""Reject a console artifact that contains the upstream host installer branch."""
import argparse
import json
import re
import subprocess
from pathlib import Path


def inspect(objdump, path, function):
    return subprocess.check_output(
        [str(objdump), '-dr', '--disassemble-symbols=' + function, str(path)],
        text=True, encoding='utf-8', errors='replace')


def verify(build, llvm, installer_object=None):
    objdump = llvm / 'llvm-objdump.exe'
    worker = inspect(objdump, installer_object or build / 'installer.o', 'stream_installer_worker')
    required = ('install_service_start', 'install_service_status', 'install_service_close')
    for name in required:
        if not re.search(r'R_X86_64_\w+\s+' + name + r'(?:-|\s|$)', worker):
            raise ValueError('native worker call missing: ' + name)
    if re.search(r'R_X86_64_\w+\s+virtual_stream_read(?:-|\s|$)', worker):
        raise ValueError('host mock virtual_stream_read call found in installer worker')
    helper = inspect(objdump, build / 'install_helper_upstream.o', 'install_helper_serve')
    for name in ('sceAppInstUtilInitialize', 'sceAppInstUtilInstallByPackage',
                 'sceAppInstUtilGetInstallStatus', 'sceAppInstUtilTerminate'):
        if name not in helper:
            raise ValueError('helper native call missing: ' + name)
    stack = {}
    for name in ('installer.su', 'stream_server.su', 'phstore_install.su', 'phstr_main.su'):
        for line in (build / name).read_text().splitlines():
            location, size, kind = line.split('\t')
            if int(size) > 65536 or kind != 'static':
                raise ValueError('unbounded or oversized stack frame: ' + line)
            if location.endswith(':stream_installer_worker') or location.endswith(':serve_connection'):
                stack[location.rsplit(':', 1)[-1]] = int(size)
    if 'stream_installer_worker' not in stack:
        raise ValueError('worker stack measurement missing')
    elf = (build / 'phstr.elf').read_bytes()
    helper_elf = (build / 'install-helper.elf').read_bytes()
    if elf[:4] != b'\x7fELF' or helper_elf[:4] != b'\x7fELF':
        raise ValueError('output is not ELF')
    if helper_elf not in elf:
        raise ValueError('embedded helper differs from the helper built in this run')
    return {'native_worker_calls': list(required), 'host_mock_read_absent': True,
            'helper_native_calls': True, 'embedded_helper_matches': True,
            'stack_frames_bytes': stack}


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--build-dir', type=Path, default=Path('build'))
    parser.add_argument('--llvm-bin', type=Path, default=Path(r'C:\Program Files\LLVM\bin'))
    parser.add_argument('--installer-object', type=Path)
    args = parser.parse_args()
    result = verify(args.build_dir, args.llvm_bin, args.installer_object)
    print(json.dumps(result, indent=2))
