#!/usr/bin/env python3
"""Build a branch's Noble workspace from its exact package, without downstream CI."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import secrets
import shutil
import subprocess
import tempfile
import time


ROOT = Path(__file__).resolve().parent.parent


def run(*args, capture=False, **kwargs):
    result = subprocess.run([str(a) for a in args], check=True, text=True,
                            stdout=subprocess.PIPE if capture else None, **kwargs)
    return result.stdout.strip() if capture else None


def replace_once(path, old, new):
    text = path.read_text()
    if text.count(old) != 1:
        raise ValueError(f"Upstream layout changed: expected one patch anchor in {path}")
    path.write_text(text.replace(old, new))


def select_package(directory, arch, pipeline, commit):
    packages = list(directory.glob('kasmvncserver_*.deb'))
    if len(packages) != 1:
        raise ValueError('Expected exactly one Noble package. Build/retry the Noble producer; '
                         'check BUILD_DISTROS_REGEX and artifact expiry.')
    package = packages[0]
    if run('dpkg-deb', '-f', package, 'Package', capture=True) != 'kasmvncserver':
        raise ValueError('Unexpected package identity')
    if run('dpkg-deb', '-f', package, 'Architecture', capture=True) != arch:
        raise ValueError('Package architecture does not match workspace architecture')
    checksum = hashlib.sha256(package.read_bytes()).hexdigest()
    provenance = (directory / 'workspace-package.txt').read_text().splitlines()
    if (len(provenance) != 5 or provenance[0:2] != [pipeline, commit]
            or not provenance[2].isdigit() or provenance[3:] != [arch, checksum]):
        raise ValueError('Missing/mismatched successful package producer provenance; '
                         'retry the Noble package job in this pipeline')
    return package, checksum, provenance[2]


def repository_git(*args):
    helper = '!f() { printf "username=gitlab-ci-token\\npassword=%s\\n" "$CI_JOB_TOKEN"; }; f'
    return run('git', '-c', 'credential.helper=', '-c', f'credential.helper={helper}',
               *args, env=dict(os.environ, GIT_TERMINAL_PROMPT='0'),
               capture=True, stderr=subprocess.PIPE)


def preflight_access(project, ref):
    print(f'Checking CI job-token read access to {project} (requested ref: {ref})', flush=True)
    if not os.environ.get('CI_JOB_TOKEN'):
        raise ValueError('CI_JOB_TOKEN is required to read the image repositories')
    try:
        repository_git('ls-remote',
                       f'https://gitlab.com/kasm-technologies/internal/{project}.git', 'HEAD')
    except subprocess.CalledProcessError:
        raise ValueError(f'Cannot read {project}. Verify its CI job-token allowlist includes '
                         'KasmVNC and that the user starting this job can read the repository. '
                         'Also check GitLab connectivity and retry transient failures.') from None
    print(f'Read access confirmed: {project}', flush=True)


def export_source(project, ref, target, source=None):
    # Export tracked files only: Git credentials and .git never enter build contexts.
    with tempfile.TemporaryDirectory(prefix='workspace-source-') as tmp:
        repo = Path(source).resolve() if source else Path(tmp) / 'repo'
        if not source:
            run('git', 'init', '-q', repo)
            url = f"https://gitlab.com/kasm-technologies/internal/{project}.git"
            try:
                repository_git('-C', repo, 'fetch', '--depth=1', url, ref)
            except subprocess.CalledProcessError:
                raise ValueError(f'Cannot fetch {project} ref {ref!r}; verify the ref exists, '
                                 'job-token access remains allowed, and GitLab is reachable.') from None
            revision = 'FETCH_HEAD'
        else:
            revision = 'HEAD'
        sha = run('git', '-C', repo, 'rev-parse', revision, capture=True)
        archive = Path(tmp) / 'source.tar'
        run('git', '-C', repo, 'archive', '-o', archive, revision)
        target.mkdir()
        run('tar', '-xf', archive, '-C', target)
        return sha


def prepare_core(core, package):
    # src is already included by the core repository's restrictive .dockerignore.
    destination = core / 'src/ubuntu/install/kasm_vnc/workspace-kasmvnc.deb'
    shutil.copyfile(package, destination)
    installer = core / 'src/ubuntu/install/kasm_vnc/install_kasm_vnc.sh'
    replace_once(installer, '    download_kasmvnc_deb kasmvncserver.deb',
                 '    cp "$INST_SCRIPTS/kasm_vnc/workspace-kasmvnc.deb" /tmp/kasmvncserver.deb')
    # The existing COPY already includes the package. Keep dependency/setup logic intact.
    # Assert this interface before building instead of silently using a released package.
    dockerfile = (core / 'dockerfile-kasm-core').read_text()
    if ('COPY ./src/ubuntu/install/kasm_vnc $INST_SCRIPTS/kasm_vnc/' not in dockerfile
            or 'RUN bash $INST_SCRIPTS/kasm_vnc/install_kasm_vnc.sh' not in dockerfile):
        raise ValueError('Core Dockerfile KasmVNC installation interface changed')


def verify_entrypoint(image, core_image):
    expected = ['/dockerstartup/kasm_default_profile.sh', '/dockerstartup/vnc_startup.sh',
                '/dockerstartup/kasm_startup.sh']
    for candidate in (core_image, image):
        actual = json.loads(run('docker', 'image', 'inspect', '-f',
                                '{{json .Config.Entrypoint}}', candidate, capture=True))
        if actual != expected:
            raise ValueError(f'Unexpected Workspaces entrypoint in {candidate}: {actual!r}')
    # The third argv element is an optional custom-startup hook, absent in Noble Desktop.
    scripts = expected[:2]
    run('docker', 'run', '--rm', '--entrypoint', '/bin/sh', image, '-ec',
        'for entry in "$@"; do test -f "$entry" && test -x "$entry" || exit 1; done',
        '--', *scripts)
    original = run('docker', 'run', '--rm', '--entrypoint', 'sha256sum',
                   core_image, *scripts, capture=True)
    installed = run('docker', 'run', '--rm', '--entrypoint', 'sha256sum',
                    image, *scripts, capture=True)
    if installed != original:
        raise ValueError('Desktop image changed the core Workspaces entrypoint scripts')


def smoke_test(image, package, work, core_image):
    verify_entrypoint(image, core_image)
    extracted = work / 'package'
    run('dpkg-deb', '-x', package, extracted)
    expected = hashlib.sha256((extracted / 'usr/bin/Xkasmvnc').read_bytes()).hexdigest()
    actual = run('docker', 'run', '--rm', '--entrypoint', 'sha256sum', image,
                 '/usr/bin/Xkasmvnc', capture=True).split()[0]
    if actual != expected:
        raise ValueError('Image contains a different KasmVNC binary than the pipeline package')
    run('docker', 'run', '--rm', '--entrypoint', '/usr/bin/Xkasmvnc', image, '-version')
    run('docker', 'run', '--rm', '--entrypoint', 'bash', image, '-ec',
        'test "$(id -u)" = 1000; command -v xfce4-session; command -v firefox; command -v google-chrome || command -v chromium')
    name = 'kasmvnc-workspace-smoke-' + secrets.token_hex(6)
    password = secrets.token_urlsafe(24)
    try:
        run('docker', 'run', '-d', '--name', name, '--shm-size=512m',
            '-e', f'VNC_PW={password}', image)
        for _ in range(60):
            result = subprocess.run(['docker', 'exec', name, 'curl', '-kfSs',
                                     '--max-time', '3', '-u', f'kasm_user:{password}',
                                     'https://localhost:6901/'], stdout=subprocess.DEVNULL,
                                    stderr=subprocess.DEVNULL)
            if result.returncode == 0:
                return
            if run('docker', 'inspect', '-f', '{{.State.Running}}', name, capture=True) != 'true':
                break
            time.sleep(2)
        raise ValueError('Workspace startup/HTTPS smoke check failed')
    finally:
        logs = subprocess.run(['docker', 'logs', name], capture_output=True, text=True)
        (ROOT / 'workspace-image-report/smoke.log').write_text(
            (logs.stdout + logs.stderr).replace(password, '[redacted]'))
        subprocess.run(['docker', 'rm', '-f', name], check=False, stdout=subprocess.DEVNULL)


def publish(image, alias, labels):
    run('docker', 'login', os.environ['CI_REGISTRY'], '-u', os.environ['CI_REGISTRY_USER'],
        '--password-stdin', input=os.environ['CI_REGISTRY_PASSWORD'] + '\n')
    # The CI resource_group serializes publishers for this branch slug + architecture.
    # Check existing alias ownership/order before replacing it; immutable tags remain usable.
    previous = subprocess.run(['docker', 'pull', alias], capture_output=True, text=True)
    update_alias = True
    if previous.returncode == 0:
        existing = json.loads(run('docker', 'image', 'inspect', '-f',
                                 '{{json .Config.Labels}}', alias, capture=True)) or {}
        if existing.get('com.kasm.workspace.branch') != labels['com.kasm.workspace.branch']:
            raise ValueError('Sanitized branch tag collision; refusing to overwrite another branch')
        update_alias = int(existing['com.kasm.workspace.pipeline']) <= int(labels['com.kasm.workspace.pipeline'])
    elif not any(s in previous.stderr.lower() for s in ('manifest unknown', 'manifest_unknown', 'not found')):
        raise ValueError('Cannot check registry branch alias: ' + previous.stderr)
    run('docker', 'push', image)
    if update_alias:
        run('docker', 'tag', image, alias)
        run('docker', 'push', alias)
    else:
        print('A newer pipeline owns the branch tag; use this build\'s immutable reference.')
    digests = json.loads(run('docker', 'image', 'inspect', '-f', '{{json .RepoDigests}}',
                            image, capture=True))
    return update_alias, digests


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--arch', required=True, choices=['amd64', 'arm64'])
    parser.add_argument('--core-source', type=Path, help='Local checkout for an offline source export')
    parser.add_argument('--desktop-source', type=Path)
    parser.add_argument('--package-dir', type=Path, default=ROOT / 'output/ubuntu_noble')
    parser.add_argument('--no-push', action='store_true', help='Build/smoke-test locally without registry writes')
    args = parser.parse_args()
    pipeline, commit, job = (os.environ[k] for k in ('CI_PIPELINE_ID', 'CI_COMMIT_SHA', 'CI_JOB_ID'))
    arch = args.arch
    package, checksum, producer = select_package(args.package_dir, arch, pipeline, commit)
    daemon_arch = run('docker', 'info', '--format', '{{.Architecture}}', capture=True)
    if daemon_arch not in {'amd64': ('x86_64', 'amd64'), 'arm64': ('aarch64', 'arm64')}[arch]:
        raise ValueError('Use a native Docker daemon matching the package architecture')
    report_dir = ROOT / 'workspace-image-report'
    report_dir.mkdir(exist_ok=True)
    registry_image = os.environ.get('CI_REGISTRY_IMAGE', 'kasmvnc-local')
    slug = os.environ['CI_COMMIT_REF_SLUG']
    image = f'{registry_image}/ubuntu-noble-desktop:{slug}-{pipeline}-{job}_{arch}'
    alias = f'{registry_image}/ubuntu-noble-desktop:{slug}_{arch}'
    core_image = f'kasmweb/core-ubuntu-noble:kasmvnc-{pipeline}-{job}_{arch}'
    for project, ref, source in (
            ('workspaces-core-images', os.environ.get('WORKSPACE_CORE_REF', 'develop'), args.core_source),
            ('workspaces-images', os.environ.get('WORKSPACE_DESKTOP_REF', 'develop'), args.desktop_source)):
        if source is None:
            preflight_access(project, ref)
    with tempfile.TemporaryDirectory(prefix='workspace-build-') as tmp:
        work = Path(tmp)
        core, desktop = work / 'core', work / 'desktop'
        core_sha = export_source('workspaces-core-images', os.environ.get('WORKSPACE_CORE_REF', 'develop'),
                                 core, args.core_source)
        desktop_sha = export_source('workspaces-images', os.environ.get('WORKSPACE_DESKTOP_REF', 'develop'),
                                    desktop, args.desktop_source)
        prepare_core(core, package)
        labels = {'org.opencontainers.image.revision': commit,
                  'com.kasm.workspace.branch': os.environ['CI_COMMIT_REF_NAME'],
                  'com.kasm.workspace.pipeline': pipeline,
                  'com.kasm.workspace.package.sha256': checksum,
                  'com.kasm.workspace.core.revision': core_sha,
                  'com.kasm.workspace.desktop.revision': desktop_sha}
        report = dict(labels, architecture=arch, package_job=producer, image=image, branch_image=alias)
        (report_dir / 'image.json').write_text(json.dumps(report, indent=2) + '\n')
        # Force the Docker driver: a docker-container buildx driver cannot see daemon-local FROMs.
        docker_env = dict(os.environ, BUILDX_BUILDER='default', DOCKER_BUILDKIT='1')
        run('docker', 'build', '--pull=false', '--platform', f'linux/{arch}', '-t', core_image,
            '--build-arg', 'BASE_IMAGE=ubuntu:24.04', '--build-arg', 'DISTRO=ubuntu',
            '-f', core / 'dockerfile-kasm-core', core, env=docker_env)
        if 'FROM kasmweb/$BASE_IMAGE:$BASE_TAG' not in (desktop / 'dockerfile-kasm-ubuntu-noble-desktop').read_text():
            raise ValueError('Desktop Dockerfile base-image interface changed')
        label_args = [arg for key, value in labels.items() for arg in ('--label', f'{key}={value}')]
        run('docker', 'build', '--pull=false', '--platform', f'linux/{arch}', '-t', image,
            '--build-arg', 'BASE_IMAGE=core-ubuntu-noble', '--build-arg', f'BASE_TAG={core_image.split(":")[-1]}',
            *label_args, '-f', desktop / 'dockerfile-kasm-ubuntu-noble-desktop', desktop, env=docker_env)
        smoke_test(image, package, work, core_image)
        report['local_image_id'] = run('docker', 'image', 'inspect', '-f', '{{.Id}}', image, capture=True)
        report['published'] = not args.no_push
        if not args.no_push:
            report['branch_tag_updated'], report['digests'] = publish(image, alias, labels)
        report['smoke_test'] = 'passed'
        (report_dir / 'image.json').write_text(json.dumps(report, indent=2) + '\n')
        print(json.dumps(report, indent=2))
        instructions = (
            'See image.json for the registry digest and source/package provenance.\n'
            'Configure a Kasm Workspaces image with this reference and a GitLab deploy token\n'
            'with read_registry permission. Do not use the expiring CI job token for pulls.\n'
            'Select an agent matching the image architecture, then launch a fresh session.\n'
            if report['published'] else
            'Not published. image.json contains a local image ID, not a registry digest.\n'
            'The image is available only in the local Docker daemon used for validation.\n')
        (report_dir / 'README.txt').write_text(
            f'Image: {image}\nBranch alias: {alias}\nArchitecture: {arch}\n' + instructions)



if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        raise SystemExit(f'Workspace image build failed: {error}')
