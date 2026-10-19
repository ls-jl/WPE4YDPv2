#!/usr/bin/env python3
"""Private matching-source delivery for an already linked kernel; never deploy."""
import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import stat
import subprocess
import tarfile
import tempfile

import build_g17_kernel_candidate as build
import compile_g17_candidate as policy


def git_environment():
    env = {key: value for key, value in os.environ.items()
           if not key.startswith('GIT_') and key not in ('LD_PRELOAD', 'LD_LIBRARY_PATH')}
    env.update(GIT_CONFIG_NOSYSTEM='1', GIT_CONFIG_GLOBAL='/dev/null',
               GIT_TERMINAL_PROMPT='0', GIT_OPTIONAL_LOCKS='0', LC_ALL='C')
    return env


def snapshot(source, destination, revision, log):
    """A standalone one-commit object closure, without source remotes or hooks."""
    if destination.exists() or destination.is_symlink():
        raise ValueError('Snapshot destination must be new')
    env = git_environment()
    with tempfile.TemporaryDirectory(prefix='empty-git-template-') as template:
        commands = [
            ['git', 'init', '--bare', '--template=' + template, str(destination)],
            ['git', '-c', 'protocol.file.allow=always', '-c', 'pack.threads=1',
             '-C', str(destination), 'fetch', '--depth=1', '--no-tags',
             '--no-recurse-submodules', source.as_uri(), revision],
            ['git', '-C', str(destination), 'update-ref', 'refs/heads/pen-source', revision],
            ['git', '-C', str(destination), 'symbolic-ref', 'HEAD', 'refs/heads/pen-source'],
        ]
        for command in commands:
            subprocess.run(command, env=env, stdout=log, stderr=subprocess.STDOUT, check=True)
    (destination / 'FETCH_HEAD').unlink()
    if (destination / 'hooks').exists() or (destination / 'objects/info/alternates').exists():
        raise ValueError('Snapshot inherited hooks or external objects')
    config = subprocess.check_output(['git', '-C', str(destination), 'config', '--list'], env=env, text=True)
    if any(not row.startswith('core.') for row in config.splitlines()):
        raise ValueError('Snapshot has nonlocal repository configuration')
    for command, expected in ((['rev-parse', 'HEAD'], revision), (['rev-list', '--count', 'HEAD'], '1')):
        if subprocess.check_output(['git', '-C', str(destination), *command], env=env, text=True).strip() != expected:
            raise ValueError('Snapshot lost the pinned single-commit boundary')
    subprocess.run(['git', '-C', str(destination), 'fsck', '--full', '--strict'],
                   env=env, stdout=log, stderr=subprocess.STDOUT, check=True)


def tree_inventory(root):
    result = {}
    for directory, directories, files in os.walk(root, followlinks=False):
        for name in sorted(directories + files):
            path = Path(directory) / name
            info = path.lstat()
            key = path.relative_to(root).as_posix()
            if stat.S_ISLNK(info.st_mode):
                result[key] = dict(kind='symlink', target=os.readlink(path))
            elif stat.S_ISDIR(info.st_mode):
                result[key] = dict(kind='directory', mode=stat.S_IMODE(info.st_mode))
            elif stat.S_ISREG(info.st_mode):
                result[key] = dict(kind='file', mode=stat.S_IMODE(info.st_mode),
                                   bytes=info.st_size, sha256=policy.sha(path))
            else:
                raise ValueError('Special file in source inventory: ' + key)
    return dict(sorted(result.items()))


