<!-- SPDX-License-Identifier: MIT -->
# Runtime detection policy

Drive GPU Lab follows an evidence-first rule: **identify when observable; otherwise report unknown**.

## Game

Windows version resources provide ProductName/ProductVersion/FileDescription/FileVersion. The executable itself is SHA-256 hashed.

### Automatic target selection

In automatic mode, Drive GPU Lab prefers a live candidate PID that emitted a valid `vklayer` `device` event from the explicit Drive GPU Lab Vulkan layer. When a game is launched normally from Winlator/Bannerlator and therefore does not inherit the separately started probe's layer environment, the probe falls back to native Windows process/window discovery: it selects the largest visible non-shell process, also considering child windows used by Wine virtual desktops. Explicit `--pid` and `--process` modes remain available for controlled targeting. Vulkan device evidence enriches and confirms a session when available; it is not a prerequisite for native auto-target discovery.

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


## Automatic shared-runtime log discovery

The stacked G720Probe auto-log candidate extends session-local log correlation without
changing the public release. During an attached session it periodically inspects the
Wine-visible Android shared Documents roots for Winlator/Bannerlator logs, including
`logs.txt`, `box64-*.txt`, Wine, DXVK, VKD3D, Vulkan, Mesa and PanVK text/log files.

Default mapped roots:

- `Z:\storage\emulated\0\Documents\Winlator`
- `Z:\storage\emulated\0\Documents\winlator`
- `Z:\storage\emulated\0\Documents\Bannerlator`
- `Z:\storage\emulated\0\Documents\bannerlator`

Additional semicolon-separated Windows roots can be supplied through `DGL_LOG_ROOTS`.
Only recently modified files are considered, archive/output directories such as
`previous` and DriveGpuLab are skipped, and each source starts tailing at its current
EOF so stale pre-session content is not merged into the new session. Discovered sources
are recorded in `raw/log-sources.tsv`.
