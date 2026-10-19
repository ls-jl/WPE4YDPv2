#!/usr/bin/env python3
"""Compile source-only Mali fixes; never produce, stage or load a kernel module."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import shlex
import subprocess
import tarfile
import tempfile

PROJECT = Path(__file__).resolve().parents[2]
REVISION = 'acdf68dc443a7c9a68be769abc6cf4d85617f283'
CONFIG_SHA = 'be1316e388224bbdb7c44005e7fcacbee34949d98909f337a9149b7cde777076'
BIFROST = Path('drivers/gpu/arm/bifrost')
PATCHES = ('0001-g17-balance-opp-helper-reference.patch',
           '0002-g17-unwind-platform-power-on-failures.patch',
           '0003-g17-own-opp-regulator-enables.patch',
           '0004-g17-unwind-prepared-clock-references.patch',
           '0005-g17-drain-platform-power-before-detach.patch',
           '0006-g17-track-runtime-pm-and-drain-suspend-work.patch',
           '0007-g17-drain-and-unregister-per-device-monitor.patch',
           '0008-g17-account-platform-regulator-enables.patch',
           '0009-g17-retain-failed-platform-regulator-returns.patch',
           '0010-g17-retain-opp-rollback-outside-kbdev.patch')
OPP_PATCHES = ('0001-opp-quarantine-failed-exclusive-regulator-release.patch',
               '0002-opp-manage-registration-ownership-before-io.patch')
ROCKCHIP_OPP_PATCHES = (*OPP_PATCHES,
    '0003-rockchip-release-opp-initialization-references.patch',
    '0004-rockchip-bound-calibration-snapshots-and-error-lookups.patch')
ROCKCHIP_OPP = Path('drivers/soc/rockchip')
G17_TARGETS = ('g17', 'g17-managed')
MANAGED_DRIVER_PATCHES = (*PATCHES, '0011-g17-use-core-owned-opp-registration.patch')
TARGET_MODULES = dict(g17=BIFROST, opp=Path('drivers/opp'),
    **{'rockchip-opp': ROCKCHIP_OPP, 'g17-managed': BIFROST})
TARGET_PATCHES = dict(g17=PATCHES, opp=OPP_PATCHES, **{'rockchip-opp': ROCKCHIP_OPP_PATCHES})
TARGET_OBJECTS = dict(g17='bifrost_kbase.o', opp='core.o',
    **{'rockchip-opp': 'rockchip_opp_select.o', 'g17-managed': 'bifrost_kbase.o'})
GCC_ARCHIVE_SHA = '1e33d53dea59c8de823bbdfe0798280bdcd138636c7060da9d77a97ded095a84'
GCC_PREFIX = 'gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu'


def sha(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def stage_module_inputs(source, module_root, target):
    """Kernel-integrated targets stage private headers, never through symlinks."""
    if target not in TARGET_MODULES:
        raise ValueError('Unknown compile-only target')
    if module_root.resolve().is_relative_to(source.resolve()) or module_root.exists() or module_root.is_symlink():
        raise RuntimeError('Module staging must be new and outside the source tree')
    new_header = source / 'include/linux/pm_opp_safe.h'
    if target != 'g17' and (new_header.exists() or new_header.is_symlink()):
        raise RuntimeError('OPP reference already contains the candidate header')
    relative = TARGET_MODULES[target]
    module = module_root / relative
    module.parent.mkdir(parents=True)
    shutil.copytree(source / relative, module, symlinks=True)
    if target == 'g17':
        (module_root / 'include').symlink_to(source / 'include', target_is_directory=True)
    else:
        (module_root / 'include').mkdir()
    if target in ('rockchip-opp', 'g17-managed'):
        shutil.copytree(source / 'drivers/opp', module_root / 'drivers/opp', symlinks=True)
        if target == 'g17-managed':
            (module_root / ROCKCHIP_OPP).parent.mkdir(parents=True)
            shutil.copytree(source / ROCKCHIP_OPP, module_root / ROCKCHIP_OPP, symlinks=True)
        for name in ('clk', 'devfreq'):
            (module_root / 'drivers' / name).symlink_to(source / 'drivers' / name, target_is_directory=True)
    (module_root / 'drivers/base').symlink_to(source / 'drivers/base', target_is_directory=True)
    return module


def check_managed_imports(symbols):
    required = {'dev_pm_opp_manage_regulators', 'dev_pm_opp_finish_managed_regulators',
                'dev_pm_opp_check_regulator_release'}
    forbidden = {'dev_pm_opp_set_regulators', 'dev_pm_opp_put_regulators',
                 'dev_pm_opp_register_set_opp_helper', 'dev_pm_opp_unregister_set_opp_helper'}
    if required - symbols or forbidden & symbols:
        raise RuntimeError('Managed driver has missing new APIs or legacy ownership imports')
    return sorted(required)


def verify_gcc_tree(archive, root):
    """Read-only verification against every member of the pinned Arm archive."""
    if sha(archive) != GCC_ARCHIVE_SHA:
        raise RuntimeError('GCC archive checksum does not match the pinned Arm release')
    root = root.resolve()
    expected = {}
    with tarfile.open(archive) as stream:
        for member in stream.getmembers():
            name = Path(member.name)
            if name.is_absolute() or '..' in name.parts or name.parts[0] != GCC_PREFIX:
                raise RuntimeError('Unexpected toolchain archive path')
            relative = Path(*name.parts[1:])
            path = root / relative
            if not path.resolve().is_relative_to(root):
                raise RuntimeError('Toolchain member escapes its root: ' + str(relative))
            if member.isdir():
                if not path.is_dir() or path.is_symlink():
                    raise RuntimeError('Missing toolchain directory: ' + str(relative))
                expected[str(relative)] = dict(kind='directory')
            elif member.issym():
                if not path.is_symlink() or str(path.readlink()) != member.linkname:
                    raise RuntimeError('Toolchain symlink differs: ' + str(relative))
                expected[str(relative)] = dict(kind='symlink', target=member.linkname)
            elif member.isfile() or member.islnk():
                if not path.is_file() or path.is_symlink():
                    raise RuntimeError('Missing toolchain file: ' + str(relative))
                digest = hashlib.sha256()
                with stream.extractfile(member) as content:
                    for block in iter(lambda: content.read(1024 * 1024), b''):
                        digest.update(block)
                if sha(path) != digest.hexdigest() or path.stat().st_mode & 0o111 != member.mode & 0o111:
                    raise RuntimeError('Toolchain file content/mode differs: ' + str(relative))
                expected[str(relative)] = dict(kind='file', sha256=digest.hexdigest(), executable=member.mode & 0o111)
            else:
                raise RuntimeError('Unsupported toolchain member type: ' + str(relative))
    extras = {str(path.relative_to(root)) for path in root.rglob('*')} - set(expected)
    if extras:
        raise RuntimeError('Uninventoried toolchain files: ' + ', '.join(sorted(extras)[:5]))
    return dict(archive_sha256=GCC_ARCHIVE_SHA, root=str(root), members=expected)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, required=True)
    parser.add_argument('--config', type=Path, required=True)
    parser.add_argument('--build-root', type=Path, default=Path('/srv/wpe-build/research/mali-g17'))
    parser.add_argument('--jobs', type=int, default=6)
    parser.add_argument('--gcc-root', type=Path)
    parser.add_argument('--gcc-archive', type=Path)
    parser.add_argument('--unpatched', action='store_true')
    parser.add_argument('--target', choices=tuple(TARGET_MODULES), default='g17')
    parser.add_argument('--historical-config', action='store_true')
    args = parser.parse_args()
    source = args.source_root.resolve()
    config = args.config.resolve()
    if not 1 <= args.jobs <= 64:
        parser.error('--jobs must be in 1..64')
    if bool(args.gcc_root) != bool(args.gcc_archive):
        parser.error('--gcc-root and --gcc-archive must be provided together')
    if args.historical_config and not args.gcc_root:
        parser.error('--historical-config requires the pinned GCC toolchain')
    if args.build_root.resolve().is_relative_to(source):
        raise RuntimeError('Build root must be outside the source tree')

    def git(*command):
        return subprocess.check_output(['git', '-C', str(source), *command], text=True).strip()

    if git('rev-parse', 'HEAD') != REVISION or git('status', '--porcelain', '--untracked-files=no'):
        raise RuntimeError('Requires the clean pinned migrated source, not another kernel tree')
    if sha(config) != CONFIG_SHA:
        raise RuntimeError('Not the inventoried preparation config; stock ABI remains unverified')
    if (source / '.config').exists() or (source / 'include/config/auto.conf').exists():
        raise RuntimeError('Source tree contains generated config; do not clean or overwrite it')
    gcc = verify_gcc_tree(args.gcc_archive, args.gcc_root) if args.gcc_root else None
    tools = ('git', 'make', 'cc', 'readelf', 'flex', 'bison')
    paths = {tool: shutil.which(tool) for tool in tools}
    target_names = dict(CC='gcc', LD='ld', AS='as', AR='ar', NM='nm', OBJCOPY='objcopy',
                        OBJDUMP='objdump', STRIP='strip')
    if gcc:
        prefix = str(args.gcc_root.resolve() / 'bin/aarch64-none-linux-gnu-')
        paths.update({name: prefix + tool for name, tool in target_names.items()})
    else:
        prefix = 'aarch64-linux-gnu-'
        paths.update({name: shutil.which('clang-18' if name in ('CC', 'AS') else
            'ld.lld-18' if name == 'LD' else 'llvm-' + tool + '-18') for name, tool in target_names.items()})
    missing = [tool for tool, path in paths.items() if not path]
    if missing:
        raise RuntimeError('Missing host tools: ' + ', '.join(missing))
    if shutil.disk_usage(args.build_root).free < 1024 * 1024 * 1024:
        raise RuntimeError('Need at least 1GiB free under the explicit build root')
    relative_module = TARGET_MODULES[args.target]
    if args.target == 'g17-managed':
        patches = [PROJECT / 'chromium/kernel-patches' / name for name in ROCKCHIP_OPP_PATCHES]
        patches += [PROJECT / 'chromium/driver-patches' / name for name in MANAGED_DRIVER_PATCHES]
    else:
        directory = 'driver-patches' if args.target == 'g17' else 'kernel-patches'
        patches = [PROJECT / 'chromium' / directory / name for name in TARGET_PATCHES[args.target]]
    if args.unpatched:
        patches = []
    inputs = dict(revision=REVISION, config_sha256=sha(config), jobs=args.jobs,
                  compiler_profile='gcc103' if gcc else 'clang18', gcc_tree=gcc,
                  historical_config=args.historical_config, unpatched=args.unpatched, target=args.target,
                  controller_sha256=sha(Path(__file__)),
                  patches={path.name: sha(path) for path in patches}, tools={})
    for name, path in paths.items():
        inputs['tools'][name] = dict(path=str(Path(path).resolve()), sha256=sha(Path(path)),
            version=subprocess.check_output([path, '--version'], text=True).splitlines()[0])
    source_key = 'bifrost_sources' if args.target in G17_TARGETS else 'opp_sources'
    source_modules = [relative_module]
    if args.target in ('rockchip-opp', 'g17-managed'):
        source_modules.append(Path('drivers/opp'))
    if args.target == 'g17-managed':
        source_modules.append(ROCKCHIP_OPP)
    inputs[source_key] = {str(path.relative_to(source)): sha(path)
        for relative in source_modules for path in sorted((source / relative).rglob('*')) if path.is_file()}
    output = Path(tempfile.mkdtemp(prefix='compile-only-', dir=args.build_root))
    print(output, flush=True)
    module_root = output / 'module'
    module = stage_module_inputs(source, module_root, args.target)
    kernel_out = output / 'kernel-out'
    kernel_out.mkdir()
    shutil.copy2(config, kernel_out / '.config')
    # Keep modifications in the new module/config copies, never in migrated inputs.
    log_path = output / 'compile.log'
    receipt = dict(kind=args.target + '-source-compile-only', inputs=inputs, outcome='preparing',
        source_root=str(source), output=str(output), kernel_abi_accepted=False,
        hardware_accepted=False, opp_consumer_ownership_fixed=False)
    compiler = paths['CC']
    if args.historical_config:
        wrapper = source / 'scripts/gcc-wrapper.py'
        inputs['gcc_wrapper_sha256'] = sha(wrapper)
        compiler = shlex.join([str(wrapper), compiler])
    make = [paths['make'], '-C', str(source), 'O=' + str(kernel_out), 'ARCH=arm64',
        'CC=' + compiler, 'HOSTCC=' + paths['cc'], 'CROSS_COMPILE=' + prefix,
        'KCFLAGS=-fmacro-prefix-map=' + str(module_root) + '/=']
    if args.target != 'g17':
        make[-1] += ' -I' + str(module_root / 'include')
    make.extend(name + '=' + paths[name] for name in target_names if name != 'CC')
    if not gcc:
        make.append('LLVM_IAS=1')
    commands = []
    for patch in patches:
        commands.extend([['git', 'apply', '--check', '--whitespace=error-all', str(patch)],
                         ['git', 'apply', '--whitespace=error-all', str(patch)]])
    if not args.historical_config and args.target in G17_TARGETS:
        commands.append([str(source / 'scripts/config'), '--file', str(kernel_out / '.config'), '-m', 'MALI_BIFROST'])
    object_name = TARGET_OBJECTS[args.target]
    commands.extend([[*make, 'olddefconfig'], [*make, 'modules_prepare'],
                     [*make, 'M=' + str(module),
                      *(['CONFIG_MALI_BIFROST=m'] if args.target in G17_TARGETS else []),
                      '-j' + str(args.jobs), object_name]])
    if args.target in ('rockchip-opp', 'g17-managed'):
        commands.append([*make, 'M=' + str(module_root / 'drivers/opp'), '-j' + str(args.jobs), 'core.o'])
    if args.target == 'g17-managed':
        commands.append([*make, 'M=' + str(module_root / ROCKCHIP_OPP), '-j' + str(args.jobs), 'rockchip_opp_select.o'])
    environment = dict(os.environ)
    for key in ('KBUILD_OUTPUT', 'KCONFIG_CONFIG', 'KCFLAGS', 'KCPPFLAGS', 'MAKEFLAGS', 'MFLAGS',
                'LD_PRELOAD', 'LD_LIBRARY_PATH', 'LLVM', 'LLVM_IAS'):
        environment.pop(key, None)
    try:
        with log_path.open('w') as log:
            for command in commands:
                log.write(json.dumps(command) + '\n')
                log.flush()
                subprocess.run(command, cwd=module_root, env=environment,
                    stdout=log, stderr=subprocess.STDOUT, check=True)
        obj = module / object_name
        header = subprocess.check_output([paths['readelf'], '-h', str(obj)], text=True)
        if 'AArch64' not in header or 'REL (Relocatable file)' not in header:
            raise RuntimeError('Candidate is not an AArch64 relocatable object')
        if list(module_root.rglob('*.ko')):
            raise RuntimeError('Unexpected loadable module output')
        receipt.update(outcome='compiled-relocatable-only', object_sha256=sha(obj),
            generated_config_sha256=sha(kernel_out / '.config'), object_header=header,
            commands={str(path.relative_to(output)): sha(path)
                      for root in (kernel_out, module) for path in sorted(root.rglob('*.cmd'))})
        if args.target == 'g17-managed' and not args.unpatched:
            symbols = subprocess.check_output([paths['readelf'], '-Ws', str(obj)], text=True)
            undefined = {fields[7] for line in symbols.splitlines()
                         if len(fields := line.split()) >= 8 and fields[6] == 'UND'}
            receipt['managed_opp_imports'] = check_managed_imports(undefined)
            receipt['undefined_imports'] = sorted(undefined)
        if args.target in ('rockchip-opp', 'g17-managed'):
            companion = module_root / 'drivers/opp/core.o'
            companion_header = subprocess.check_output([paths['readelf'], '-h', str(companion)], text=True)
            if 'AArch64' not in companion_header or 'REL (Relocatable file)' not in companion_header:
                raise RuntimeError('Companion OPP core is not an AArch64 relocatable object')
            receipt['companion_opp_object_sha256'] = sha(companion)
            receipt['companion_opp_commands'] = {str(path.relative_to(output)): sha(path)
                for path in sorted((module_root / 'drivers/opp').rglob('*.cmd'))}
        if args.target == 'g17-managed':
            companion = module_root / ROCKCHIP_OPP / 'rockchip_opp_select.o'
            companion_header = subprocess.check_output([paths['readelf'], '-h', str(companion)], text=True)
            if 'AArch64' not in companion_header or 'REL (Relocatable file)' not in companion_header:
                raise RuntimeError('Companion Rockchip object is not an AArch64 relocatable object')
            receipt['companion_rockchip_object_sha256'] = sha(companion)
            receipt['companion_rockchip_commands'] = {str(path.relative_to(output)): sha(path)
                for path in sorted((module_root / ROCKCHIP_OPP).rglob('*.cmd'))}
    except Exception as error:
        receipt.update(outcome='failed', error=str(error))
        raise
    finally:
        receipt['log_sha256'] = sha(log_path)
        receipt['source_unchanged'] = (git('rev-parse', 'HEAD') == REVISION and
            not git('status', '--porcelain', '--untracked-files=no') and sha(config) == CONFIG_SHA and
            all(sha(source / name) == digest for name, digest in inputs[source_key].items()) and
            (args.target == 'g17' or not ((source / 'include/linux/pm_opp_safe.h').exists() or
                                        (source / 'include/linux/pm_opp_safe.h').is_symlink())))
        receipt['controller_patches_unchanged'] = (sha(Path(__file__)) == inputs['controller_sha256'] and
            all(sha(path) == inputs['patches'][path.name] for path in patches))
        receipt['toolchain_unchanged'] = (all(sha(Path(info['path'])) == info['sha256']
            for info in inputs['tools'].values()) and
            (not gcc or verify_gcc_tree(args.gcc_archive, args.gcc_root) == gcc))
        path = output / 'receipt.json'
        path.write_text(json.dumps(receipt, indent=2) + '\n')
        path.chmod(0o600)
    if not all(receipt[name] for name in ('source_unchanged', 'controller_patches_unchanged', 'toolchain_unchanged')):
        raise RuntimeError('Inputs changed during compile; do not promote this result')


if __name__ == '__main__':
    main()
