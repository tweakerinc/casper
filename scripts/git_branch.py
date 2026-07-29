"""
PlatformIO pre-build script: inject git branch and short SHA into
CROSSPOINT_VERSION for the default (dev) environment.

Results in a version string like:  1.1.0-dev-feat-kosync-xpath-05c6cf8
Release environments are unaffected; they set CROSSPOINT_VERSION in the ini.
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
    """Prefer [casper] version (Casper product), then legacy [crosspoint]."""
    config, ini_path = _read_ini(project_dir)
    if config is None:
        warn(f'platformio.ini not found at {ini_path}; base version will be "0.0.0"')
        return '0.0.0'
    if config.has_option('casper', 'version'):
        return config.get('casper', 'version')
    if config.has_option('crosspoint', 'version'):
        return config.get('crosspoint', 'version')
    warn('No [casper]/[crosspoint] version in platformio.ini; base version will be "0.0.0"')
    return '0.0.0'


def inject_version(env):
    # Only applies to the dev (default) environment; release envs set the
    # version via build_flags in platformio.ini and are unaffected.
    if env['PIOENV'] != 'default':
        return

    project_dir = env['PROJECT_DIR']
    base_version = get_base_version(project_dir)
    # Casper product builds: inject clean product version as-is (e.g. "v0.1.2"),
    # no -dev-branch-sha suffix. Upstream CrossPoint keeps the long dev stamp.
    product = ''
    config, _ = _read_ini(project_dir)
    if config is not None:
        if config.has_option('casper', 'product'):
            product = config.get('casper', 'product')
        elif config.has_option('crosspoint', 'product'):
            product = config.get('crosspoint', 'product')
    is_casper = product.lower() == 'casper' or base_version.startswith('casper') or (
        base_version.startswith('v') and len(base_version) > 1 and base_version[1].isdigit()
    )
    if is_casper:
        version_string = base_version
    else:
        branch = get_git_branch(project_dir)
        short_sha = get_git_short_sha(project_dir)
        version_string = f'{base_version}-dev-{branch}-{short_sha}'

    # Both macros so Boot/Settings/OTA see the same clean string.
    env.Append(CPPDEFINES=[
        ('CASPER_VERSION', f'\\"{version_string}\\"'),
        ('CROSSPOINT_VERSION', f'\\"{version_string}\\"'),
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
