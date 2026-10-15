# OBS Upscaler

A GPU video filter for OBS Studio, built for **OBS 31.1.1 on Windows x64**. Upscale 1920 × 1080 to 3840 × 2160 using Performance, Balanced or Quality presets, while keeping OBS's configured frame rate. Other source sizes and aspect ratios work too.

The same binary uses standard graphics shaders on **NVIDIA, AMD and Intel** GPUs through OBS's Direct3D 11 backend. There is no CUDA, Tensor Core, RTX, DLSS, or vendor SDK requirement. Linux/OpenGL is also built and tested. macOS packaging and Windows ARM64 are not included.

This is **spatial scaling and sharpening**, not AI detail reconstruction. It cannot recover all the detail of a native 4K capture, create missing frames, or guarantee 4K60 on a GPU already at full load. The scaling algorithms are established techniques also available in OBS; this plugin brings them together with sharpening, comparison and diagnostics, rather than claiming a new AI algorithm.

## Install on Windows

1. Download `obs-upscaler-windows-x64` from a successful run under [Actions](https://github.com/Cornwall92/OBS-Upscaler/actions). Extract the downloaded artifact to find `obs-upscaler-0.1.0-windows-x64.zip`.
2. Close OBS. Extract that inner ZIP into `C:\ProgramData\obs-studio\plugins` (create the folder if needed). The resulting DLL must be at `C:\ProgramData\obs-studio\plugins\obs-upscaler\bin\64bit\obs-upscaler.dll` and the shader at `...\obs-upscaler\data\upscale.effect`.
3. Start OBS. Right-click your game/video source → **Filters** → **Effect Filters +** → **OBS Upscaler**.

For portable OBS, copy the DLL into `<OBS>\obs-plugins\64bit` and the contents of the package's `data` folder into `<OBS>\data\obs-plugins\obs-upscaler`. Use one installation location only. To uninstall, close OBS and remove the files installed above.

If the filter is absent, check **Help → Log Files → View Current Log**, search for `obs-upscaler`, and confirm the x64 installation and data paths. OBS must be able to load the plugin before its diagnostics can appear. Windows builds use the normal Microsoft Visual C++ runtime required by OBS.

## Set up 1080p to 4K

1. Keep the capture/video source at **1920 × 1080**. Apply the filter directly to that source.
2. Choose **3840 × 2160**, **Balanced**, sharpening **0.15**, with halo reduction enabled.
3. In **Settings → Video**, set both **Base (Canvas)** and **Output (Scaled) Resolution** to **3840 × 2160**. Choose your normal FPS, such as 60 or 30; the plugin does not change it.
4. Select the source and use **Transform → Fit to Screen** after adding the filter. Check positioning/cropping in existing scenes. A different source aspect ratio is preserved and can leave unused canvas space.
5. Leave **Rescale Output** disabled in the encoder settings so the encoder doesn't shrink the result again. Choose recording/streaming quality appropriate for 4K.
6. Open **View → Stats**, then test while the game and recording/stream are running. Compare camera movement, fine text, foliage, and edges. The split comparison is included in recordings too: turn it off when finished.

The filter runs before scene transforms. Applying it to an already-4K scene does not give it an original 1080p image to reconstruct. Avoid combining several scaling/sharpening filters. It preserves the input's timing, including 59.94 FPS, rather than performing interpolation or frame generation. A 30 FPS capture remains 30 distinct frames even if OBS is configured for 60 FPS.

## Controls

| Control | Behaviour |
| --- | --- |
| Performance | Bilinear reconstruction; lowest sampling cost. |
| Balanced | Catmull–Rom bicubic reconstruction; default. |
| Quality | Lanczos3 reconstruction; greatest sampling cost, not necessarily best for every image. |
| Output size | 4K, 1440p, 2× source, or a custom bounding box. Preserves aspect ratio; never downscales. |
| Sharpening | 0–1; start low for game interface text. Zero disables the sharpening contribution. |
| Reduce edge halos | Limits reconstruction/sharpening overshoot using nearby source colours. |
| Automatic fallback | Steps down one preset after two consecutive approximately 2-second windows with OBS rendering pressure. Keeps output dimensions and configured FPS. |
| Before/after | Left: ordinary bilinear scaling. Right: the chosen effective preset and sharpening. |
| Diagnostics / Refresh | Backend, dimensions, configured FPS, requested/effective preset, OBS-wide rendering statistics and warnings. Click Refresh for a new snapshot. |
| Reset fallback | Retries the requested preset. Changing preset or toggling automatic fallback also resets it. |

Automatic fallback uses OBS's overall render lag (at least 1% in a sample) or average rendering time (at least 90% of the frame interval), with at least 30 frames in a valid sample. It does not identify the cause of lag, and it is not a per-filter GPU timer. It does not respond to network dropped frames or encoding lag. It only steps downward; it never oscillates upward during a stream. At Performance, reduce game GPU load or other OBS effects if rendering still falls behind. Turning fallback off keeps your selected preset fixed.

## Compatibility and warnings

- **SDR only in this version.** HDR/extended-colour sources or an HDR OBS output pipeline bypass the filter without applying an SDR clamp. Use OBS's own scaler for HDR.
- The graphics backend must be Direct3D 11 or OpenGL and must compile the effect successfully. A missing shader, a shader compiler failure or an unsupported backend leaves the source passing through and reports a warning.
- Custom bounds must be 16–8192 pixels on each axis and at most 33,554,432 pixels in area. Oversized input/output requests pass through. These are conservative plugin limits, not a promise about free GPU memory.
- Sources already at or above the selected bounds pass through. Empty sources wait for video.
- Smaller canvas/output dimensions trigger warnings because they can discard the extra resolution. Encoder-specific rescaling cannot be detected by this filter; check it manually.
- The shader keeps premultiplied alpha and suppresses sharpening across transparency transitions.
- Working driver/backend support matters more than the GPU's brand or model. Integrated GPUs can load the shader but may have insufficient 4K60 headroom. Graphics-device removal or allocation failure may also be reported by OBS itself; this plugin cannot guarantee recovery from a lost device.
- Windows x64 is the main delivery target. Builds against newer OBS versions should be checked before deployment; a successful compile does not constitute testing every GPU/driver.

For OSRS, start with Balanced and mild sharpening. Compare Quality during motion; extra sharpening can make text harsh. Avoid claiming a performance result until measured on the machine actually capturing and encoding.

## Build

Windows: Visual Studio 2022 with C++ tools and a Windows 10/11 SDK, 64-bit PowerShell, CMake 3.28 or newer, and internet access for the pinned OBS SDK dependencies:

```powershell
.\scripts\build-windows.ps1
```

The script verifies the OBS 31.1.1 source checksum, builds its SDK, builds this plugin with compiler warnings treated as errors, runs policy and live Direct3D rendering tests, and creates the installable ZIP in `build-windows\packages`. The graphics test prefers Microsoft's software adapter when available; it does not benchmark your physical GPU. The script does not install anything into OBS. Source/dependency caches live in `.deps`. The plugin does not link Qt or an encoder library directly.

Linux development build (Ubuntu 24.04):

```sh
sudo apt-get install cmake ninja-build pkg-config libobs-dev xvfb libgl1-mesa-dri
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DENABLE_STRICT_WARNINGS=ON -DBUILD_OBS_SMOKE_TEST=ON
cmake --build build --parallel
LIBGL_ALWAYS_SOFTWARE=1 xvfb-run -a ctest --test-dir build --output-on-failure
```

You can set `CMAKE_PREFIX_PATH` to a separately installed OBS SDK. Ubuntu's libobs can be older than the Windows target; the Linux job tests the shared API/OpenGL code, while the Windows job compiles against the pinned 31.1.1 SDK.

Without an OBS SDK, build just the policy tests with `-DBUILD_PLUGIN=OFF`. The live graphics tests compile all shader techniques, render and read back sample textures, and exercise the loaded filter on Windows/Direct3D and Linux/OpenGL. Software rendering validates correctness, **not hardware throughput**. No production frames are copied back to the CPU by the plugin.

## Validation before a live stream

Run a short recording at the intended FPS while gaming. Check text, movement, transparency, scene switching, source resizing, filter toggling and a saved scene reload. Watch rendering and encoding lag separately. Test both with and without automatic fallback. NVIDIA/AMD/Intel hardware throughput, Windows driver behaviour and long-session stability need testing on the actual machines.

GPL-2.0-or-later. Author: James C.
