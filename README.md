# All Tests Wonder

**All Tests Wonder** is a standalone test, telemetry, diagnostics and qualification
repository for graphics/runtime experiments maintained by Wonder.

The repository is intentionally independent from any single GPU-driver source
tree. Tools live as self-contained components under `tools/`, with their own
versioning, documentation and licensing.

## Current component

### Drive GPU Lab — `0.1.0-dev.1`

`tools/drive-gpu-lab/` contains the first development version of the Winlator /
Vulkan / PanVK-oriented telemetry stack:

- `G720Probe.exe` — Windows/Wine/Winlator background probe and HUD;
- `G720VkLayer.dll` — Vulkan telemetry layer;
- `g720-hostd` — Android/Linux host telemetry daemon;
- per-game persistent folders with per-session separation;
- automatic runtime/translator/driver identification;
- DXVK, VKD3D-Proton, Wine, Proton, Box64, FEX, Vulkan, Mesa/PanVK and host
  evidence ingestion when those data are actually available;
- structured `timeline.jsonl` plus human-readable `MASTER.log`;
- bug markers, screenshots, ring-buffer context and regression-oriented data.

See [`tools/drive-gpu-lab/README.md`](tools/drive-gpu-lab/README.md) for technical
details and installation instructions.

## Repository principles

1. Evidence must be attributable to an exact session and environment.
2. Unknown data stays unknown; tools should not guess runtime identity.
3. Generated binaries do not belong in Git history when they can be published
   as release or CI assets.
4. Source, CI and release artifacts must be reproducible and checksumable.
5. A developer build is not called stable until it passes the relevant real
   device qualification gates.

## Layout

```text
.github/workflows/      CI / qualification automation
docs/                   repository-level policies
tools/drive-gpu-lab/    Drive GPU Lab source and documentation
artifacts/              textual manifests/checksums for published artifacts
```

## Status

`Drive GPU Lab 0.1.0-dev.1` is developer instrumentation. It is not a Vulkan
conformance statement and does not claim universal game compatibility.
