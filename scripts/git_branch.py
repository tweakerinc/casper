"""
PlatformIO pre-build script.

Two jobs:

1. CROSSPOINT_VERSION -- the *product* version, e.g. "v0.1.9". Injected here for the
   default (dev) environment; release environments set it in platformio.ini.

2. CROSSPOINT_BUILD_ID -- the *build* fingerprint, e.g. "05c6cf8" or "05c6cf8-dirty".
   Injected for every environment.

The distinction matters for support. CROSSPOINT_VERSION only changes when the
product version is bumped, so consecutive test builds all report the same
string: a device log reading "ver=v0.1.9" could have come from any of them, and
there was no way to tell which firmware produced a capture. CROSSPOINT_BUILD_ID
pins a log to a commit, and matches the short SHA in dist/CrossPoint-NNNN-<sha>.bin.
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


def get_build_id(project_dir):
    """Short SHA, suffixed '-dirty' when the tree has uncommitted changes.

    A dirty build is not reproducible from the SHA alone, so say so rather than
    let a capture claim to be a commit it is not.
    """
    sha = get_git_short_sha(project_dir)
    try:
        subprocess.check_call(
            ['git', 'diff', '--quiet', 'HEAD'],
            cwd=project_dir,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
    except subprocess.CalledProcessError:
        return f'{sha}-dirty'
    except (FileNotFoundError, OSError):
        return sha
    return sha


def inject_version(env):
    project_dir = env['PROJECT_DIR']

    # Build fingerprint: every environment, so any capture identifies its build.
    build_id = get_build_id(project_dir)
    env.Append(CPPDEFINES=[
        ('CROSSPOINT_BUILD_ID', f'\\"{build_id}\\"'),
    ])
    print(f'CrossPoint build id: {build_id}')

    # Product version: dev only; release envs set it via build_flags.
    if env['PIOENV'] != 'default':
        return

    version_string = get_base_version(project_dir)
    env.Append(CPPDEFINES=[
        ('CROSSPOINT_VERSION', f'\\"{version_string}\\"'),
    ])
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
