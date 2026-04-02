#pragma once

#include <Arduino.h>
#include <functional>

// Check GitHub releases for a newer firmware version.
// Returns true if a newer version is available; fills latestVersion with the tag.
bool ota_check_for_update(String& latestVersion);

// Compare a release tag (e.g. "v1.2.3") against FIRMWARE_VERSION.
// Returns true if latestTag is strictly newer.
bool ota_is_update_newer(const String& latestTag);

// Download and install the firmware.bin asset from the latest release.
// progressCallback receives percent (0-100).  Returns true on success.
bool ota_install_update(std::function<void(int)> progressCallback);
