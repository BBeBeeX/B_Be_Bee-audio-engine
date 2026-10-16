# Third-Party Dependencies & Precompiled Library Specifications

## 1. Zero Binary Policy in Git

In strict adherence to repository hygiene and Git scalability principles, **no precompiled large binary files (`.so`, `.dll`, `.dylib`, `.tar.gz`, `.zip`) are stored or tracked inside this repository's Git history**.

Storing 400MB+ of binary libraries across macOS, Windows, and Linux directly inside the Git database causes clone times to explode, pushes repository size beyond Git hosting limits, and interferes with diffs.

All precompiled multi-platform libraries are distributed exclusively via **GitHub Releases** and dynamically pulled by client build scripts with cryptographic hash validation.

## 2. Integrity Verification & Provenance

During the decoupling audit (Group 0), all existing precompiled binaries across `apps/desktop/resources/libmpv` and `apps/desktop2/src-tauri/lib` were verified 100% byte-for-byte identical via cross-platform MD5 checksums:

| Platform | Library | Role | Verified MD5 Checksum |
|---|---|---|---|
| **macOS (arm64)** | `darwin/libmpv.2.dylib` | Patched mpv core | `89e1859c95a988fdf47bb29da75ec660` |
| **Linux (x64)** | `linux/libmpv.so.2` | Patched mpv core | `dc746ce206c812a5bd84ac96531e7707` |
| **Windows (x64)** | `win64/libmpv-2.dll` | Patched mpv core | `6e18221ba07c050aeaa757c39708369a` |
| **Linux (x64)** | `linux/libasound.so.2` | ALSA audio stack | `6aeaba120dd68c9f0eed26cf3ee947b4` |
| **Windows (x64)** | `win64/zlib1.dll` | zlib compression | `30a31b969a3356efde11078dc635baa0` |

## 3. Dynamic Dependency Management & Manifest Schema

Detailed library catalogs and platform pruning policies are documented in `manifest.json`.

### Dual-Package Release Architecture (D2)
For each desktop platform, GitHub Releases distribute two variants:
- **Full Package** (`audio-engine-<platform>-<arch>-full.tar.gz` or `.zip`):
  Contains the `audio-engine` executable along with the complete closure of FFmpeg, audio backends, and helper dynamic libraries. Recommended for production application bundling.
- **Slim Package** (`audio-engine-<platform>-<arch>-slim.tar.gz` or `.zip`):
  Contains only the `audio-engine` executable and the patched `libmpv` shared library. Relies on the host OS package manager (e.g. system FFmpeg, ALSA, PulseAudio).

### Verification Rules
When automated fetch scripts download release archives:
1. The archive is hashed with SHA-256 and matched against `SHA256SUMS.txt` and `audio-engine.version.json`.
2. Core `libmpv` dynamic libraries are verified against the authoritative MD5 / SHA-256 baseline before placement into `bin/` or `resources/bin/`.
3. Any mismatch triggers an immediate hard abort preventing compromised or corrupt libraries from reaching runtime environments.
