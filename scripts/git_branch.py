"""
PlatformIO pre-build script: inject Git metadata into preprocessor defines.

- The default (dev) environment gets CROSSPOINT_VERSION with a branch suffix like:
  1.1.0-dev+feat-koysnc-xpath
- The gh_release_rc environment gets CROSSPOINT_VERSION with an RC tag from CI metadata
  when available, or a local fallback like: 1.1.0-rc+local
- All environments get CROSSPOINT_GIT_REPOSITORY, resolved from CI metadata
  or local Git remotes. A safe fallback is defined in src/network/OtaUpdater.h in case
  resolution here fails.
"""

import configparser
import os
import re
import subprocess
import sys


def warn(msg):
    print(f'WARNING [git_branch.py]: {msg}', file=sys.stderr)


def run_git_command(*args: str, project_dir: str) -> str:
    try:
        return subprocess.check_output(
            ['git', *args],
            text=True, stderr=subprocess.PIPE, cwd=project_dir
        ).strip()
    except FileNotFoundError:
        warn('git not found on PATH')
        raise
    except subprocess.CalledProcessError as e:
        warn(f'git command "git {" ".join(args)}" failed (exit {e.returncode}): {e.stderr.strip()}')
        raise


def get_git_branch(project_dir):
    try:
        branch = run_git_command('rev-parse', '--abbrev-ref', 'HEAD', project_dir=project_dir)
        # Detached HEAD — show the short SHA instead
        if branch == 'HEAD':
            branch = run_git_command('rev-parse', '--short', 'HEAD', project_dir=project_dir)
        # Strip characters that would break a C string literal
        return ''.join(c for c in branch if c not in '"\\')
    except FileNotFoundError:
        warn('git not found on PATH; branch suffix will be "unknown"')
        return 'unknown'
    except subprocess.CalledProcessError as e:
        warn(f'git command failed (exit {e.returncode}): {e.stderr.strip()}; branch suffix will be "unknown"')
        return 'unknown'
    except Exception as e:
        warn(f'Unexpected error reading git branch: {e}; branch suffix will be "unknown"')
        return 'unknown'


def get_all_remotes(project_dir: str) -> list[str]:
    try:
        remotes = run_git_command('remote', project_dir=project_dir)
        return remotes.splitlines()
    except FileNotFoundError:
        warn('git not found on PATH; cannot read git remotes')
        return []
    except subprocess.CalledProcessError as e:
        warn(f'git command failed (exit {e.returncode}): {e.stderr.strip()}; cannot read git remotes')
        return []
    except Exception as e:
        warn(f'Unexpected error reading git remotes: {e}; cannot read git remotes')
        return []


def parse_git_repository(remote_url: str) -> str | None:
    # Match strings like:
    # - https://github.com/owner/repo.git
    # - https://code.example.com/owner/repo
    # - git+ssh://vcs.example.org:owner/repo.git
    # - codeberg.org:owner/repo.git
    match = re.search(r'^(?:.+)?(?:://)?[^:/]+[:/]([^/]+)/([^/]+?)(?:\.git)?$', remote_url.strip())
    if not match:
        return None
    owner = match.group(1)
    repo = match.group(2)
    if not owner or not repo:
        return None
    return f'{owner}/{repo}'


def get_git_remote_url(project_dir, remote_name):
    try:
        return run_git_command('remote', 'get-url', remote_name, project_dir=project_dir)
    except (FileNotFoundError, subprocess.CalledProcessError):
        return None


def get_git_repository(project_dir):
    # Other CI systems (Forgejo, Codeberg) may set GITHUB_REPOSITORY for compatibility
    # with GHA. We could also check for other CI-specific env vars to expand support
    # later, such as:
    # - FORGEJO_REPOSITORY
    # - CI_REPOSITORY_URL (this one is a full URL, likely will work with parse_git_repository)
    # - BITBUCKET_REPO_FULL_NAME
    ci_repository = os.environ.get('GITHUB_REPOSITORY')
    if ci_repository:
        return ci_repository

    remotes = get_all_remotes(project_dir)
    # 'origin' is most likely to be the primary remote, so always check for it first
    if 'origin' in remotes:
        remotes = ['origin'] + [r for r in remotes if r != 'origin']
    for remote_name in remotes:
        remote_url = get_git_remote_url(project_dir, remote_name)
        if not remote_url:
            continue
        repository = parse_git_repository(remote_url)
        if repository:
            return repository

    warn(
        'Could not resolve a repository from CI metadata or git remotes; '
        'falling back to compile-time default.'
    )
    return None


def get_base_version(project_dir):
    ini_path = os.path.join(project_dir, 'platformio.ini')
    if not os.path.isfile(ini_path):
        warn(f'platformio.ini not found at {ini_path}; base version will be "0.0.0"')
        return '0.0.0'
    config = configparser.ConfigParser()
    config.read(ini_path)
    if not config.has_option('crosspoint', 'version'):
        warn('No [crosspoint] version in platformio.ini; base version will be "0.0.0"')
        return '0.0.0'
    return config.get('crosspoint', 'version')


def normalize_semver_patch(version: str) -> str:
    version = version.strip()
    if version.count('.') == 1:
        return f'{version}.0'
    return version


def inject_version(env):
    project_dir = env['PROJECT_DIR']
    git_repository = get_git_repository(project_dir)
    if git_repository:
        env.Append(CPPDEFINES=[('CROSSPOINT_GIT_REPOSITORY', f'\\"{git_repository}\\"')])
        print(f'CrossPoint Git repository: {git_repository}')

    # Release candidate builds use the CI-provided RC tag when available, but
    # keep local gh_release_rc builds identifiable instead of leaving the
    # firmware version empty.
    if env['PIOENV'] == 'gh_release_rc':
        base_version = normalize_semver_patch(get_base_version(project_dir))
        version_string = os.environ.get('CROSSPOINT_RC_VERSION') or f'{base_version}-rc.0+local'
        env.Append(CPPDEFINES=[('CROSSPOINT_VERSION', f'\\"{version_string}\\"')])
        print(f'CrossPoint build version: {version_string}')
        return

    # Only applies to the dev (default) environment; release envs set the
    # version via build_flags in platformio.ini and are unaffected.
    if env['PIOENV'] != 'default':
        return

    base_version = get_base_version(project_dir)
    branch = get_git_branch(project_dir)
    version_string = f'{base_version}-dev+{branch}'

    env.Append(CPPDEFINES=[('CROSSPOINT_VERSION', f'\\"{version_string}\\"')])
    print(f'CrossPoint build version: {version_string}')


# PlatformIO/SCons entry point — Import and env are SCons builtins injected at runtime.
# When run directly with Python (e.g. for validation), a lightweight fake env is used
# so the git/version logic can be exercised without a full build.
try:
    Import('env')           # noqa: F821  # type: ignore[name-defined]
    inject_version(env)     # noqa: F821  # type: ignore[name-defined]
except NameError:
    class _Env(dict):
        def Append(self, **_): pass

    _project_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    inject_version(_Env({'PIOENV': 'default', 'PROJECT_DIR': _project_dir}))
