# x360: Xbox 360 build of Sonic 3 A.I.R.

Everything specific to the Xbox 360 port lives here, in the `x360/` folder at the root of the sonic3air repository (`E:\_S3AIR_X360\sonic3air-main\x360`). The port's changes to the upstream code are in the same repository, so one commit covers both.

## Environment (dev machine)

| Component | Location |
|---|---|
| Xbox 360 SDK 2.0.21256 | `E:\Microsoft Xbox 360 SDK` (env `XEDK`) |
| RXDK-360 (release `latest`, commit `be16a94`) | `E:\RXDK-360` (env `RXDK360`) |
| Visual Studio 2022 Professional | `E:\Microsoft Visual Studio\2022\Professional` |
| Xenia Canary | `E:\_S3AIR_X360\xenia_canary_windows` (`..\..\xenia_canary_windows` from here) |

Toolchain: `Xbox 360` platform with the **`clang`** PlatformToolset from RXDK-360. That is a patched Clang 24 targeting `powerpc64-unknown-xbox360` (ILP32 on ppc64, ELF64), with libc++ and picolibc, linked by ld.lld and packed by XexTool.

## One-time step: fix RXDK-360 (required)

The RXDK-360 installer converts the XDK libraries to **ELF32**. The compiler, `libc.a` and `libcpp.a` in the same release are **ELF64**, and `libc.a` is missing the MSVC helpers (`__savegprlr_*`, `__restfpr_*`, `_RtlCheckStack12`, `__u64tod`, …). Without the fix, nothing links (`ld.lld: ... is incompatible with ...`).

```bash
python -I E:\_S3AIR_X360\sonic3air-main\x360\tools\fix_rxdk_ppc64.py --rxdk E:\RXDK-360
```

What the script does:
- Backs up the originals once to `E:\RXDK-360\lib\xbox\_elf32_backup`.
- Re-converts the XDK `.lib` files to ELF64 with the upstream `coff2elf.py --ppc64` (vendored in `tools/rxdk_upstream`).
- Reassembles `kernel_import.a` for ppc64.
- Adds the 6 MSVC helpers from `libcMT` into `libc.a`.
- Swaps libc.a's `memcpy`/`memmove`/`mempcpy` (member `xbox_memcopy_irql.o`) for `runtime/memcopy_safe.c`, which never uses VMX on physical/GPU memory. Without this, D3D hangs the GPU on the console.
- Swaps `xbox_fileio.o`/`xbox_dirio.o` for `runtime/rxdk_patched/{fileio,dirio}.c`: paths go to the kernel as `game:\x` with `RootDirectory = (HANDLE)-3`, like the XDK's `CreateFile`, instead of `\??\game:\x`. With this, **`fopen`, `stat`, `opendir`, `std::ifstream` and `std::filesystem` work in Xenia too** (before, they only worked on hardware).

It's idempotent. **Run it again if you reinstall or update RXDK-360.**

## Build

```bash
"E:\Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\MSBuild.exe" x360\tests\cpp17hello\cpp17hello.vcxproj /p:Configuration=Release "/p:Platform=Xbox 360"
```

Or open the `.vcxproj` in VS2022 (Release | Xbox 360). F5/Ctrl+F5 launches Xenia (`DeploymentType=Xenia`).

Required settings in each `.vcxproj` (see `tests/cpp17hello`):
- `<PlatformToolset>clang</PlatformToolset>`
- `<RxdkClangXdkHeaders>true</RxdkClangXdkHeaders>` and `<RxdkClangCppStd>c++17</RxdkClangCppStd>`: XDK headers (D3D9, XInput, …) in C++17.
- XDK libs under Linker → Additional Dependencies (`xapilib.lib;d3d9.lib;xboxkrnl.lib;…`). Each one maps to the converted `.a`.
- `PostBuildEvent` does **not** run on this platform. Use a `<Target AfterTargets="Build">` to copy files.

## S3AIR libraries (Phase 2)

The repository is under **local git**: the commit `97caa6f` is the original upstream snapshot, so `git diff 97caa6f -- . ":!x360"` shows every change the port made to the upstream code. The `x360/` folder was a separate repository until 19/10/2026 (its single commit `9636c7a` is kept outside the tree in `E:\_S3AIR_X360\_x360_git_backup`).

Build commands below are run from the repository root (`E:\_S3AIR_X360\sonic3air-main`).

