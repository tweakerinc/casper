"""
PlatformIO pre-build script: inject git branch and short SHA into
CASPER_VERSION for the default (dev) environment.

Results in a version string like:  v0.1.9-dev-feature-branch-05c6cf8
Release environments are unaffected; they set CASPER_VERSION in the ini.
"""

import configparser
import os
import subprocess
import sys


def warn(msg):
    print(f'WARNING [git_branch.py]: {msg}', file=sys.stderr)


def run_git_value(project_dir, args, label):
    try:
        value = subprocess.check_output(
            ['git', *args],
            text=True, stderr=subprocess.PIPE, cwd=project_dir
        ).strip()
        # Strip characters that would break a C string literal
        return ''.join(c for c in value if c not in '"\\')
    except FileNotFoundError:
        warn(f'git not found on PATH; {label} suffix will be "unknown"')
        return 'unknown'
    except subprocess.CalledProcessError as e:
        warn(
            f'git command failed (exit {e.returncode}): '
            f'{e.stderr.strip()}; {label} suffix will be "unknown"'
        )
        return 'unknown'
    except OSError as e:
        warn(
            f'OS error reading git {label}: {e}; '
            f'{label} suffix will be "unknown"'
        )
        return 'unknown'
    except Exception as e:  # pylint: disable=broad-exception-caught
        warn(
            f'Unexpected error reading git {label}: {e}; '
            f'{label} suffix will be "unknown"'
        )
        return 'unknown'


def get_git_branch(project_dir):
    branch = run_git_value(
        project_dir, ['rev-parse', '--abbrev-ref', 'HEAD'], 'branch'
    )
    # Detached HEAD has no branch name.
    if branch == 'HEAD':
        return 'detached'
    return branch


def get_git_short_sha(project_dir):
    return run_git_value(
        project_dir, ['rev-parse', '--short', 'HEAD'], 'short SHA'
    )


def _read_ini(project_dir):
    ini_path = os.path.join(project_dir, 'platformio.ini')
    if not os.path.isfile(ini_path):
        return None, ini_path
    config = configparser.ConfigParser()
    config.read(ini_path)
    return config, ini_path


def get_base_version(project_dir):
    """Read [casper] version from platformio.ini."""
    config, ini_path = _read_ini(project_dir)
    if config is None:
        warn(f'platformio.ini not found at {ini_path}; base version will be "0.0.0"')
        return '0.0.0'
    if config.has_option('casper', 'version'):
        return config.get('casper', 'version')
    warn('No [casper] version in platformio.ini; base version will be "0.0.0"')
    return '0.0.0'


def inject_version(env):
    # Only applies to the dev (default) environment; release envs set the
    # version via build_flags in platformio.ini and are unaffected.
    if env['PIOENV'] != 'default':
        return

    project_dir = env['PROJECT_DIR']
    version_string = get_base_version(project_dir)

    env.Append(CPPDEFINES=[
        ('CASPER_VERSION', f'\\"{version_string}\\"'),
    ])
    print(f'Casper build version: {version_string}')


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
