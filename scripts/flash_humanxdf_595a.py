Import("env")

import os

XDF_NAME = "human302.xdf"
XDF_SIZE = 1261568
FLASH_OFFSET = "0x810000"

project_dir = env.subst("$PROJECT_DIR")
xdf_path = os.path.abspath(os.path.join(project_dir, XDF_NAME))

if not os.path.isfile(xdf_path):
    print("ERROR: Build 5.95a requires: %s" % xdf_path)
    env.Exit(1)

actual_size = os.path.getsize(xdf_path)
if actual_size != XDF_SIZE:
    print("ERROR: %s must be %d bytes, got %d" % (xdf_path, XDF_SIZE, actual_size))
    env.Exit(1)

# PanicPlayer-style dedicated raw flash partition.  POST scripts run after
# PlatformIO has constructed the normal ESP-IDF upload command, so append the
# raw image pair directly to UPLOADERFLAGS.  The app image remains a separate
# normal factory image at $ESP32_APP_OFFSET.
if env.subst("$UPLOAD_PROTOCOL") == "esptool":
    env.Append(UPLOADERFLAGS=[FLASH_OFFSET, xdf_path])

# Also expose it as an extra image to PlatformIO integration/metadata.
env.Append(FLASH_EXTRA_IMAGES=[(FLASH_OFFSET, xdf_path)])

print("Build 5.95a: Human302 XDF -> humanxdf flash @ %s (%d bytes): %s" %
      (FLASH_OFFSET, XDF_SIZE, xdf_path))
