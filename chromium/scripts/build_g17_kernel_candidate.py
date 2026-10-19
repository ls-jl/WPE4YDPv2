#!/usr/bin/env python3
"""Build a coherent private candidate kernel and Mali module; never install it."""
import argparse
import datetime
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tarfile
import tempfile

import compile_g17_candidate as policy

SUFFIX = '-pen-managed'
REQUIRED = {'dev_pm_opp_check_regulator_release', 'dev_pm_opp_manage_regulators',
            'dev_pm_opp_finish_managed_regulators'}


def extract_source(archive, destination):
    """Only a new real directory may receive the checked Git archive."""
    if destination.exists() or destination.is_symlink():
        raise ValueError('Source destination must not exist')
    destination.mkdir()
    with tarfile.open(archive) as stream:
        stream.extractall(destination, filter='data')


def verify_config(text):
    fields = dict(line.split('=', 1) for line in text.splitlines()
                  if line.startswith('CONFIG_') and '=' in line)
    required = dict(CONFIG_LOCALVERSION='"' + SUFFIX + '"', CONFIG_MALI_BIFROST='m',
                    CONFIG_OF='y', CONFIG_REGULATOR='y', CONFIG_PM_OPP='y',
                    CONFIG_MALI_BIFROST_DEVFREQ='y', CONFIG_MODULES='y')
    if any(fields.get(key) != value for key, value in required.items()):
        raise ValueError('Candidate config lost managed-API/module/release requirements')
    if fields.get('CONFIG_LOCALVERSION_AUTO') == 'y':
        raise ValueError('Uncontrolled automatic kernel release')
    return {key: fields[key] for key in required}


