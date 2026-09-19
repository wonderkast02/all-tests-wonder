<!-- SPDX-License-Identifier: MIT -->
# Runtime detection policy

Drive GPU Lab follows an evidence-first rule: **identify when observable; otherwise report unknown**.

## Game

Windows version resources provide ProductName/ProductVersion/FileDescription/FileVersion. The executable itself is SHA-256 hashed.

## Wine / Proton

Wine version is read from Wine's `ntdll` runtime export when present. Proton is marked as detected only from Proton/Steam compatibility environment evidence. The tool does not synthesize a Proton version from an unrelated Wine version.

## Box64 / FEX

Box64 and FEX are detected from their runtime environment indicators. Their relevant environment variables are preserved in `environment.json`. If the runtime does not expose a version string to the Wine guest, the version remains unavailable rather than guessed.

## DXVK / VKD3D-Proton / Vulkan / Mesa / PanVK

The target process module list is inspected. Relevant modules are hashed and scanned for printable identity tokens such as `DXVK`, `vkd3d-proton`, `Mesa` and `PanVK`. Vulkan device identity comes independently from the explicit telemetry layer.

This dual path is intentional: a file name says what was loaded; an embedded identity may say its build/version; the SHA-256 says exactly which bytes were loaded.

## Host GPU / Kbase

`g720-hostd` searches `/sys/class/devfreq` only for nodes whose path/name looks GPU-specific (`mali`, `gpu`, `g3d`, `panfrost`). It reads Kbase version and `/dev/mali0` accessibility when exposed. It never selects an arbitrary devfreq node merely because one exists.

## Environment scope

Only runtime-relevant prefixes are exported to `environment.json`; unrelated user environment is excluded. Current families include DXVK, VKD3D, Wine, Box64, FEX, Proton/Steam compatibility, Vulkan, Mesa, Pan/Mali-related Mesa flags, Gallium, Zink and Winlator variables.
