"""
PlatformIO pre-build script: inject version defines for CI/test envs.

Product version comes from platformio.ini [crosspoint] crossink_version
(Casper v0.1). default/tiny/debug set CROSSINK_VERSION in platformio.ini;
this script only fills envs that still need git-based suffixes (test, RC).
"""

import configparser
import os
import re
import subprocess
import sys


def warn(msg):
    print(f'WARNING [git_branch.py]: {msg}', file=sys.stderr)


def get_git_short_hash(project_dir, length=5):
    try:
        return subprocess.check_output(
            ['git', 'rev-parse', '--short', 'HEAD'],
            text=True, stderr=subprocess.PIPE, cwd=project_dir
        ).strip()[:length]
    except Exception as e:
        warn(f'Could not read git hash: {e}; hash will be "00000"')
        return '00000'


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
    return sanitize_version_component(branch)


def sanitize_version_component(value):
    value = value.strip()
    value = re.sub(r'[^A-Za-z0-9._-]+', '-', value)
    value = re.sub(r'-{2,}', '-', value)
    value = value.strip('-.')
    return value or 'unknown'


def get_git_short_sha(project_dir):
    return run_git_value(
        project_dir, ['rev-parse', '--short', 'HEAD'], 'short SHA'
    )


def _read_ini(project_dir):
    ini_path = os.path.join(project_dir, 'platformio.ini')
    config = configparser.ConfigParser()
    if os.path.isfile(ini_path):
        config.read(ini_path)
    else:
        warn(f'platformio.ini not found at {ini_path}')
    return config


def get_crossink_version(project_dir):
    config = _read_ini(project_dir)
    if not config.has_option('crossink', 'version'):
        warn(
            'No [crossink] version in platformio.ini; '
            'build version will be "0.0.0"'
        )
        return '0.0.0'
    return config.get('crossink', 'version')


def inject_version(env):
    project_dir = env['PROJECT_DIR']
    pioenv = env['PIOENV']
    product = get_crossink_version(project_dir)

    # default / tiny / debug / xlarge / simulator: version is set in platformio.ini.
    if pioenv in ('default', 'tiny', 'debug', 'xlarge', 'simulator'):
        print(f'Casper build version: {product} (env={pioenv})')
        return

    if pioenv == 'test':
        branch = get_git_branch(project_dir)
        short_hash = get_git_short_hash(project_dir)
        version_string = f'{product}-{branch}+{short_hash}'
        env.Append(CPPDEFINES=[
            ('CROSSINK_VERSION', f'\\"{version_string}\\"'),
        ])
        print(f'Casper test build version: {version_string}')

    elif pioenv == 'gh_release_rc':
        short_hash = os.environ.get('CROSSPOINT_RC_HASH') or get_git_short_hash(project_dir)
        version_string = f'{product}-rc+{short_hash}'
        env.Append(CPPDEFINES=[
            ('CROSSINK_VERSION', f'\\"{version_string}\\"'),
        ])
        print(f'Casper RC build version: {version_string}')


# PlatformIO/SCons entry point — Import and env are SCons builtins injected at runtime.
# When run directly with Python (e.g. for validation), a lightweight fake env is used
# so the git/version logic can be exercised without a full build.
try:
    Import('env')  # noqa: F821  # type: ignore[name-defined]
except NameError:
    class _Env(dict):
        def Append(self, **_): pass

    if '__file__' in globals():
        _project_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    else:
        _project_dir = os.getcwd()
    inject_version(_Env({'PIOENV': 'default', 'PROJECT_DIR': _project_dir}))
else:
    inject_version(env)  # noqa: F821  # type: ignore[name-defined]