**Build order** (Release | Xbox 360). The static libraries go to `build\out\<Config>\`:

```bash
"E:\Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\MSBuild.exe" x360\build\zlib.vcxproj /p:Configuration=Release "/p:Platform=Xbox 360"
```

Repeat with `rmxbase`, `rmxmedia`, `lemonscript`, `sdl2lite` and `x360runtime`. Then build `tests\libtest\libtest.vcxproj`.

| Path | Contents |
|---|---|
| `build/x360.toolset.props` | clang toolset, XDK headers, C++17, DWARF. Import **before** `Microsoft.Cpp.props` |
| `build/x360.common.props` | Paths (`$(S3AIRRoot)`, `$(X360Root)`), includes, defines, OutDir/IntDir, force-include of `x360_compat.h`. Import **after** `Microsoft.Cpp.props` |
| `build/*.vcxproj` | `zlib` (pure clang mode, C without XDK), `rmxbase`, `rmxmedia`, `lemonscript` (sources by wildcard from upstream), `sdl2lite`, `x360runtime` |
| `include/x360_compat.h` | MS CRT shims that XDK mode expects but picolibc lacks (`vsnprintf_s`, `_vsnwprintf_s`, `_ftelli64`/`_fseeki64` for libc++, `std::strftime`). Keeps toolchain workarounds out of the upstream code |
| `sdl2lite/` | SDL2 for the 360. `include/SDL.h` uses **SDL2's real headers** (`framework/external/sdl/SDL2/include`, with `_WIN32` hidden), and `src/` implements on top of the XDK only what S3AIR uses. Missing functions show up as link errors |
| `runtime/x360_runtime_support.cpp` | `expf`/`logf`/`powf`/`log2f` and `nothrow` `new`/`delete`, which the RXDK runtime doesn't provide (`x360runtime.lib`, link in every app) |
| `tests/libtest` | Runtime test. Checks rmxbase/zlib/JSON/files/SDL threads, then runs upstream's lemonscript test (`main` renamed). 30 s watchdog → back to the dashboard |

**Upstream changes (Phase 2):**
- `PLATFORM_X360`/`PLATFORM_BIG_ENDIAN`/`USE_UTF8_PATHS` in `PlatformDefinitions.h`, detected **before** Windows (the toolchain also defines `_WIN32`).
- `export.h` has no `dllexport`.
- `rmxmedia.h`/`rmxmedia_externals.h` have no OpenGL (`RMX_WITH_OPENGL_SUPPORT` off).
- `FileIO.cpp` takes the POSIX path (`std::filesystem` + `dirent`).
- `FunctionWrapper.h` no longer casts a temporary to a non-const reference, which only MSVC accepted.

**SDL2-lite, state:**

| Area | State |
|---|---|
| Init/errors, timers (`SDL_GetTicks`, `SDL_Delay`, perf counter) | ✅ Real |
| Threads (`CreateThread`, 1 MB stack), mutex (`CRITICAL_SECTION`), condition variables (SDL2's generic algorithm with semaphores) | ✅ Real |
| RWops over `fopen` | ✅ Real |
| Window/cursor/surface | Minimal (no windows on the 360; the image goes through D3D9 in Phase 3) |
| Events (`SDL_PollEvent`) | No events (input will come straight from XInput, Phase 5) |
| Audio | Fails cleanly with an error until the XAudio2 backend (Phase 5) |

**⚠️ Headers and incremental builds:** RXDK's `ClangCompile` has **no header dependency tracking**. It only recompiles a `.cpp` that is newer than its `.o`. `x360.common.props` works around this with the `X360InvalidateObjectsOnHeaderChange` target: if any header in the code trees is newer than the project's last compile, it deletes all the objects. Changes to a `.h` therefore cause a full recompile of the project (lemonscript takes ~5 min). If in doubt, use `/t:Rebuild`.

**Big-endian fixes (Phase 4, started 17/10):**

| Spot | Problem | Fix (only under `PLATFORM_BIG_ENDIAN`) |
|---|---|---|
| `lemon/runtime/RuntimeOpcode.h` | Opcode parameters written as `int64`/pointer and read as `int16`/`uint32`/`uint64` | Parameter memory in **little-endian layout** (byte swap on every access, pointers zero-extended to 8 bytes) |
| `Default/OptimizedOpcodeProvider.cpp` | Narrow access (`u8`/`u16`/`u32`) to the 8-byte slot of a global variable | `getValuePointerInSlot()` points to the last bytes of the slot |
| `lemon/utility/AnyBaseValue.h` | A `float` in a union with `uint64` ends up in the high bits on BE | The float's bits go into the low 32 bits (like on LE) |
| `rmxbase/tools/Tools.cpp` `getMurmur2_64` | Reads 64-bit blocks in host order, so the hash differs from the PC's | Reads them as LE, giving **the same hash as the PC** (function/string names in `scripts.bin`) |
| `lemon/program/function/Function.cpp` | Signature hashes = FNV over the bytes of a `uint32[]` | FNV over the LE representation, matching the PC |
| `rmxbase/memory/VectorBinarySerializer.cpp` | Primitives serialised in host order (`scripts.bin`, etc.) | Format **always LE**, so PC files are readable on the 360 and vice versa |
| `lemon/runtime/ControlFlow.h` | Each scalar local variable has an 8-byte slot, written as `int64`; the nativized code reads it as `u8`/`u16` | `readLocalVariable`/`writeLocalVariable` point to the last bytes of the slot (local arrays stay packed, via `accessLocalVariable`) |
| `lemon/translator/Nativizer.cpp`, `NativizedOpcodeProvider.cpp` | Lookup hashes over the `uint32` bytes, tables generated as LE byte arrays, global variable slots | Hash over the LE bytes, tables read as LE, `getValuePointerInSlot()`, so **the nativized code generated on the PC is used on the 360** |

The `.bin` files (`scripts.bin`, `gamedata.bin`, …) are **generated on the PC from the same source** and shipped with the XEX. That's why hashes and serialisation must be identical to the PC's.

**State of lemonscript on big-endian (17/10):** ✅ in Xenia the interpreter runs upstream's test (`mainscript.lemon`): arrays, bools, constants, floats, if/else, object handles, strings, and **all the script unit tests pass** (`Running tests...` → `Done with all tests`, no failures). **Confirmed on the RGH (v9, 17/10): output identical to Xenia's, byte for byte.** Still to do: load a `scripts.bin` generated on the PC (serialisation and hashes are already PC-compatible, but untested).

## The game: oxygen + sonic3air (Phase 3)

**Projects** (generated by `tools/gen_projects.py`; edit the script, not the `.vcxproj` files):

| Project | Contents |
|---|---|
| `oggvorbis`, `minizip` | Pure C (no XDK). minizip leaves out `zip.c`: it isn't used, and RXDK's clang refuses one of its patterns ("OR8_rec ... sets CR0 from the low 32 bits") |
| `rmxext_oggvorbis` | rmx's ogg extension |
| `oxygen_netcore` | `Sockets.cpp` is replaced by `port/Sockets_x360.cpp`. **No network on the 360** (update check, ghosts, netplay); every socket fails cleanly |
| `oxygen` | The engine (182 files) |
| `sonic3air` | **Application**. Output in `x360/deploy/<Config>/default.xex`. Includes `port/x360_platform.cpp` |

**Build order:** `zlib`, `rmxbase`, `rmxmedia`, `lemonscript`, `sdl2lite`, `x360runtime`, `oggvorbis`, `minizip`, `rmxext_oggvorbis`, `oxygen_netcore`, `oxygen`, then `sonic3air`.

**`port/x360_platform.cpp`** uses a static constructor, so upstream's `main()` stays as it is:
- `std::cout`/`std::cerr` go to `game:\s3air.log` (written to disk immediately) and to `DbgPrint`;
- an `atexit` handler calls `XLaunchNewImage`, because returning from `main` would reboot the console.

The engine also writes its own log to `game:\savedata\logfile.txt`.

**Deploy folder** (`x360/deploy/Release`):
- `default.xex`
- `data/*.bin` and `config.json`
- `oxygenproject.json` (game profile: main script name, ROM)
- `scripts/` (sources, used as a fallback when `scripts.bin` doesn't fit)
- `Sonic_Knuckles_wSonic3.bin`

The engine copies the ROM to `savedata/`.

**SDL2-lite, Phase 3 additions:**
- **D3D9 presenter**: `SDL_GetWindowSurface` provides an ABGR8888 buffer; `SDL_UpdateWindowSurface` copies it into a linear `D3DFMT_LIN_A8B8G8R8` texture (two textures, `BlockUntilNotBusy`) and draws a quad in 16:9 with the shaders in `sdl2lite/shaders` (compiled offline with the XDK's `fxc`: `build_shaders.cmd`).
- **XInput**: joysticks and game controllers with a fixed layout, connection events, timed rumble.
- OpenGL fails, so the engine falls back to the software renderer.
- `SDL_ShowMessageBox` writes the text to `DbgPrint` and chooses the **Escape** button, avoiding "Retry?" loops.

**Upstream changes in Phase 3:**
- `ImGuiIntegration.cpp`: a missing `const` in the stub used when ImGui is off. An upstream bug on any platform without ImGui.
- `FileIO.cpp`: on the 360, **no `std::filesystem`**; POSIX functions instead (`stat`, `mkdir`, `rename`, `unlink`, `getcwd`/`chdir`). In XDK mode, `_WIN32` makes the libc++ headers use the Windows variant (paths in `wchar_t`), which doesn't match the RXDK `libcpp.a` build (`char` paths), so the paths came out empty.
- `BitmapCodecPNG.cpp`, `BitmapCodecBMP.cpp`, `PaletteBitmap.cpp`: LE (BMP) and BE (PNG) headers read correctly on BE; RGBA pixels converted to the engine's `0xAABBGGRR` value.

**State in Xenia at the end of Phase 3 (17/10):**
- ✅ The engine starts fully: configuration, OPCK packages, software renderer, input, mods, sprites/fonts, and **the scripts compile from source on the 360**.
- ✅ The main loop runs, and the D3D9 presenter keeps presenting frames (`SDL2-lite: presented frame N` in `xenia.log`).
- ❌ Black screen. The scripts get stuck (`Reached limit for runtime steps per update`). The cause is the **emulated Mega Drive memory**: `EmulatorInterface` keeps RAM in big-endian order and converts every access with `swapBytes*`/`readMemoryUnalignedSwapped`. On the 360 those conversions must be no-ops.
- **Next block (Phase 4):** audit ~20 swap sites and ~40 raw `uint16*`/`uint32*` reads in `EmulatorInterface`, `SoundDriver`, `PlaneManager`, `SpriteManager`, `ScrollOffsetsManager`, `RenderUtils`, `FilePackage`, `LemonScriptBindings`, `RendererBindings`, `BlueSpheresRendering`, `Kosinski`, plus the lemonscript `*_DIRECT_SWAP` opcodes.
- Audio stays off until Phase 5. `audiodata.bin` loads, but there's no output.

## Big-endian of the emulation (Phase 4)

**Principle:** the emulated Mega Drive memory (RAM, ROM, shared memory) is stored in **big-endian**, the original byte order. On the PC every access swaps bytes; on the 360 that is the native order, so the conversions become no-ops. VRAM stays in **native** 16-bit words, as on the PC. librmx gets two helpers for this:
- `convertBigEndian16/32/64()` and `rmx::convertBigEndian<T>()` (in `Basics.h`);
- `rmx::readMemoryUnalignedBigEndian<T>()` (in `UnalignedMemory.h`).

On the PC they are identical to the `swapBytes*` they replace.

| Spot | Problem on BE | Fix |
|---|---|---|
| `EmulatorInterface.cpp` | `readMemory*`/`writeMemory*` and `copyFromMemoryToVRam` with a swap | `convertBigEndian*` / `readMemoryUnalignedBigEndian` |
| `OptimizedOpcodeProvider.cpp` `*_DIRECT_SWAP` | Direct memory access with `rmx::swapBytes` | `rmx::convertBigEndian`. `mSwapBytes` now means "memory stored in BE" |
| `sonic3air/_nativized/NativizedCode.cpp` | The generated code calls `swapBytes16/32/64` on emulated memory | On BE, `lemon::swapBytes*` (identity) hide the global functions |
| `LemonScriptBindings.cpp` `accessRegister` | `D0.u8`/`D0.u16` point to the start of the `uint32` register, which is its high byte on BE | Each size points to the low bytes of the register |
| `LemonScriptBindings.cpp` | `fillMemory_u16/u32` and `System_loadExternalPaletteData` swap by hand | `convertBigEndian*` |
| `RenderUtils.cpp` | `expandPatternDataFromVRAM/ROM` take the pixels (nibbles) out in LE order | On BE both use the natural order (from the high nibble down) |
| `ScriptImplementations.cpp` `decompressKosinskiData` | Copies into VRAM swapping bytes | `memcpy` on BE |
| `BlueSpheresRendering.cpp` | Writes values to RAM with a swap | `convertBigEndian16` |
| `drawing/software/*` (Blitter, BlitterHelper, SoftwareRasterizer, SoftwareDrawer), `Bitmap.cpp` | Colours accessed byte by byte with `[0]`=R … `[3]`=A, which is only true on LE | Byte offsets in `colorbyte::R/G/B/A` (`Bitmap.h`): `A`=0 … `R`=3 on BE |

Not touched (they are fine on BE): `Kosinski.cpp` (a logical transform of the value), plane/sprite/scroll managers (they read VRAM in native words), `PersistentData`/`copyMemory` (byte by byte, so the save files are identical to the PC's).

**Pending:**
- `SoundDriver.cpp` (Z80 sound driver, Phase 5): the Z80 is little-endian and the code uses `uint16&` over `mRegisters`/`mRam` and structs with `uint16`. It needs a BE layout before audio.
- `BitmapCodecJPG.cpp`, and saving BMP/PNG on BE (not used by the normal game).
- Save states (`SaveStateSerializer`) serialise VRAM as native words, so they aren't interchangeable between the PC and the 360.

**Diagnostic tools added in Phase 4:**
- **Frame dump:** if `game:\dump_frames.txt` exists, SDL2-lite writes `game:\frame_NNNNN.bmp` every 300 frames (at most 40, i.e. the first ~3 minutes at 60 fps). This works in Xenia (where window screenshots fail) and on the console. Remove the file for normal use, because every frame written costs time.
- **Call stack when a script gets stuck:** in `CodeExec.cpp` (`PLATFORM_X360` only), "Reached limit for runtime steps" writes to `savedata/logfile.txt` the function and line of each level of the script call stack.

**Hardware v10 (18/10):** ✅ same image as in Xenia (title screen, Angel Island demo), but **~25 FPS**, falling to single digits with the performance graph on. Every frame dump froze the console for a few seconds (3.2 MB written to USB). `SoundDriver` also logs "Invalid address" (the Z80's LE layout, Phase 5).

**Performance, first round (v11):**
- **Window at 400x224** (`"WindowSize": "400 x 224"` in the port's `config.json`). Before, the window was 1200x672 (the PC default): the software drawer upscaled the frame 3x on the CPU, and SDL2-lite copied 3.2 MB per frame into the texture. Now the drawer works at native resolution and the GPU scales.
- **"Sharp bilinear" shader** in `sdl2lite/shaders/present.hlsl` (constant `c0` = texture size and scale): sharp pixels without the uneven sizes that point sampling gives at ×3.2.
- **Profiling in the log:** every 300 frames, `savedata/logfile.txt` gets `Profiling (avg ms per frame): Simulation=… Audio=… Rendering=… Frame Sync=…` (from `Application.cpp`, `PLATFORM_X360` only). `s3air.log` gets `avg … ms per present`, the time spent in `SDL_UpdateWindowSurface`.

**State in Xenia (17/10):** ✅ disclaimer, Sonic 3 title screen and rolling demo of Angel Island Zone (Tornado intro, then gameplay with Sonic and Tails, HUD, rings, title card), all with correct colours. The speed in Xenia doesn't say anything about the hardware's.

## D3D9 drawer (Phase 6, started 18/10)

**Why:** the hardware v11 test showed the slow menus spending 53–79 ms per frame on CPU drawing (software drawer). On the Xenon, every float→int conversion per pixel costs a load-hit-store stall. On top of that, the 400x224 window made the disclaimer unreadable.

**What it does:** `port/D3D9Drawer.cpp` implements Oxygen's `DrawerInterface` on the GPU, modelled on `OpenGLDrawer`.
- The game screen still comes from the **software renderer** (it's cheap: ~2–5 ms) and is uploaded as a texture every frame.
- **Everything drawn through the drawer is done by the GPU at 1280x720:** the upscaled game screen, menus, sprites, text and overlays (`"WindowSize": "1280 x 720"` in `config.json`).

| Part | Implementation |
|---|---|
| Device | The one SDL2-lite creates (`SDL_X360_GetD3DDevice()` in `sdl2lite/include/SDL_x360.h`); the drawer does its own `Present` |
| Engine textures | `D3DFMT_LIN_A8B8G8R8` (the engine's `0xAABBGGRR` layout, no conversion). Textures rewritten every frame are double-buffered, and any change is preceded by `BlockUntilNotBusy` (the 360 doesn't synchronise CPU/GPU) |
| Palette sprites | `D3DFMT_LIN_L8` + a 256x1 palette texture, looked up in the pixel shader |
| Text | One glyph atlas (512x512 pages) per font, filled on demand with the glyphs including effects (`Font::applyEffects`) |
| Render targets (GameView, OxygenMenu) | EDRAM surfaces **placed after the back buffer** (`D3DSURFACE_PARAMETERS::Base`), so switching targets doesn't destroy the others' content; `Resolve` into a texture on switch and before `Present` |
| Upscaling | "Sharp bilinear" shader (`UPSCALED_RECT`) |
| Shaders | `port/shaders/drawer.hlsl` → `drawer_*.h` via `port/shaders/build_shaders.cmd` (XDK `fxc`) |
| States | `D3DRS_HALFPIXELOFFSET` (pixel centres as in OpenGL), blend modes as in `OpenGLDrawerResources`, scissor in top-left pixel coordinates (like the software drawer) |

**Engine changes:**
- `Drawer::Type::D3D9`.
- `EngineMain::createWindow` calls `createX360Drawer()` on the 360, and falls back to the software drawer if it fails.
- The render method stays `SOFTWARE` (that's what selects the software renderer for the game screen).

**Diagnostics:**
- `s3air.log` gets `D3D9Drawer: presented frame N, avg X ms per Present` every 600 frames (time waiting for the GPU or vsync).
- SDL2-lite's frame dump (`dump_frames.txt`) only works with the software drawer. With the D3D9 drawer, the capture via `Resolve` came out empty in Xenia, so it was removed.
- In Xenia, use `tools/xenia_run.ps1 -Screenshot`, which now uses `PrintWindow` (`PW_RENDERFULLCONTENT`) and works even when `CopyFromScreen` fails. On the console, use Aurora's screenshots.

**State (18/10):** ✅ in Xenia, disclaimer at 720p with readable text, title screen and demo with sharp upscaling. ⏳ Menus and hardware performance still to test (package `hardware_test_v12`).

## Performance: parallel rendering and 30 FPS mode (Phase 6, 18/10)

**Diagnosis (hardware v12):** in-game, simulation 3–12 ms plus rendering 5–21 ms per frame. The rendering is almost all the software renderer drawing the 400x224 game screen on a single core. The Xenon has 3 cores / 6 hardware threads.

**Software renderer in bands** (`SoftwareRenderer.cpp/.h`):
- The game screen is split into horizontal bands (one per thread), and each band renders **all** geometries clipped to its own lines.
- Each band has its own context: viewport, depth-buffer flag, plane caches (`BufferedPlaneData`), `Blitter` (temp buffers) and window-plane scroll. The bands write disjoint lines of the image and of the depth buffer.
- Blurs (`EFFECT_BLUR`) need the whole screen. Rendering is therefore done in phases, with each blur serial between them.
- Sprite masking copies and restores only each band's lines.
- `PaletteSprite::getUpscaledBitmap()` (built lazily) is warmed on the main thread before going parallel.
- Only the first band (calling thread) checks the render time limit; the others stop through a flag.
- On the PC there's 1 band, so behaviour is the same as before.

**`SDL_X360_ParallelFor`** (`sdl2lite/src/SDL_x360_parallel.cpp`, declared in `SDL_x360.h`):
- Native workers (`CreateThread` + events) pinned with `XSetThreadProcessor` to hardware threads **2 and 4** (the other cores). The main thread runs on 0 and executes index 0.
- 3 bands in total (`SDL_X360_GetParallelThreadCount()`).

**30 FPS mode:**
- The "Frame Sync" option is called "Frame Rate" on the 360, with "60 FPS" (V-Sync On) and "30 FPS" (the "V-Sync + FPS Cap" value).
- At 30 FPS, `Application.cpp` doubles the frame-cap interval (the simulation stays at 60 Hz, with 2 steps per frame), and the D3D9 drawer uses `D3DRS_PRESENTINTERVAL = TWO`.
- Other saved values (e.g. "V-Sync Off") are normalised to 60 FPS.
- Tested in Xenia: ~33 ms per frame, simulation at 60/s.

**Hardware v14 (18/10):**
- In-game rendering dropped from 5–21 ms to 2.5–12 ms. But the game stayed at ~40–50 FPS, with ~9–14 ms per frame in "Frame Sync", and 30 FPS had no effect.
- Causes and fixes:
  - **The frame cap stacked on top of vsync.** On the 360, `preciseDelay` sleeps in 1 ms steps (`sleep_for`/`yield`), and then Present still waits for vsync. Now, on the 360, `Application.cpp` doesn't use the frame cap or the minimum delay: the D3D9 drawer's present interval (ONE = 60 FPS, TWO = 30 FPS) alone sets the frame rate.
  - **The Frame Sync option didn't work on big-endian.** `CONFIG_ENUM_8` wrote 1 byte into an `int`-sized enum (`"FrameSync": 16777217` = 0x01000001). It's now registered with `setupOptionEntryInt`; this was a latent upstream bug that only showed on BE. Old saved values are normalised by the D3D9 drawer.
  - **Slow boot (~38 s).** The ROM load took 22 s on the console (1 s in Xenia): `FileIO::readFile` (`std::ifstream`) and `FileHandle` (`fread`) go through the runtime's stdio, which reads in small blocks. On the 360 they now use `open`/`read`/`lseek`/`fstat` directly, which call `NtReadFile` with the full size; `FileHandle` uses a file descriptor for read-only files.
  - **Scripts compiled on every boot (~12 s).** The 26.03 `data/scripts.bin` doesn't match the 26.08.15 source. The game then compiles from source and writes `saves/scripts.bin`, but on the next boot it tries `data/scripts.bin` again. On top of that, the module signature was handled as a native `uint32` (`*(uint32*)"LMD|"`), so a `scripts.bin` written on the 360 came out with the signature reversed and couldn't be read back. `ModuleSerializer.cpp` (and the runtime's save state, "LMN|", in `Runtime.cpp`) now write and compare the signature as bytes: same file on the PC and the 360.
- **How `data/scripts.bin` is generated now:** run the XEX once with the `scripts/` folder and without `data/scripts.bin` (in Xenia), then copy `saves/scripts.bin` into `data/`. The result is deterministic: byte for byte the same in Xenia and on the console. With a valid `data/scripts.bin`, the `scripts/` folder isn't needed (package v15 doesn't include it).
- **Every time the scripts change** (e.g. `actselect.lemon`), `data/scripts.bin` has to be regenerated. Otherwise the game falls back to the source (if `scripts/` exists) or fails to start.

**Hardware v15 (18/10):**
- 60 FPS reached, but averaging ~50 with a periodic stutter. The scripts now load in 0 s, but "Loading ROM" still took 20 s.
- Changes (package v16):
  - **Async logging:** every log line did WriteFile + FlushFileBuffers (several times per line) on the main thread, which caused hitches of tens of ms on USB. That included my profiling line every 5 s, which matches the stutter.
    - `std::cout` → `game:\s3air.log` is now written by a thread on hardware thread 5.
    - On the 360 the engine uses only `StdCoutLogger` with timestamps; the `FileLogger` / `savedata/logfile.txt` is not used.
    - The periodic diagnostics (profiling, Present time) are only written with "Full Profiling".
  - **ROM:** it's no longer copied into `savedata/` on the 360 (writing 4 MB through the runtime's stdio was the 20 s). It's read from where it is. The log shows `ROM loading took N ms`.
  - **`FileIO::saveFile`** writes in a single `write` (settings, saves, persistent data without hitches).
  - **Adaptive vsync** at 60 FPS: `D3DRS_PRESENTIMMEDIATETHRESHOLD = 20`. A late frame is presented immediately if scanout is still in the first 20% of the screen, instead of waiting a whole frame; a tear may show near the top. Off at 30 FPS.

**Not done yet (pending measurement):** pipelining the simulation with the rendering (frame N rendered while N+1 is simulated). It needs a copy of all render state (VRAM, pattern cache, palettes, sprite lists, scroll), so it only makes sense if the bands aren't enough.

## Audio (Phase 5, started 18/10)

**XAudio2 backend** (`sdl2lite/src/SDL_x360_audio.cpp`): SDL's callback model on top of an XAudio2 source voice.
- A *feeder* thread (hardware thread 3) keeps 3 buffers queued, filling each one with the application's callback. rmxmedia's `AudioManager` does all the mixing there.
- The XAudio2 engine runs on hardware thread 5.
- The format is 16-bit PCM in native (big-endian) order, which is what XAudio2 on the 360 takes. The obtained spec reports `AUDIO_S16MSB`; the engine itself writes native `short`.
- `SDL_LockAudioDevice` uses the same lock as the callback invocation.
- **Debug:** with `game:\dump_audio.txt` present, the first 20 s of output also go to `game:\audio_dump.wav`.

**SDL2-lite threads:** on the 360 a thread starts on its creator's hardware thread. `SDL_CreateThread` therefore spreads threads over hardware threads 5, 3 and 1. rmxmedia's audio *job worker* (sound chip emulation, OGG decoding; `mUseAudioThreading` is on) lands on 5, away from the main thread (0) and the rendering workers (2, 4).

**Z80 sound driver on big-endian** (`SoundDriver.cpp`):
- The register pairs (`af`, `bc`, `de`, `hl`) are `uint16&` over `f/a`, `c/b`… On BE the byte order within each pair is swapped (high byte first).
- The 16-bit aliases into the Z80 RAM (little-endian: `zVoiceTblPtr`, `zSongPosition`, `zTrackInitPos`, …) use `Z80Word`, which is endian-neutral and behaves the same on the PC.
- The driver's "Invalid address" errors disappeared.
- OGG decoding (float PCM → `short` by hand) and the chip-write files (`VectorBinarySerializer`) were already endian-independent.

**Verified in Xenia (18/10)**, by analysing `audio_dump.wav`:
- Emulated soundtrack and remastered soundtrack (`audioremaster.bin`, the latest version from the server) both give music: ~4,000–5,000 zero crossings/s, RMS ~3,000, peaks ~15,000.
- Byte-swapped PCM would look like noise at ~24,000/s.

## Mods (18/10)

- `savedata/mods/` with `active-mods.json` works.
- In Xenia, the 43 mods are found and the active mods' scripts compile (the script step goes from 0.3 s to 3 s). Sprite mods apply (e.g. the "Sonic 3 Complete Sprites" rocks in AIZ).
- "menu sprites" and "menu sprites - template" have invalid `mod.json` (line breaks inside strings), so they would fail on the PC as well. That's a problem with the mods, not the port.

## Lost music and saves during play (19/10)

**Symptom (hardware, music mods active):** after a few acts the BGM stopped (also in the following acts), and after restarting, the progress was lost.

**Cause:** the RXDK runtime's file descriptor table had only 32 entries (29 usable, shared with directory handles), and was not thread-safe.
- `OggAudioSource` opens its file in `load()` and keeps it open until the object is destroyed, and audio sources are never removed.
- OGGs inside `audioremaster.bin` share the package's file, but the music mods are loose `.ogg` files (e.g. 61 + 16 + 17 in the active mods), and each one that ever played kept a descriptor.
- Once the table was full, new music failed to open and `saveFile` failed to open the save files.

**Fix:**
- `runtime/rxdk_patched/fileio.c` (marker `s3air xdk-paths 3`; re-run `tools/fix_rxdk_ppc64.py`) now has **1024 slots** and serializes slot allocation and release with a spinlock.
- It also exports `__rxdk_fd_count()`; the D3D9 drawer writes `Open files: N` to the log whenever the count grew by 8 or more.
- `FileIO::saveFile` on the 360 logs a warning when a file can't be opened or fully written.
- `game:\s3air.log` of the previous run is kept as `game:\s3air_previous.log`, so the log of a problem session survives a restart.

## Known toolchain behaviour (Phase 0)

| Topic | Result |
|---|---|
| C++17 (string_view, optional, enum class, range-for, structured bindings, if constexpr, fold expressions) | ✅ |
| libc++ (map, unordered_map, function, unique_ptr, tuple, atomic) | ✅ |
| Exceptions (throw/catch) | ✅ |
| Static constructors | ✅ |
| `thread_local` | ❌ Not supported. S3AIR only uses it once (jsoncpp) and it will become `static` |
| `xgraphics.lib` (XGTileSurface, …) | ⚠️ Not tested after the fix. Use linear textures or check it later |
| D3D9 (Direct3DCreate9, CreateDevice 1280x720, Present) | ✅ Xenia and RGH (with the memcpy fix) |
| XInput (XInputGetState, buttons) | ✅ RGH |
| `CreateFileA`/`ReadFile`/`WriteFile` on `game:\` and `d:\` | ✅ Reading, and writing (Xenia needs `allow_game_relative_writes = true`) |
| `fopen` from RXDK/picolibc | ✅ Hardware. ✅ Xenia since the libc patch from 17/10 (`rxdk_patched/fileio.c`). Before that, Xenia sent `\??\game:` to a NullDevice |
| Paths relative to the cwd (`testdata.txt`) | `fopen` ✅ (cwd = `game:\`). `CreateFileA` ❌, it needs `game:\...` |
| Render loop (Clear + Present) on hardware | ✅ **Root cause found (17/10, v6)**: RXDK's `memcpy`/`memmove`/`mempcpy` use VMX (`lvx`/`stvx`) at PASSIVE_LEVEL, and D3D uses them to copy into physical/write-combined GPU memory. On the console that makes `CreateDevice` take ~1 s and leaves the GPU with corrupt commands, so the first `Present`/fence freezes the console. Xenia doesn't reproduce it. Only the v6 versions with mem* without VMX worked (T6_2, T6_4). The official startup (T6_1) and MS-compiled code (T6_3) still failed. **Fix:** `runtime/memcopy_safe.c` (VMX only with IRQL 0 and both addresses < `0xA0000000`). `fix_rxdk_ppc64.py` swaps it into `libc.a`. **Confirmed on the RGH (v7, 17/10):** 4573 frames at a stable 60 fps, no workaround in the code |
| Exiting the title | `XLaunchNewImage(XLAUNCH_KEYWORD_DEFAULT_APP, 0)`. Returning from `main` reboots the console (RXDK runtime) |

## Xenia

- `xenia-canary.config.toml`: `allow_game_relative_writes = true` (behaves like an HDD launch on RGH, where `game:\` is writable).
- `DbgPrint` output shows up in `xenia.log` as `(DbgPrint) ...`. `OutputDebugStringA` does **not** show up.

## Testing on the console (RGH / ABadAvatar)

1. Copy the whole `bin\Release` folder (`default.xex`, `testdata.txt` and optionally the ROM) by FTP to, for example, `Hdd1:\Games\S3AIR_Test\`.
2. Launch it from Aurora/FSD/XeXMenu.
3. A green screen means every test passed. Red means a failure. A cycles the colour and BACK returns to the dashboard.
4. Fetch `cpp17hello.log` from the same folder by FTP.

### Stock XDK toolchain (control)

- The RXDK `2010-01` toolset (MS `cl.exe`/`link.exe`/`imagexex`) needs **VS2022 17.14 or newer**. Its task DLL loads `Microsoft.Build.CPPTasks.Common` 17.14. This machine has **17.6**, so MSBuild fails with MSB4062. Updating VS2022 in the Visual Studio Installer fixes it.
- Meanwhile, `tests/d3dmin/build_stock.cmd` calls the XDK tools directly (`E:\RXDK-360\bin\win32`).
- `tests/d3dmin` builds the same source with both toolchains (`build_stock.cmd` and `d3dmin_clang.vcxproj`). It writes `game:\d3dmin.log` **with a flush on every line**, and returns to the dashboard on its own after 300 frames.

### Crash diagnosis

- From v2 on, the test installs `SetUnhandledExceptionFilter`. On a crash, it writes `!!! CRASH` to the log with the exception code, IAR, LR, SP and the back-trace.
- Every build keeps `bin\<Config>\default.elf` with symbols (`RxdkClangDebug=true`). Don't copy it to the console.
- Symbolise with:
  ```bash
  python -I x360\tools\addr2sym.py x360\tests\cpp17hello\bin\Release\default.elf --log cpp17hello.log
  ```
- `/p:ExtraDefines=CRASH_TEST_FRAME=120` makes a build that crashes on purpose at frame 120, to validate the handler. Xenia doesn't emulate that fault, so it can only be tested on hardware.
- `x360\tools\elf_ar_symbols.py` lists the symbols of an `.a` (there's no `nm` in the toolchain).
