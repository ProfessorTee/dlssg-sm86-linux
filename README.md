# dlssg-sm86-linux

**NVIDIA DLSS Frame Generation (including multi-frame 3x/4x) on RTX 30 GPUs in native Linux Vulkan games.**

[Deutsche Version](README.de.md)

NVIDIA's DLSS-G runtime refuses to run below RTX 40. On Windows, [sdli1995's DLSSG SM86 mod](https://github.com/sdli1995/dlssg_for_sm86)
solves that for D3D12 games. Vulkan frame generation is not supported there, and the mod does not work for native Linux games at all.
This project is a small `LD_PRELOAD` library that does the same job for the **Linux** NGX stack. It runs NVIDIA's real DLSS-G
model. It is not FSR or a replacement interpolator.

Tested: **RTX 3090, NVIDIA 620.70.06 (open kernel module), DLSS-G 310.9.16, Longshot v3.0.0** (Ocarina of Time recomp),
2x and 4x, 4K output, ~40 fps base → 120 Hz.

## How it works

Everything happens in memory after the libraries were loaded and their signatures checked. No file on disk is changed.

| Where | Gate | Patch |
|---|---|---|
| `libnvidia-ngx.so` (driver's NGX core) | `NGXValidateSnippetMetaData`: *snippet is using newer GPU arch 190 > 170* | skip the reject |
| | per-feature minimum arch in the capability pass (`FrameGeneration.Available`) and `NVSDK_NGX_CreateFeature_Validate` (*feature is not supported on this device*) | skip the compare |
| `libnvidia-ngx-dlssg.so` (DLSS-G snippet) | `GetFeatureRequirements` minimum arch 0x190 | 0x160 |
| | `SetGPUArch` | report Ada (0x190), so the snippet picks its sm_89 kernel set |
| | `DLSSG.MultiFrameCountMax` (per device and for all devices), `mfgSupported` | Blackwell-only branches opened for older GPUs |
| `vkCreateCuModuleNVX` | the snippet's sm_89 kernels do not load on Ampere | swapped by SHA-256 for sm_86 builds from a kernel pack |

The kernel pack comes from sdli1995's release (MIT). `tools/make_pack.py` maps its sm_86 cubins to the blobs of the Linux
snippet: 39 kernels are byte-identical to the Windows runtime, the rest match by PTX entry name. 69 of 71 kernels are covered.
The two missing ones are capture/debug kernels that games don't use.

All patches are byte patterns. A driver or runtime that changes them is reported in the log and left alone, so it can't crash.

## Install

Requirements: NVIDIA RTX 30 (RTX 20 untested: build the pack with `--sm 75`), the proprietary or open NVIDIA driver with NGX (`/usr/lib/libnvidia-ngx.so`), `gcc`,
Vulkan headers, `python-pefile`. The game must ship (or be given) `libnvidia-ngx-dlssg.so.310.9.16`.

```sh
git clone https://github.com/ProfessorTee/dlssg-sm86-linux && cd dlssg-sm86-linux
./build.sh
# kernel pack: from sdli1995's DLSSG SM86 release (0.4.2), file dlssg_sm86/kernels/fg_kernels.dll
python3 tools/make_pack.py /path/to/dlssg_sm86/kernels/fg_kernels.dll \
    /path/to/game/ngx/libnvidia-ngx-dlssg.so.310.9.16 pack/310.9.16-sm86
```

The pack is read from `pack/` next to the library (or `DLSSG_SM86_PACK=<dir>`).

## Use

```sh
./dlssg-sm86 /path/to/game          # sets LD_PRELOAD and starts the game
# Steam launch options:  /path/to/dlssg-sm86-linux/dlssg-sm86 %command%
```

In the game, pick DLSS Frame Generation with a **fixed** multiplier (2x/3x/4x).

**Longshot:** `tools/longshot-dlssg.sh` starts `~/Games/Longshot` (or `LONGSHOT_DIR`) with DLSS FG forced at 4x
(`RX_VK_DLSS_FG=1`, `RX_VK_FRAMEGEN_MULT`) and logs to `logs/session.txt`. Frame generation *Auto* in the Longshot menu
means FSR 3.1, so keep a fixed multiplier there.

### Options

| Variable | Effect |
|---|---|
| `DLSSG_SM86_PACK=<dir>` | kernel pack location |
| `DLSSG_SM86_MFG=0` | keep DLSS-G at 2x (no multi-frame patches) |
| `DLSSG_SM86_SPOOF_ARCH=0x190` | arch reported to the snippet |
| `DLSSG_SM86_QUIET=1` | no `[dlssg-sm86]` log lines |
| `DLSSG_SM86_TRACE=1` | log `dlopen`/`dlsym` traffic (debugging) |

Lines to look for in the log: `core ... snippet arch gate lifted`, `dlssg: multi-frame generation allowed`, and
`cu module ... swapped for ... -> 0`.

## Limits

- Native Linux Vulkan games that use NGX DLSS-G directly. Proton/Windows games need sdli1995's mod and
  [dlssg-open](https://github.com/dniminenn/dlssg-open) instead.
- Byte patterns are made for driver r620 and runtime 310.9.16. A newer runtime needs a new pack and possibly new patterns.
- Blackwell's hardware flip metering does not exist on older GPUs. The game's own frame pacing has to do that work
  (Longshot does).

## Credits

- [sdli1995/dlssg_for_sm86](https://github.com/sdli1995/dlssg_for_sm86): DLSSG SM86 and its Ampere kernels (MIT). This project only maps them to the Linux runtime.
- NVIDIA DLSS is NVIDIA's. This repository contains no NVIDIA files and no kernel pack.

## License

MIT, see [LICENSE](LICENSE).
