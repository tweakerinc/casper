"""
PlatformIO post-build: do not dump a second name into dist/.

Shipping / test bins are published only by scripts/dist_bin.sh as
Casper-v0.1.9.NNNN.bin. A post-script copy of CrossPoint-<version>.bin used to
land in dist/ on every gh_release compile and fight that numbering.
"""

Import("env")  # pylint: disable=undefined-variable


def copy_release_firmware(source, target, env):  # pylint: disable=unused-argument
    print("Release firmware is at $BUILD_DIR/firmware.bin — publish with scripts/dist_bin.sh")


# Keep the hook so extra_scripts does not surprise anyone, but do not write dist/.
env.AddPostAction("$BUILD_DIR/firmware.bin", copy_release_firmware)  # pylint: disable=undefined-variable
