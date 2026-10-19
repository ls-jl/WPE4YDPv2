#!/usr/bin/env python3
"""Export reviewed text inputs for the matched G17 candidate, never binaries."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import urllib.request

sys.dont_write_bytecode = True
import build_g17_kernel_candidate as build
import compile_g17_candidate as policy
import package_g17_source_bundle as bundle

REPOSITORY = 'https://github.com/rockchip-linux/kernel'
TREE = 'b43acf827e54177453b8097668356a9ae4fa2c03'
API = 'https://api.github.com/repos/rockchip-linux/kernel/git/commits/' + policy.REVISION
TEMPLATE = 'chromium/source-delivery/G17_SOURCE_COMPANION.md'
SCRIPTS = ('build_g17_kernel_candidate.py', 'compile_g17_candidate.py',
           'package_g17_source_bundle.py', 'export_g17_source_companion.py')
# Reviewed per-file scope, not a whole-tree license classifier. Prefix lengths
# retain the original copyright/grant blocks, including contradictory SPDX lines.
SCOPES = {
    'drivers/opp/core.c': ('GPL-2.0-only', 9),
    'drivers/opp/opp.h': ('GPL-2.0-only', 9),
    'include/linux/pm_opp_safe.h': ('GPL-2.0-only', 1),
    'drivers/soc/rockchip/rockchip_opp_select.c': ('GPL-2.0-or-later', 5),
    'drivers/gpu/arm/bifrost/mali_kbase_core_linux.c': ('GPL-2.0 WITH Linux-syscall-note', 21),
    'drivers/gpu/arm/bifrost/mali_kbase_defs.h': ('GPL-2.0 WITH Linux-syscall-note', 21),
    'drivers/gpu/arm/bifrost/platform/rk/mali_kbase_config_rk.c': ('GPL-2.0-only', 8),
    'drivers/gpu/arm/bifrost/platform/rk/mali_kbase_rk.h': ('GPL-2.0-only', 7),
    'drivers/gpu/arm/bifrost/backend/gpu/mali_kbase_devfreq.c': ('GPL-2.0; no exception asserted', 23),
    'drivers/gpu/arm/bifrost/backend/gpu/mali_kbase_devfreq.h': ('GPL-2.0 WITH Linux-syscall-note', 21),
}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def patches():
    return [Path('chromium/kernel-patches') / p for p in policy.ROCKCHIP_OPP_PATCHES] + [
        Path('chromium/driver-patches') / p for p in policy.MANAGED_DRIVER_PATCHES]


def payload_names():
    return {'README.md', TEMPLATE, 'LICENSE', 'PUBLIC-UPSTREAM.json',
            'config/original.config', 'config/built.config', 'notices/COPYING',
            'notices/LICENSES/preferred/GPL-2.0', 'notices/LICENSES/exceptions/Linux-syscall-note',
            *(str(p) for p in patches()), *('chromium/scripts/' + p for p in SCRIPTS)}


def public_identity(data):
    require(isinstance(data, dict) and data.get('sha') == policy.REVISION and
            isinstance(data.get('tree'), dict) and data['tree'].get('sha') == TREE and
            data.get('html_url') == REPOSITORY + '/commit/' + policy.REVISION,
            'Public upstream object does not match the reviewed commit/tree')
    require(isinstance(data.get('verification'), dict) and
            type(data['verification'].get('verified')) is bool, 'Missing upstream signature status')
    return dict(repository=REPOSITORY, revision=policy.REVISION, tree=TREE, api=API,
                signature_verified=data['verification']['verified'])


def check_text(data):
    require(len(data) <= 2 * 1024**2 and b'\0' not in data and
            not data.startswith((b'\x7fELF', b'MZ')), 'Not a bounded text-only source input')
    data.decode('utf-8')
    for kind in (b'', b'RSA ', b'EC ', b'OPENSSH '):
        require(b'-----BEGIN ' + kind + b'PRIVATE KEY-----' not in data, 'Private key in source export')
    require(not re.search(rb'\bgh[pousr]_[A-Za-z0-9]{30,}\b|\bgithub_pat_[A-Za-z0-9_]{30,}\b', data),
            'Credential-shaped content in source export')


def payload_files(root):
    result = {}
    for directory, dirs, files in os.walk(root, followlinks=False):
        if Path(directory) == root and '.git' in dirs:
            dirs.remove('.git')
        for name in dirs + files:
            path = Path(directory) / name
            if path == root / '.git':
                continue
            mode = path.lstat().st_mode
            require(stat.S_ISDIR(mode) or stat.S_ISREG(mode), 'Link/special input in source companion')
            if stat.S_ISREG(mode):
                data = path.read_bytes()
                check_text(data)
                result[path.relative_to(root).as_posix()] = dict(
                    sha256=hashlib.sha256(data).hexdigest(), bytes=len(data), executable=bool(mode & 0o111))
    return result


def verify(directory):
    root = directory.resolve(strict=True)
    files = payload_files(root)
    require(files.keys() == payload_names() | {'SOURCE-MANIFEST.json', 'SHA256SUMS'},
            'Unlisted or missing source companion input')
    manifest = json.loads((root / 'SOURCE-MANIFEST.json').read_text())
    reference = json.loads((root / 'PUBLIC-UPSTREAM.json').read_text())
    require(manifest['upstream'] == public_identity(reference), 'Source companion upstream mismatch')
    require(manifest.get('kind') == 'g17-managed-source-companion-v1' and
            manifest.get('source_companion_only') is True and
            manifest.get('kernel_binary_release_accepted') is False and
            manifest.get('hardware_accepted') is False, 'Source companion is not binary approval')
    expected = {name: row for name, row in files.items() if name in payload_names()}
    require(manifest['files'] == expected, 'Source companion payload hash/mode mismatch')
    require(manifest['patch_series'] == [str(p) for p in patches()], 'Source patch order differs')
    require(policy.sha(root / 'config/original.config') == policy.CONFIG_SHA,
            'Source companion lost the exact original configuration')
    build.verify_config((root / 'config/built.config').read_text())
    checksums = ''.join(row['sha256'] + '  ' + name + '\n' for name, row in sorted(files.items())
                        if name != 'SHA256SUMS')
    require((root / 'SHA256SUMS').read_text() == checksums, 'Source companion checksum index mismatch')
    return manifest


def reviewed_notice(path, before, after):
    basis, lines = SCOPES[path]
    prefix = b''.join(after.splitlines(keepends=True)[:lines])
    if before is None:
        require(path == 'include/linux/pm_opp_safe.h' and
                prefix == b'/* SPDX-License-Identifier: GPL-2.0-only */\n', 'Unreviewed new source notice')
    else:
        require(prefix == b''.join(before.splitlines(keepends=True)[:lines]), 'Source notice changed: ' + path)
    return dict(redistribution_basis=basis, retained_prefix_sha256=hashlib.sha256(prefix).hexdigest(),
                spdx_lines=[line.strip(' /*\t') for line in prefix.decode().splitlines()
                            if 'SPDX-License-Identifier:' in line], new_file=before is None)


def create(args):
    source, previous = args.source_root.resolve(strict=True), args.build_output.resolve(strict=True)
    output = args.output.resolve()
    require(not args.output.exists() and not args.output.is_symlink() and output.parent.is_dir(),
            'Source export requires a new directory with an existing parent')
    require(not any(output.is_relative_to(p) for p in (source, previous, policy.PROJECT)),
            'Source export must be outside original inputs/project')
    require(shutil.disk_usage(output.parent).free >= 8 * 1024**3, 'Source replay needs at least 8GiB free')
    env = bundle.git_environment()
    env['GIT_NO_REPLACE_OBJECTS'] = '1'
    def git(*parts):
        return subprocess.check_output(['git', '-C', str(source), *parts], env=env).strip()
    require(git('rev-parse', 'HEAD').decode() == policy.REVISION and
            git('rev-parse', 'HEAD^{tree}').decode() == TREE and
            not git('status', '--porcelain', '--untracked-files=all'), 'Not the clean reviewed upstream source')
    receipt = json.loads((previous / 'receipt.json').read_text())
    bundle.verify_build(receipt, previous)
    require(policy.sha(args.config) == policy.CONFIG_SHA, 'Original config differs')
    controls = [policy.PROJECT / 'chromium/scripts' / p for p in SCRIPTS[:2]]
    require([policy.sha(p) for p in controls] == [receipt['inputs']['controller_sha256'],
            receipt['inputs']['policy_sha256']], 'Build controllers changed since the recorded build')
    series = patches()
    require({p.name: policy.sha(policy.PROJECT / p) for p in series} == receipt['inputs']['patches'],
            'Patches changed since the recorded build')
    request = urllib.request.Request(API, headers={'User-Agent': 'dictpen-source-companion/1'})
    with urllib.request.urlopen(request, timeout=30) as response:
        require(response.status == 200 and response.url == API, 'Unexpected public upstream response')
        raw_reference = response.read(1024**2 + 1)
    require(len(raw_reference) <= 1024**2, 'Oversized public upstream response')
    upstream = public_identity(json.loads(raw_reference))
    frozen = {p: policy.sha(p) for p in [*(policy.PROJECT / 'chromium/scripts' / n for n in SCRIPTS),
                                       *(policy.PROJECT / p for p in series), args.config,
                                       policy.PROJECT / TEMPLATE, policy.PROJECT / 'LICENSE']}
    with tempfile.TemporaryDirectory(prefix='g17-source-companion-', dir=output.parent) as temporary:
        temp = Path(temporary)
        archive = temp / 'base.tar'
        subprocess.run(['git', '-C', str(source), 'archive', '--format=tar', '--output=' + str(archive),
                        policy.REVISION], env=env, check=True)
        require(policy.sha(archive) == receipt['source_archive_sha256'], 'Archive differs from actual build input')
        replay = temp / 'replay'
        build.extract_source(archive, replay)
        archive.unlink()
        for relative in series:
            subprocess.run(['git', 'apply', '--check', '--whitespace=error-all', str(policy.PROJECT / relative)],
                           cwd=replay, env=env, check=True)
            subprocess.run(['git', 'apply', '--whitespace=error-all', str(policy.PROJECT / relative)],
                           cwd=replay, env=env, check=True)
        inventory = bundle.tree_inventory(replay)
        require(inventory == bundle.tree_inventory(previous / 'source'),
                'Preferred source replay differs from the actual linked kernel')
        touched = set()
        for relative in series:
            rows = subprocess.check_output(['git', 'apply', '--numstat', '-z', str(policy.PROJECT / relative)], env=env)
            for row in rows.split(b'\0'):
                if row:
                    added, removed, name = row.decode().split('\t', 2)
                    require(added.isdecimal() and removed.isdecimal(), 'Binary source patch refused')
                    touched.add(name)
        require(touched == SCOPES.keys(), 'Patch file scope needs a new notice review')
        notices = {}
        for path in sorted(touched):
            before = None if path == 'include/linux/pm_opp_safe.h' else subprocess.check_output(
                ['git', '-C', str(source), 'show', policy.REVISION + ':' + path], env=env)
            notices[path] = reviewed_notice(path, before, (replay / path).read_bytes())
        payload = temp / 'payload'
        payload.mkdir(mode=0o700)
        copied = {str(p): policy.PROJECT / p for p in series}
        copied.update({'chromium/scripts/' + p: policy.PROJECT / 'chromium/scripts' / p for p in SCRIPTS})
        copied.update({'README.md': policy.PROJECT / TEMPLATE, TEMPLATE: policy.PROJECT / TEMPLATE,
                       'LICENSE': policy.PROJECT / 'LICENSE', 'config/original.config': args.config,
                       'config/built.config': previous / 'kernel-out/.config'})
        for name in ('COPYING', 'LICENSES/preferred/GPL-2.0', 'LICENSES/exceptions/Linux-syscall-note'):
            copied['notices/' + name] = replay / name
        for name, original in copied.items():
            target = payload / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(original.read_bytes())
            target.chmod(0o755 if original.stat().st_mode & 0o111 else 0o644)
        (payload / 'PUBLIC-UPSTREAM.json').write_bytes(raw_reference)
        files = payload_files(payload)
        manifest = dict(kind='g17-managed-source-companion-v1', source_companion_only=True,
            upstream=upstream, observed_at=datetime.datetime.now(datetime.timezone.utc).isoformat(),
            original_build_receipt_sha256=policy.sha(previous / 'receipt.json'),
            source_archive_sha256=receipt['source_archive_sha256'],
            preferred_source_entries=len(inventory),
            preferred_source_inventory_sha256=hashlib.sha256((json.dumps(inventory, sort_keys=True) + '\n').encode()).hexdigest(),
            patch_series=[str(p) for p in series], notice_review=notices, build_artifacts=receipt['artifacts'],
            files=files, hardware_accepted=False, kernel_binary_release_accepted=False)
        (payload / 'SOURCE-MANIFEST.json').write_text(json.dumps(manifest, indent=2, sort_keys=True) + '\n')
        indexed = payload_files(payload)
        (payload / 'SHA256SUMS').write_text(''.join(row['sha256'] + '  ' + name + '\n'
                                                  for name, row in sorted(indexed.items())))
        verify(payload)
        require(all(policy.sha(p) == digest for p, digest in frozen.items()), 'Inputs changed during source export')
        bundle.verify_build(receipt, previous)
        require(not git('status', '--porcelain', '--untracked-files=all'), 'Original source changed')
        shutil.copytree(payload, output)
        verify(output)
    print(json.dumps(dict(output=str(output), files=len(payload_files(output)),
                         manifest_sha256=policy.sha(output / 'SOURCE-MANIFEST.json')), indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    export = commands.add_parser('create')
    for name in ('source-root', 'config', 'build-output', 'output'):
        export.add_argument('--' + name, type=Path, required=True)
    check = commands.add_parser('verify')
    check.add_argument('--directory', type=Path, required=True)
    args = parser.parse_args()
    if args.command == 'create':
        create(args)
    else:
        result = verify(args.directory)
        print(json.dumps(dict(verified=True, kind=result['kind'], binary_release_accepted=False)))


if __name__ == '__main__':
    main()