def verify_build(receipt, output):
    if (receipt['kind'] != 'private-coherent-managed-kernel' or
            receipt['outcome'] != 'linked-coherent-candidate' or
            receipt['source_unchanged'] is not True or receipt['build_inputs_unchanged'] is not True or
            receipt['installed'] is not False or receipt['public_release_accepted'] is not False or
            receipt['hardware_accepted'] is not False or receipt['stock_kernel_abi_accepted'] is not False or
            receipt['release'] != '5.10.160' + build.SUFFIX or
            receipt['inputs']['revision'] != policy.REVISION or
            Path(receipt['output']).resolve() != output):
        raise ValueError('Not a matching private coherent build receipt')
    expected = {'kernel-out/.config', 'kernel-out/vmlinux', 'kernel-out/System.map',
                'kernel-out/Module.symvers', 'kernel-out/modules.order',
                'kernel-out/arch/arm64/boot/Image',
                'kernel-out/' + str(policy.BIFROST / 'bifrost_kbase.ko')}
    if set(receipt['artifacts']) != expected or not receipt['kbuild_commands']:
        raise ValueError('Missing or foreign coherent build artifacts')
    hashes = {name: row['sha256'] for name, row in receipt['artifacts'].items()}
    hashes.update(receipt['kbuild_commands'])
    hashes['build.log'] = receipt['log_sha256']
    for name, digest in hashes.items():
        path = output / name
        if not path.resolve().is_relative_to(output) or path.is_symlink() or policy.sha(path) != digest:
            raise ValueError('Build file differs from receipt: ' + name)
        if name in receipt['artifacts'] and path.stat().st_size != receipt['artifacts'][name]['bytes']:
            raise ValueError('Build file length differs from receipt: ' + name)
    build.verify_config((output / 'kernel-out/.config').read_text())


def write_archive(root, destination, epoch):
    """Deterministic wrapper; the payload itself must not contain links/devices."""
    with destination.open('xb') as raw:
        with gzip.GzipFile(filename='', fileobj=raw, mode='wb', mtime=epoch, compresslevel=1) as compressed:
            with tarfile.open(fileobj=compressed, mode='w|') as archive:
                for path in sorted(root.rglob('*')):
                    info = archive.gettarinfo(str(path), path.relative_to(root).as_posix())
                    if not info.isdir() and not info.isfile():
                        raise ValueError('Only regular files/directories belong in the bundle')
                    if info.mode & 0o7000:
                        raise ValueError('Privileged modes do not belong in the bundle')
                    info.uid = info.gid = 0
                    info.uname = info.gname = ''
                    info.mtime = epoch
                    if info.isfile():
                        with path.open('rb') as content: archive.addfile(info, content)
                    else: archive.addfile(info)
    destination.chmod(0o600)


def normalize_payload_modes(root):
    """Safe data extraction must preserve the private delivery's declared modes."""
    for path in root.rglob('*'):
        info = path.lstat()
        if (not stat.S_ISREG(info.st_mode) and not stat.S_ISDIR(info.st_mode)) or info.st_mode & 0o7000:
            raise ValueError('Unsafe payload type or privileged mode')
        path.chmod(0o755 if path.is_dir() or info.st_mode & 0o111 else 0o644)


