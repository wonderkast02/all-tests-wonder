# Contributing

Keep changes scoped, reproducible and reviewable.

Before proposing a change:

- keep generated binaries out of Git history;
- preserve existing SPDX/license notices;
- run `git diff --check`;
- build and test the component you changed where practical;
- document behavioral, format or protocol changes;
- avoid claiming a performance improvement from a single uncontrolled run;
- preserve backward compatibility of recorded session evidence unless the
  schema/protocol version is intentionally changed.

Drive GPU Lab-specific architecture and test guidance lives under
`tools/drive-gpu-lab/docs/`.
