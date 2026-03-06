"""
PlatformIO pre-build script: inject git branch into CROSSPOINT_VERSION for
the default (dev) environment.

Results in a version string like:  1.1.0-dev+feat-koysnc-xpath
Release environments are unaffected; they set CROSSPOINT_VERSION in the ini.
"""

import configparser
import os
import subprocess
import sys


def warn(msg):
    print(f'WARNING [git_branch.py]: {msg}', file=sys.stderr)


def get_git_branch(project_dir):
    try:
        branch = subprocess.check_output(
            ['git', 'rev-parse', '--abbrev-ref', 'HEAD'],
            text=True, stderr=subprocess.PIPE, cwd=project_dir
        ).strip()
        # Detached HEAD — show the short SHA instead
        if branch == 'HEAD':
            branch = subprocess.check_output(
                ['git', 'rev-parse', '--short', 'HEAD'],
                text=True, stderr=subprocess.PIPE, cwd=project_dir
            ).strip()
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


def inject_version(env):
    # Only applies to the dev (default) environment; release envs set the
    # version via build_flags in platformio.ini and are unaffected.
    if env['PIOENV'] != 'default':
        return

    project_dir = env['PROJECT_DIR']
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
