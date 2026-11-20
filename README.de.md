# dlssg-sm86-linux

**NVIDIA DLSS Frame Generation (auch Multi-Frame 3x/4x) auf RTX-30-Karten in nativen Linux-Vulkan-Spielen.**

[English version](README.md)

NVIDIAs DLSS-G-Runtime läuft erst ab RTX 40. Unter Windows löst
[sdli1995s DLSSG SM86 Mod](https://github.com/sdli1995/dlssg_for_sm86) das für D3D12-Spiele. Vulkan-Frame-Generation
unterstützt er dort nicht, und für native Linux-Spiele funktioniert er gar nicht. Dieses Projekt ist eine kleine
`LD_PRELOAD`-Bibliothek, die dasselbe für den **Linux**-NGX-Stack macht. Es läuft NVIDIAs echtes DLSS-G-Modell, kein FSR und
kein Ersatz-Interpolator.

Getestet: **RTX 3090, NVIDIA 620.70.06 (open kernel module), DLSS-G 310.9.16, Longshot v3.0.0** (Ocarina-of-Time-Recomp),
2x und 4x, 4K-Ausgabe, ~40 fps Basis → 120 Hz.

## Wie es funktioniert

Alles passiert im Speicher, nachdem die Bibliotheken geladen und ihre Signaturen geprüft sind. Keine Datei auf der Platte wird
verändert.

| Wo | Sperre | Patch |
|---|---|---|
| `libnvidia-ngx.so` (NGX-Core des Treibers) | `NGXValidateSnippetMetaData`: *snippet is using newer GPU arch 190 > 170* | Ablehnung übersprungen |
| | Mindest-Architektur pro Feature beim Abfragen der Fähigkeiten (`FrameGeneration.Available`) und in `NVSDK_NGX_CreateFeature_Validate` (*feature is not supported on this device*) | Vergleich übersprungen |
| `libnvidia-ngx-dlssg.so` (DLSS-G-Snippet) | `GetFeatureRequirements` Mindest-Arch 0x190 | 0x160 |
| | `SetGPUArch` | meldet Ada (0x190), damit das Snippet seine sm_89-Kernels nimmt |
| | `DLSSG.MultiFrameCountMax` (pro Gerät und über alle Geräte), `mfgSupported` | Blackwell-Zweige für ältere Karten geöffnet |
| `vkCreateCuModuleNVX` | die sm_89-Kernels des Snippets laden auf Ampere nicht | per SHA-256 gegen sm_86-Builds aus einem Kernel-Paket getauscht |

Das Kernel-Paket stammt aus sdli1995s Release (MIT). `tools/make_pack.py` ordnet seine sm_86-Cubins den Blobs des
Linux-Snippets zu: 39 Kernels sind byte-gleich mit der Windows-Runtime, der Rest passt über den PTX-Entry-Namen. 69 von
71 Kernels sind abgedeckt. Die zwei fehlenden sind Capture-/Debug-Kernels, die Spiele nicht nutzen.

Alle Patches sind Byte-Muster. Ändert ein Treiber oder eine Runtime sie, steht das im Log und es wird nichts gepatcht,
abstürzen kann es dadurch nicht.

## Installation

Voraussetzungen: NVIDIA RTX 30 (RTX 20 ungetestet: Paket mit `--sm 75` bauen), proprietärer oder offener NVIDIA-Treiber
mit NGX (`/usr/lib/libnvidia-ngx.so`), `gcc`, Vulkan-Header, `python-pefile`. Das Spiel muss
`libnvidia-ngx-dlssg.so.310.9.16` mitbringen (oder bekommen).

```sh
git clone https://github.com/ProfessorTee/dlssg-sm86-linux && cd dlssg-sm86-linux
./build.sh
# Kernel-Paket: aus sdli1995s DLSSG-SM86-Release (0.4.2), Datei dlssg_sm86/kernels/fg_kernels.dll
python3 tools/make_pack.py /pfad/zu/dlssg_sm86/kernels/fg_kernels.dll \
    /pfad/zum/spiel/ngx/libnvidia-ngx-dlssg.so.310.9.16 pack/310.9.16-sm86
```

Das Paket wird aus `pack/` neben der Bibliothek gelesen (oder aus `DLSSG_SM86_PACK=<ordner>`).

## Benutzung

```sh
./dlssg-sm86 /pfad/zum/spiel        # setzt LD_PRELOAD und startet das Spiel
# Steam-Startoptionen:  /pfad/zu/dlssg-sm86-linux/dlssg-sm86 %command%
```

Im Spiel DLSS Frame Generation mit **festem** Faktor (2x/3x/4x) wählen.

**Longshot:** `tools/longshot-dlssg.sh` startet `~/Games/Longshot` (oder `LONGSHOT_DIR`) mit erzwungenem DLSS-FG 4x
(`RX_VK_DLSS_FG=1`, `RX_VK_FRAMEGEN_MULT`) und loggt nach `logs/session.txt`. Frame-Generation *Auto* im Longshot-Menü
bedeutet FSR 3.1, dort also einen festen Faktor lassen.

### Optionen

| Variable | Wirkung |
|---|---|
| `DLSSG_SM86_PACK=<ordner>` | Ort des Kernel-Pakets |
| `DLSSG_SM86_MFG=0` | DLSS-G bleibt bei 2x (keine Multi-Frame-Patches) |
| `DLSSG_SM86_SPOOF_ARCH=0x190` | Architektur, die das Snippet sieht |
| `DLSSG_SM86_QUIET=1` | keine `[dlssg-sm86]`-Logzeilen |
| `DLSSG_SM86_TRACE=1` | `dlopen`/`dlsym` loggen (Fehlersuche) |

Im Log auf diese Zeilen achten: `core ... snippet arch gate lifted`, `dlssg: multi-frame generation allowed` und
`cu module ... swapped for ... -> 0`.

## Grenzen

- Native Linux-Vulkan-Spiele, die NGX DLSS-G direkt nutzen. Für Proton/Windows-Spiele gibt es sdli1995s Mod plus
  [dlssg-open](https://github.com/dniminenn/dlssg-open).
- Die Byte-Muster passen zu Treiber r620 und Runtime 310.9.16. Eine neuere Runtime braucht ein neues Paket und eventuell
  neue Muster.
- Blackwells Hardware-Flip-Metering gibt es auf älteren Karten nicht. Das Frame-Pacing muss dann das Spiel selbst
  übernehmen (Longshot macht das).

## Danksagung

- [sdli1995/dlssg_for_sm86](https://github.com/sdli1995/dlssg_for_sm86): DLSSG SM86 und seine Ampere-Kernels (MIT). Dieses
  Projekt ordnet sie nur der Linux-Runtime zu.
- NVIDIA DLSS gehört NVIDIA. Dieses Repository enthält keine NVIDIA-Dateien und kein Kernel-Paket.

## Lizenz

MIT, siehe [LICENSE](LICENSE).