def verify_symbols(symvers, readelf):
    exports = {}
    for line in symvers.splitlines():
        row = line.split()
        if len(row) < 4 or not row[0].startswith('0x') or row[1] in exports:
            raise ValueError('Malformed or duplicate Module.symvers export')
        exports[row[1]] = dict(owner=row[2], kind=row[3], crc=row[0])
    if any(name not in exports or exports[name]['owner'] != 'vmlinux' or
           exports[name]['kind'] != 'EXPORT_SYMBOL_GPL' for name in REQUIRED):
        raise ValueError('Managed APIs are not exported by this built kernel')
    imports = set()
    for line in readelf.splitlines():
        row = line.split()
        if len(row) >= 8 and row[6] == 'UND' and row[4] == 'GLOBAL':
            imports.add(row[7])
    policy.check_managed_imports(imports)
    if imports - exports.keys():
        raise ValueError('Module imports missing from coherent build: ' + ', '.join(sorted(imports - exports.keys())))
    return {name: exports[name] for name in sorted(imports)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, required=True)
    parser.add_argument('--config', type=Path, required=True)
    parser.add_argument('--gcc-root', type=Path, required=True)
    parser.add_argument('--gcc-archive', type=Path, required=True)
    parser.add_argument('--build-root', type=Path, default=Path('/srv/wpe-build/research/mali-g17'))
    parser.add_argument('--jobs', type=int, default=6)
    args = parser.parse_args()
    if not 1 <= args.jobs <= 64:
        parser.error('--jobs must be in 1..64')
    source, root = args.source_root.resolve(), args.build_root.resolve()
    if root.is_relative_to(source):
        raise ValueError('Build root must be outside the source tree')

    def git(*command):
        return subprocess.check_output(['git', '-C', str(source), *command], text=True).strip()

    if git('rev-parse', 'HEAD') != policy.REVISION or git('status', '--porcelain', '--untracked-files=all'):
        raise ValueError('Requires the exact clean non-Android reference tree')
    if policy.sha(args.config) != policy.CONFIG_SHA:
        raise ValueError('Not the inventoried preparation config')
    if (source / '.config').exists() or (source / 'include/config/auto.conf').exists():
        raise ValueError('Reference source has generated config; never clean it here')
    if not root.is_dir() or shutil.disk_usage(root).free < 8 * 1024**3:
        raise ValueError('Existing build root needs at least 8GiB free')
    gcc = policy.verify_gcc_tree(args.gcc_archive, args.gcc_root)
    tools = {name: shutil.which(name) for name in ('make', 'cc', 'bc', 'flex', 'bison', 'modinfo')}
    if not all(tools.values()):
        raise ValueError('Missing host tools: ' + ', '.join(key for key, value in tools.items() if not value))
    prefix = str(args.gcc_root.resolve() / 'bin/aarch64-none-linux-gnu-')
    patches = [policy.PROJECT / 'chromium/kernel-patches' / name for name in policy.ROCKCHIP_OPP_PATCHES]
    patches += [policy.PROJECT / 'chromium/driver-patches' / name for name in policy.MANAGED_DRIVER_PATCHES]
    inputs = dict(revision=policy.REVISION, config_sha256=policy.sha(args.config),
        controller_sha256=policy.sha(Path(__file__)), policy_sha256=policy.sha(Path(policy.__file__)),
        gcc_tree=gcc, patches={path.name: policy.sha(path) for path in patches}, jobs=args.jobs,
        tools={name: dict(path=path, sha256=policy.sha(Path(path))) for name, path in tools.items()})
    output = Path(tempfile.mkdtemp(prefix='coherent-kernel-', dir=root))
    print(output, flush=True)
    log_path = output / 'build.log'
    receipt = dict(kind='private-coherent-managed-kernel', inputs=inputs, output=str(output),
        outcome='preparing', installed=False, stock_kernel_abi_accepted=False,
        hardware_accepted=False, public_release_accepted=False)
    environment = dict(os.environ)
    for key in tuple(environment):
        if key.startswith(('KBUILD_', 'KCONFIG_', 'CROSS_COMPILE', 'LLVM')) or key in (
            'MAKEFLAGS', 'MFLAGS', 'KCFLAGS', 'KCPPFLAGS', 'HOSTCFLAGS', 'HOSTLDFLAGS',
            'LD_PRELOAD', 'LD_LIBRARY_PATH', 'ARCH', 'SUBARCH', 'LOCALVERSION'):
            environment.pop(key, None)
    epoch = int(git('show', '-s', '--format=%ct', policy.REVISION))
    environment.update(KBUILD_BUILD_TIMESTAMP=datetime.datetime.fromtimestamp(epoch, datetime.timezone.utc).strftime('%a %b %d %H:%M:%S UTC %Y'),
        KBUILD_BUILD_USER='pen-builder', KBUILD_BUILD_HOST='isolated', KBUILD_BUILD_VERSION='1',
        SOURCE_DATE_EPOCH=str(epoch), LC_ALL='C')
    commands = []
    try:
        with log_path.open('w') as log:
            def run(command, cwd=output):
                commands.append(command)
                log.write(json.dumps(command) + '\n'); log.flush()
                subprocess.run(command, cwd=cwd, env=environment, stdout=log, stderr=subprocess.STDOUT, check=True)
            archive = output / 'source.tar'
            run(['git', '-C', str(source), 'archive', '--format=tar', '--output=' + str(archive), policy.REVISION])
            receipt['source_archive_sha256'] = policy.sha(archive)
            tree = output / 'source'
            extract_source(archive, tree)
            archive.unlink()
            for patch in patches:
                run(['git', 'apply', '--check', '--whitespace=error-all', str(patch)], tree)
                run(['git', 'apply', '--whitespace=error-all', str(patch)], tree)
            kernel = output / 'kernel-out'; kernel.mkdir()
            shutil.copy2(args.config, kernel / '.config')
            run([str(tree / 'scripts/config'), '--file', str(kernel / '.config'), '-m', 'MALI_BIFROST',
                 '--set-str', 'LOCALVERSION', SUFFIX, '-d', 'LOCALVERSION_AUTO'])
            cc = shlex.join([str(tree / 'scripts/gcc-wrapper.py'), prefix + 'gcc'])
            make = [tools['make'], '-C', str(tree), 'O=' + str(kernel), 'ARCH=arm64',
                    'CROSS_COMPILE=' + prefix, 'CC=' + cc, 'HOSTCC=' + tools['cc']]
            run([*make, 'olddefconfig'])
            receipt['config_requirements'] = verify_config((kernel / '.config').read_text())
            receipt['outcome'] = 'building'
            run([*make, '-j' + str(args.jobs), 'Image', 'modules'])
            module = kernel / policy.BIFROST / 'bifrost_kbase.ko'
            header = subprocess.check_output([prefix + 'readelf', '-h', str(module)], text=True)
            if 'AArch64' not in header or 'REL (Relocatable file)' not in header:
                raise ValueError('Not an AArch64 loadable-module ELF')
            release = (kernel / 'include/config/kernel.release').read_text().strip()
            if release != '5.10.160' + SUFFIX:
                raise ValueError('Unexpected or stock-confusable candidate kernel release')
            vermagic = subprocess.check_output([tools['modinfo'], '-F', 'vermagic', str(module)], text=True).strip()
            if vermagic.split()[0] != release:
                raise ValueError('Module vermagic does not match built candidate')
            receipt['resolved_imports'] = verify_symbols((kernel / 'Module.symvers').read_text(),
                subprocess.check_output([prefix + 'readelf', '-Ws', str(module)], text=True))
            artifacts = [kernel / name for name in ('.config', 'vmlinux', 'System.map', 'Module.symvers',
                'modules.order', 'arch/arm64/boot/Image')]
            artifacts += sorted(kernel.rglob('*.ko'))
            receipt.update(outcome='linked-coherent-candidate', release=release, vermagic=vermagic,
                artifacts={str(path.relative_to(output)): dict(sha256=policy.sha(path), bytes=path.stat().st_size) for path in artifacts},
                kbuild_commands={str(path.relative_to(output)): policy.sha(path) for path in sorted(kernel.rglob('*.cmd'))})
    except BaseException as error:
        receipt.update(outcome='failed', error=str(error))
        raise
    finally:
        receipt.update(commands=commands, log_sha256=policy.sha(log_path) if log_path.exists() else None)
        receipt['source_unchanged'] = (git('rev-parse', 'HEAD') == policy.REVISION and
            not git('status', '--porcelain', '--untracked-files=all') and policy.sha(args.config) == inputs['config_sha256'])
        receipt['build_inputs_unchanged'] = (policy.sha(Path(__file__)) == inputs['controller_sha256'] and
            policy.sha(Path(policy.__file__)) == inputs['policy_sha256'] and
            all(policy.sha(path) == inputs['patches'][path.name] for path in patches) and
            all(policy.sha(Path(row['path'])) == row['sha256'] for row in inputs['tools'].values()) and
            policy.verify_gcc_tree(args.gcc_archive, args.gcc_root) == gcc)
        path = output / 'receipt.json'
        path.write_text(json.dumps(receipt, indent=2) + '\n'); path.chmod(0o600)
    if not receipt['source_unchanged'] or not receipt['build_inputs_unchanged']:
        raise RuntimeError('Inputs changed during full build; do not promote')


if __name__ == '__main__':
    main()
