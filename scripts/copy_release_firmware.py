"""
PlatformIO post-build: copy gh_release firmware.bin to dist/CrossPoint-<version>.bin.

Naming matches shipping releases, e.g. CrossPoint-v0.1.2.bin from [casper] version = v0.1.2.
"""

Import("env")  # pylint: disable=undefined-variable

import configparser
import os
import shutil


def _casper_version(project_dir: str) -> str:
    ini_path = os.path.join(project_dir, "platformio.ini")
    config = configparser.ConfigParser()
    config.read(ini_path)
    if config.has_option("casper", "version"):
        return config.get("casper", "version").strip()
    return "v0.0.0"


def copy_release_firmware(source, target, env):  # pylint: disable=unused-argument
    project_dir = env["PROJECT_DIR"]
    version = _casper_version(project_dir)
    # [casper] version already includes the "v" prefix → CrossPoint-v0.1.2.bin
    out_name = f"CrossPoint-{version}.bin"
    dist_dir = os.path.join(project_dir, "dist")
    os.makedirs(dist_dir, exist_ok=True)
    src = env.subst("$BUILD_DIR/firmware.bin")
    dst = os.path.join(dist_dir, out_name)
    if not os.path.isfile(src):
        print(f"WARNING [copy_release_firmware]: missing {src}")
        return
    shutil.copy2(src, dst)
    print(f"Copied release firmware → dist/{out_name}")


# Run after the firmware binary is produced.
env.AddPostAction("$BUILD_DIR/firmware.bin", copy_release_firmware)  # pylint: disable=undefined-variable