def verify_archive(path, manifest):
    seen = set()
    with tarfile.open(path, 'r|gz') as archive:
        for member in archive:
            name = PurePosixPath(member.name)
            if name.is_absolute() or '..' in name.parts or str(name) != member.name or member.name in seen:
                raise ValueError('Unsafe/duplicate bundle path')
            if member.uid or member.gid or member.uname or member.gname or member.mode & 0o7000:
                raise ValueError('Unexpected bundle ownership/privileged mode')
            seen.add(member.name)
            if member.isdir(): continue
            if not member.isfile(): raise ValueError('Unexpected bundle link/special file')
            with archive.extractfile(member) as stream:
                digest = hashlib.file_digest(stream, 'sha256').hexdigest()
            if member.name == 'manifest.json':
                if digest != manifest['manifest_sha256']: raise ValueError('Manifest transport mismatch')
            else:
                row = manifest['files'].get(member.name)
                if not row or (digest, member.size, member.mode) != (row['sha256'], row['bytes'], row['mode']):
                    raise ValueError('Payload transport mismatch: ' + member.name)
    if {name for name in seen if name in manifest['files']} != set(manifest['files']) or 'manifest.json' not in seen:
        raise ValueError('Incomplete bundle payload')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, required=True)
    parser.add_argument('--config', type=Path, required=True)
    parser.add_argument('--build-output', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True, help='New private directory outside source/project/build')
    args = parser.parse_args()
    source, output, previous = args.source_root.resolve(), args.output.resolve(), args.build_output.resolve()
    if args.output.exists() or args.output.is_symlink(): raise ValueError('Output must be new')
    if any(output.is_relative_to(root) for root in (source, previous, policy.PROJECT)):
        raise ValueError('Output must be outside source, project and previous build')
    if not output.parent.is_dir() or shutil.disk_usage(output.parent).free < 8 * 1024**3:
        raise ValueError('Existing output parent needs at least 8GiB free')
    env = git_environment()
    def git(*command):
        return subprocess.check_output(['git', '-C', str(source), *command], env=env, text=True).strip()
    if git('rev-parse', 'HEAD') != policy.REVISION or git('status', '--porcelain', '--untracked-files=all'):
        raise ValueError('Requires the exact clean non-Android reference')
    receipt_path = previous / 'receipt.json'
    receipt = json.loads(receipt_path.read_text())
    verify_build(receipt, previous)
    if policy.sha(args.config) != policy.CONFIG_SHA or receipt['inputs']['config_sha256'] != policy.CONFIG_SHA:
        raise ValueError('Original configuration does not match the build')
    inputs = [policy.PROJECT / 'chromium/scripts' / name for name in
              ('build_g17_kernel_candidate.py', 'compile_g17_candidate.py')]
    if [policy.sha(path) for path in inputs] != [receipt['inputs']['controller_sha256'], receipt['inputs']['policy_sha256']]:
        raise ValueError('Build controller/policy no longer match the original build')
    patches = [policy.PROJECT / 'chromium/kernel-patches' / name for name in policy.ROCKCHIP_OPP_PATCHES]
    patches += [policy.PROJECT / 'chromium/driver-patches' / name for name in policy.MANAGED_DRIVER_PATCHES]
    if {path.name: policy.sha(path) for path in patches} != receipt['inputs']['patches']:
        raise ValueError('Patch series no longer matches the build')
    if receipt['inputs']['gcc_tree']['archive_sha256'] != policy.GCC_ARCHIVE_SHA:
        raise ValueError('Unknown original compiler')
    frozen = {path: policy.sha(path) for path in [Path(__file__), *inputs, *patches, policy.PROJECT / 'LICENSE']}
    epoch = int(git('show', '-s', '--format=%ct', policy.REVISION))
    output.mkdir(mode=0o700)
    result = dict(kind='private-matching-kernel-source-bundle', outcome='preparing', installed=False,
                  hardware_accepted=False, public_release_accepted=False,
                  original_build_receipt_sha256=policy.sha(receipt_path))
    try:
        with (output / 'package.log').open('w') as log, tempfile.TemporaryDirectory(prefix='source-delivery-', dir=output) as temporary:
            temp = Path(temporary); payload = temp / 'payload'; payload.mkdir()
            reference = payload / 'reference.git'
            snapshot(source, reference, policy.REVISION, log)
            print('Single-commit source snapshot created', flush=True)
            baseline = temp / 'baseline.tar'
            subprocess.run(['git', '-C', str(reference), 'archive', '--format=tar',
                            '--output=' + str(baseline), policy.REVISION], env=env, check=True)
            if policy.sha(baseline) != receipt['source_archive_sha256']:
                raise ValueError('Snapshot archive differs from actual build input')
            replay = temp / 'replayed'
            build.extract_source(baseline, replay); baseline.unlink()
            for patch in patches:
                subprocess.run(['git', 'apply', '--check', '--whitespace=error-all', str(patch)], cwd=replay, env=env, check=True)
                subprocess.run(['git', 'apply', '--whitespace=error-all', str(patch)], cwd=replay, env=env, check=True)
            inventory = tree_inventory(replay)
            if inventory != tree_inventory(previous / 'source'):
                raise ValueError('Replayed source differs from the actual linked build tree')
            print('Complete patched source matches the linked build', flush=True)
            copied = [Path(__file__), *inputs, *patches, policy.PROJECT / 'LICENSE']
            for path in copied:
                destination = payload / path.relative_to(policy.PROJECT)
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(path, destination)
            (payload / 'config').mkdir()
            shutil.copy2(args.config, payload / 'config/original.config')
            shutil.copy2(previous / 'kernel-out/.config', payload / 'config/built.config')
            shutil.copy2(receipt_path, payload / 'original-build-receipt.json')
            (payload / 'patched-source-inventory.json').write_text(json.dumps(inventory, sort_keys=True) + '\n')
            (payload / 'notices').mkdir()
            for name in ('COPYING', 'LICENSES/preferred/GPL-2.0', 'LICENSES/exceptions/Linux-syscall-note'):
                destination = payload / 'notices' / name
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(replay / name, destination)
            (payload / 'REPRODUCE.md').write_text(
                '# Private Matching Kernel Source\n\n'
                'Not a stock-compatible module, hardware approval or public-release clearance.\n'
                'This is the complete pinned shallow source, all 15 patches, exact original/built\n'
                'configurations and the original unmodified build controller and policy.\n'
                'All kernel notices remain in reference.git; notices/ also copies the root GPL texts.\n'
                'Project LICENSE applies to original tooling, not a relicensing of the kernel.\n\n'
                'After safe extraction into a new private directory, set BUNDLE to that directory.\n'
                'Use Git, Python 3.12+, make/cc/bc/flex/bison/modinfo and the pinned original Arm\n'
                'GCC10.3.1 archive/tree (see original-build-receipt.json for every tool/input hash).\n'
                'Do not copy compiler binaries, firmware or Mali userspace into a public release.\n\n'
                '```sh\n'
                'git clone --no-local --no-hardlinks --depth=1 --no-tags "file://$BUNDLE/reference.git" "$NEW_SOURCE"\n'
                'python3 "$BUNDLE/chromium/scripts/build_g17_kernel_candidate.py" \\\n'
                '  --source-root "$NEW_SOURCE" --config "$BUNDLE/config/original.config" \\\n'
                '  --gcc-root "$GCC_ROOT" --gcc-archive "$GCC_ARCHIVE" \\\n'
                '  --build-root "$EXISTING_PRIVATE_BUILD_ROOT" --jobs 6\n'
                '```\n\n'
                'The build root must be outside source and have at least 8GiB free.\n'
                'The original controller checks the compiler tree, replays all patches, assigns\n'
                '5.10.160-pen-managed and links Image/modules; it never installs or flashes.\n'
                'Compare its source archive/config/imports and record fresh artifact hashes;\n'
                'this bundle does not claim a second full rebuild or bit-identical outputs.\n'
                'No DTB/boot image, recovery instructions, stock ABI or universal KO is supplied.\n'
                'Full-source licensing/provenance, firmware, patents, sandbox and browser release\n'
                'gates remain separate and unaccepted. Do not publicly upload this private bundle.\n')
            normalize_payload_modes(payload)
            files = {name: row for name, row in tree_inventory(payload).items() if row['kind'] == 'file'}
            manifest = dict(version=1, revision=policy.REVISION, source_archive_sha256=receipt['source_archive_sha256'],
                original_build_receipt_sha256=policy.sha(receipt_path), source_entries=len(inventory),
                source_inventory_sha256=policy.sha(payload / 'patched-source-inventory.json'),
                snapshot_commits=1, files=files, public_release_accepted=False,
                license_audit_complete=False, hardware_accepted=False, separate_toolchain_payload_included=False)
            (payload / 'manifest.json').write_text(json.dumps(manifest, indent=2, sort_keys=True) + '\n')
            (payload / 'manifest.json').chmod(0o644)
            manifest['manifest_sha256'] = policy.sha(payload / 'manifest.json')
            archive = output / 'private-source-bundle.tar.gz'
            write_archive(payload, archive, epoch)
            verify_archive(archive, manifest)
            result.update(outcome='verified-matching-source-bundle', archive_sha256=policy.sha(archive),
                          archive_bytes=archive.stat().st_size, manifest_sha256=manifest['manifest_sha256'],
                          patched_source_inventory_sha256=manifest['source_inventory_sha256'],
                          source_entries=len(inventory), payload_files=len(files), snapshot_commits=1)
        verify_build(receipt, previous)
        if git('rev-parse', 'HEAD') != policy.REVISION or git('status', '--porcelain', '--untracked-files=all'):
            raise ValueError('Original reference changed while packaging')
        if policy.sha(receipt_path) != result['original_build_receipt_sha256'] or policy.sha(args.config) != policy.CONFIG_SHA:
            raise ValueError('Original build inputs changed while packaging')
        if any(policy.sha(path) != digest for path, digest in frozen.items()):
            raise ValueError('Packaging scripts/patches/notices changed during delivery')
        result['original_inputs_unchanged'] = True
    except BaseException as error:
        result.update(outcome='failed', error=str(error)); raise
    finally:
        result['packager_sha256'] = policy.sha(Path(__file__))
        path = output / 'receipt.json'
        path.write_text(json.dumps(result, indent=2) + '\n'); path.chmod(0o600)
    print(json.dumps(result, indent=2))


if __name__ == '__main__': main()
