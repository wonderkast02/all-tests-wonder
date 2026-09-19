<!-- SPDX-License-Identifier: MIT -->
# Privacy and community log handling

Drive GPU Lab is designed for shareable engineering captures but does not automatically upload anything.

## Collected by design

- game executable name/path, Windows version-resource metadata and SHA-256;
- relevant loaded graphics-module names/paths, hashes and identity strings;
- selected runtime-related environment variables;
- DXVK/VKD3D/Wine/Box64/FEX/Proton/Mesa/PanVK-style logs discovered during the session;
- Vulkan/device/performance telemetry;
- Android model/release/build fingerprint, kernel/Kbase/devfreq/thermal/memory facts exposed to the Termux process;
- screenshots only when the user presses F11.

## Not intentionally collected

The host daemon does not request Android ID, hardware serial, Wi-Fi identifiers, IP configuration, account credentials, location or authentication tokens.

## Sharing warning

Paths, third-party logs and explicitly captured screenshots can still contain user-chosen names or unrelated application data. Community submissions should therefore be reviewed before public posting. Automatic upload is intentionally absent from `dev.1`.
