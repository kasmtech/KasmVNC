#!/usr/bin/env python3
"""Build a branch's Noble workspace from its exact package, without downstream CI."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import secrets
import shutil
import subprocess
import tempfile
import time

import yaml


ROOT = Path(__file__).resolve().parent.parent


def run(*args, capture=False, **kwargs):
    result = subprocess.run([str(a) for a in args], check=True, text=True,
                            stdout=subprocess.PIPE if capture else None, **kwargs)
    return result.stdout.strip() if capture else None


def require_env(name):
    value = os.environ.get(name)
    if not value:
        raise ValueError(f'{name} is required')
    return value


def git_error(error):
    detail = error.stderr or ''
    token = os.environ.get('CI_JOB_TOKEN')
    return detail.replace(token, '[redacted]') if token else detail


def install_signal_handlers():
    def interrupted(signum, frame):
        raise SystemExit(128 + signum)
    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGHUP, interrupted)


def registry_settings():
    values = {key: require_env(key) for key in
              ('CI_REGISTRY', 'CI_REGISTRY_USER', 'CI_REGISTRY_PASSWORD', 'CI_REGISTRY_IMAGE')}
    if values['CI_REGISTRY'] != 'registry.gitlab.com':
        raise ValueError('CI_REGISTRY must be registry.gitlab.com')
    if values['CI_REGISTRY_IMAGE'] != 'registry.gitlab.com/kasm-technologies/internal/kasmvnc':
        raise ValueError('CI_REGISTRY_IMAGE must be the KasmVNC project registry')
    return values


def replace_once(path, old, new):
    text = path.read_text()
    if text.count(old) != 1:
        raise ValueError(f"Upstream layout changed: expected one patch anchor in {path}")
    path.write_text(text.replace(old, new))


def select_package(directory, arch, pipeline, commit):
    packages = [p for p in directory.glob('kasmvncserver_*.deb')
                if not p.name.endswith('.dbgsym.deb')]
    if len(packages) != 1:
        raise ValueError('Expected exactly one portable DEB. Build/retry the portable producer; '
                         'check artifact expiry.')
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
                         'retry the portable package job in this pipeline')
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
    if not ref or any(c in ref for c in '*?[') or ref.startswith('-'):
        raise ValueError(f'Invalid image repository ref: {ref!r}')
    if re.fullmatch(r'[0-9a-fA-F]{40}', ref):
        print('Commit SHA cannot be verified by ls-remote; fetch will verify it.', flush=True)
        return
    refs = [ref] if ref.startswith('refs/') else [f'refs/heads/{ref}', f'refs/tags/{ref}']
    try:
        matches = repository_git('ls-remote', '--',
                                 f'https://gitlab.com/kasm-technologies/internal/{project}.git', *refs)
    except subprocess.CalledProcessError as error:
        raise ValueError(f'Cannot read {project}. Verify its CI job-token allowlist includes '
                         'KasmVNC and that the user starting this job can read the repository. '
                         f'Git error: {git_error(error)}') from None
    if not matches:
        raise ValueError(f'Ref {ref!r} does not exist in {project}')
    print(f'Read access and ref confirmed: {project}', flush=True)


def export_source(project, ref, target, source=None):
    # Export tracked files only: Git credentials and .git never enter build contexts.
    with tempfile.TemporaryDirectory(prefix='workspace-source-') as tmp:
        repo = Path(source).resolve() if source else Path(tmp) / 'repo'
        if not source:
            run('git', 'init', '-q', repo)
            url = f"https://gitlab.com/kasm-technologies/internal/{project}.git"
            try:
                repository_git('-C', repo, 'fetch', '--depth=1', '--', url, ref)
            except subprocess.CalledProcessError as error:
                raise ValueError(f'Cannot fetch {project} ref {ref!r}; verify the ref exists, '
                                 f'job-token access remains allowed, and GitLab is reachable. Git error: {git_error(error)}') from None
            revision = 'FETCH_HEAD'
        else:
            revision = 'HEAD'
        sha = run('git', '-C', repo, 'rev-parse', f'{revision}^{{commit}}', capture=True)
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


def build_core(core, core_image, arch, docker_env):
    # Match the selected core revision's own CI release metadata.
    class GitLabLoader(yaml.SafeLoader):
        pass
    GitLabLoader.add_constructor('!reference', lambda loader, node: loader.construct_sequence(node))
    try:
        config = yaml.load((core / '.gitlab-ci.yml').read_text(), Loader=GitLabLoader)
    except yaml.YAMLError as error:
        raise ValueError(f'Invalid core CI YAML: {error}') from None
    variables = config.get('variables') if isinstance(config, dict) else None
    release = variables.get('KASM_RELEASE') if isinstance(variables, dict) else None
    if not isinstance(release, str) or not release.strip() or '$' in release:
        raise ValueError('Core CI must define a non-empty literal KASM_RELEASE')
    run('docker', 'build', '--pull=false', '--platform', f'linux/{arch}', '-t', core_image,
        '--build-arg', 'BASE_IMAGE=ubuntu:24.04', '--build-arg', 'DISTRO=ubuntu',
        '--build-arg', f'KASM_RELEASE={release}',
        '-f', core / 'dockerfile-kasm-core', core, env=docker_env)
    return release


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


def smoke_test(image, package, work, core_image, name, report_dir):
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
                desktop = subprocess.run(['docker', 'exec', name, 'bash', '-ec',
                    'pgrep -u 1000 -x xfce4-session >/dev/null; '
                    'pgrep -u 1000 -x xfwm4 >/dev/null; '
                    'xprop -root _NET_SUPPORTING_WM_CHECK | grep -q "window id # 0x[1-9a-fA-F]"'],
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                if desktop.returncode == 0:
                    return
            if run('docker', 'inspect', '-f', '{{.State.Running}}', name, capture=True) != 'true':
                break
            time.sleep(2)
        raise ValueError('Workspace startup/HTTPS smoke check failed')
    finally:
        logs = subprocess.run(['docker', 'logs', name], capture_output=True, text=True)
        try:
            (report_dir / 'smoke.log').write_text(
                (logs.stdout + logs.stderr).replace(password, '[redacted]'))
        except OSError as error:
            print(f'Warning: cannot save smoke log: {error}', flush=True)
        subprocess.run(['docker', 'rm', '-f', name], check=False, stdout=subprocess.DEVNULL)


def publish(image, alias, labels, report=None, save_report=lambda: None):
    run('docker', 'push', image)
    if report is not None:
        report['published'] = True
        report['publish_state'] = 'passed'
        save_report()
    # The CI resource_group serializes publishers for this branch slug + architecture.
    # Check existing alias ownership/order before replacing it; immutable tags remain usable.
    previous = subprocess.run(['docker', 'pull', alias], capture_output=True, text=True)
    update_alias = True
    if previous.returncode == 0:
        existing = json.loads(run('docker', 'image', 'inspect', '-f',
                                 '{{json .Config.Labels}}', alias, capture=True))
        if not isinstance(existing, dict) or not existing:
            raise ValueError('Existing branch alias has missing/invalid labels')
        if existing.get('com.kasm.workspace.branch') != labels['com.kasm.workspace.branch']:
            raise ValueError('Sanitized branch tag collision; refusing to overwrite another branch')
        previous_pipeline = existing.get('com.kasm.workspace.pipeline')
        if not isinstance(previous_pipeline, str) or not previous_pipeline.isdigit():
            raise ValueError('Existing branch alias has missing/invalid pipeline label; cannot establish ordering')
        update_alias = int(previous_pipeline) <= int(labels['com.kasm.workspace.pipeline'])
    elif not any(s in previous.stderr.lower() for s in ('manifest unknown', 'manifest_unknown', 'not found')):
        raise ValueError('Cannot check registry branch alias: ' + previous.stderr)
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
    parser.add_argument('--package-dir', type=Path, help='Defaults to output/portable_<arch>')
    parser.add_argument('--no-push', action='store_true', help='Build/smoke-test locally without registry writes')
    args = parser.parse_args()
    pipeline, commit, job = (require_env(k) for k in ('CI_PIPELINE_ID', 'CI_COMMIT_SHA', 'CI_JOB_ID'))
    arch = args.arch
    report_dir = ROOT / 'workspace-image-report'
    report_dir.mkdir(exist_ok=True)
    registry = registry_settings() if not args.no_push else None
    registry_image = registry['CI_REGISTRY_IMAGE'] if registry else 'kasmvnc-local'
    slug = require_env('CI_COMMIT_REF_SLUG')
    image = f'{registry_image}/ubuntu-noble-desktop:{slug}-{pipeline}-{job}_{arch}'
    alias = f'{registry_image}/ubuntu-noble-desktop:{slug}_{arch}'
    core_image = f'kasmweb/core-ubuntu-noble:kasmvnc-{pipeline}-{job}_{arch}'
    name = 'kasmvnc-workspace-smoke-' + secrets.token_hex(6)
    report = dict(image=image, branch_image=alias, architecture=arch, published=False,
                  build='pending', smoke_test='pending', publish_state='pending', alias_state='pending')
    def save_report():
        (report_dir / 'image.json').write_text(json.dumps(report, indent=2) + '\n')
    save_report()
    (report_dir / 'README.txt').write_text(
        f'Image: {image}\nBranch alias: {alias}\nArchitecture: {arch}\n'
        'Check image.json stage states before using these references. Pending is not published.\n')
    stage = 'preflight'
    report[stage] = 'running'
    try:
        package, checksum, producer = select_package(args.package_dir or ROOT / f'output/portable_{arch}', arch, pipeline, commit)
        daemon_arch = run('docker', 'info', '--format', '{{.Architecture}}', capture=True)
        if daemon_arch not in {'amd64': ('x86_64', 'amd64'), 'arm64': ('aarch64', 'arm64')}[arch]:
            raise ValueError('Use a native Docker daemon matching the package architecture')
        branch = require_env('CI_COMMIT_REF_NAME')
        for project, ref, source in (
                ('workspaces-core-images', os.environ.get('WORKSPACE_CORE_REF', 'develop'), args.core_source),
                ('workspaces-images', os.environ.get('WORKSPACE_DESKTOP_REF', 'develop'), args.desktop_source)):
            if source is None:
                preflight_access(project, ref)
        if registry:
            run('docker', 'login', registry['CI_REGISTRY'], '-u', registry['CI_REGISTRY_USER'],
                '--password-stdin', input=registry['CI_REGISTRY_PASSWORD'] + '\n')
        report[stage] = 'passed'
        stage = 'build'
        report[stage] = 'running'
        save_report()
        with tempfile.TemporaryDirectory(prefix='workspace-build-') as tmp:
            work = Path(tmp)
            core, desktop = work / 'core', work / 'desktop'
            core_sha = export_source('workspaces-core-images', os.environ.get('WORKSPACE_CORE_REF', 'develop'),
                                     core, args.core_source)
            desktop_sha = export_source('workspaces-images', os.environ.get('WORKSPACE_DESKTOP_REF', 'develop'),
                                        desktop, args.desktop_source)
            prepare_core(core, package)
            labels = {'org.opencontainers.image.revision': commit,
                      'com.kasm.workspace.branch': branch,
                      'com.kasm.workspace.pipeline': pipeline,
                      'com.kasm.workspace.package.sha256': checksum,
                      'com.kasm.workspace.core.revision': core_sha,
                      'com.kasm.workspace.desktop.revision': desktop_sha}
            report.update(labels, package_job=producer)
            (report_dir / 'image.json').write_text(json.dumps(report, indent=2) + '\n')
            # Force the Docker driver: a docker-container buildx driver cannot see daemon-local FROMs.
            docker_env = dict(os.environ, BUILDX_BUILDER='default', DOCKER_BUILDKIT='1')
            report['kasm_release'] = build_core(core, core_image, arch, docker_env)
            if 'FROM kasmweb/$BASE_IMAGE:$BASE_TAG' not in (desktop / 'dockerfile-kasm-ubuntu-noble-desktop').read_text():
                raise ValueError('Desktop Dockerfile base-image interface changed')
            label_args = [arg for key, value in labels.items() for arg in ('--label', f'{key}={value}')]
            run('docker', 'build', '--pull=false', '--platform', f'linux/{arch}', '-t', image,
                '--build-arg', 'BASE_IMAGE=core-ubuntu-noble', '--build-arg', f'BASE_TAG={core_image.split(":")[-1]}',
                *label_args, '-f', desktop / 'dockerfile-kasm-ubuntu-noble-desktop', desktop, env=docker_env)
            report[stage] = 'passed'
            stage = 'smoke_test'
            report[stage] = 'running'
            save_report()
            smoke_test(image, package, work, core_image, name, report_dir)
            report[stage] = 'passed'
            save_report()
            report['local_image_id'] = run('docker', 'image', 'inspect', '-f', '{{.Id}}', image, capture=True)
            if not args.no_push:
                stage = 'publish_state'
                report[stage] = 'running'
                save_report()
                def published():
                    nonlocal stage
                    stage = 'alias_state'
                    report[stage] = 'running'
                    save_report()
                report['branch_tag_updated'], report['digests'] = publish(image, alias, labels, report, published)
                report[stage] = 'passed' if report['branch_tag_updated'] else 'skipped'
            else:
                report['publish_state'] = report['alias_state'] = 'skipped'
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

    except BaseException as error:
        report[stage] = 'interrupted' if isinstance(error, (SystemExit, KeyboardInterrupt)) else 'failed'
        report['error'] = str(error)
        if report['published']:
            print(f'Immutable image was published: {image}; see report for alias state.', flush=True)
        raise
    finally:
        try:
            save_report()
        except OSError as error:
            print(f'Warning: cannot save final report: {error}', flush=True)
        finally:
            subprocess.run(['docker', 'rm', '-f', name], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            subprocess.run(['docker', 'image', 'rm', core_image], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            if registry:
                subprocess.run(['docker', 'image', 'rm', image, alias],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                subprocess.run(['docker', 'logout', registry['CI_REGISTRY']], stdout=subprocess.DEVNULL)


if __name__ == '__main__':
    install_signal_handlers()
    try:
        main()
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        raise SystemExit(f'Workspace image build failed: {error}')
