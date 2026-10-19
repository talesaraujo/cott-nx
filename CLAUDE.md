# Crash of the Titans → Nintendo Switch (cott-nx)

Working notes for agents. Static recompilation port of the Xbox 360 game (2007, Radical Entertainment, Title ID
`565507E4`) to the Switch with [ReXGlue](https://github.com/rexglue/rexglue-sdk), modelled on
[nfsmw-nx](https://github.com/StevensND/nfsmw-nx). The user-facing overview is [README.md](README.md).

Personal setup (console IP, local paths, how the maintainer tests) goes in `CLAUDE.local.md`, which is not
committed. Read it too if it exists.

## Status

- Phases 0–3 done: disc extracted, runs on the PC (Linux), runs on the Switch, native Vulkan renderer.
- Phase 4, performance, in progress. Handheld: ~29–31 FPS in levels with dynamic resolution at ~0.75–0.9 of
  720p (GPU 460.8 MHz). Docked: 720p at ~28 FPS before the native skinning, CPU-bound; not measured since.
- Working: boot, menus, levels, saves, 2-player co-op, XMA 5.1 dialogue, Bink videos, Switch button prompts,
  "Nintendo Switch" texts, language from the console (plus a pt-BR translation), hbmenu icon.

## Rules

- **No game data in git, nothing of it sent anywhere**: ISO, XEX, `.rcf`, generated C++ (`app/generated/`),
  shader library (`build/shaders/`), captures, saves, dumps, extracted art, the generated icon
  (`tools/switch/icon/`), translation catalogs holding English (`translations/work/`). `translations/pt_BR.txt`
  (Portuguese + hash keys only) is committed. Code may hold only short snippets needed for matching (hashes,
  replacement fragments), never the game's text or art.
- **Licenses**: repo GPL-3.0; `sdk/` changes stay BSD-3-Clause. Mind over Recomp (GPL-3.0) may be reused with
  credit; MojoRecomp is ISC; Xelu's button icons are CC0. Credit in `THIRD_PARTY_NOTICES.md`.
- **Reference repos are read-only** (MojoRecomp, Mind over Recomp, nfsmw-nx). Copy with attribution.
- **Mark SDK changes** with a `COTT:` comment. Every `overrides.toml` entry says symptom, cause and how it was
  found.
- **Native replacements of game functions are guarded** (`app/src/cott_bink.cpp`, `cott_skinning.cpp`): the
  same operations in the same order (bit-identical), a cvar to turn it off, the first N calls and 1 in M after
  run both versions from the same memory and compare everything written; a difference logs and falls back.
  Test on the PC first (random inputs against the recompiled original).
- **After adding a hook, run `python3 tools/llamadas_directas.py`** before the PC build (the Switch build runs it
  itself). Otherwise callers still call `__imp__sub_X` directly and the hook never fires.
- **Performance**: measure on the console at stock clocks, A/B in the same scene; handheld mode is the baseline,
  docked is extra. Look at the distribution, not only the mean.
- **Every new wait between threads has a timeout and logs how long it waited.**
- **README.md is English; README.pt-BR.md is the Portuguese copy.** Change both in the same commit.
- **Never hand game-derived material to external services or other AI tools**: their prompts must forbid
  reading `build/`, `logs/` and `app/generated/`.

## Layout

| Path | What |
|---|---|
| `app/cott_manifest.toml`, `app/overrides.toml` | Codegen config: functions found by hand, jump tables, `[rexcrt]` fibers |
| `app/src/main.cpp`, `cott_app.h` | App entry, cvar defaults (`SetIfNotRequested`), game_root/ISO discovery, nxlink stdout |
| `app/src/cott_bink.cpp` | Bink IDCTs and coefficient reader, native + guard |
| `app/src/cott_skinning.cpp` | CPU skinning `sub_82353478`, native + guard + self-test |
| `app/src/cott_prompt_icons.cpp` (+ `_data.h`, `cott_font_pages.h`) | Xbox button glyphs → Switch ones in fonts/textures |
| `app/src/cott_switch_text.cpp` | "Xbox 360" → "Nintendo Switch" in loaded texts; calls the translator first |
| `app/src/cott_language.cpp` | `cott_language`, pt-BR strings, letters drawn into fonts |
| `app/src/cott_ring_wait.cpp` | Diagnostic: where/how long the game waits for its D3D ring |
| `app/prompts/` | Switch button images (Xelu, CC0) |
| `sdk/` | ReXGlue SDK v0.10.0 + nfsmw-nx Horizon layer + our fixes |
| `sdk/src/graphics/native/` | The native renderer (below) |
| `sdk/src/ui/switch_apm.cpp`, `switch_perf.cpp` | GPU clock request; perf log + stack sampler |
| `sdk/src/audio/xma_context.cpp` | XMA multi-stream (5.1) fix |
| `sdk/patches-from-mindover/` | Mind over Recomp SDK patches applied (0003, 0005, 0008, 0014) |
| `tools/switch/` | `build_nro.sh`, `send.sh`, `pull_logs.sh`, `build_mesa_linux.sh`, `make_icon.py` |
| `tools/shaders/` | `build_translator.sh` (MojoRecomp translator + XenosRecomp patches), `xsh_extract.py` |
| `tools/text/` | `textbible.py` (RCF/TextBible reader), `export_po.py`, `compile_po.py` |
| `tools/` | `fase1_extraer.py` (ISO → files), `llamadas_directas.py`, `huecos.py`, `lee_antes.py`, `calientes.py`, `mojo_lookup.py`, `prompt_icons.py` |
| `translations/` | `pt_BR.txt` (compiled) and the translators' guide (Portuguese) |

Tool names come from nfsmw-nx and are Spanish: huecos = gaps, llamadas directas = direct calls, lee antes =
read-before, calientes = hot, fotograma = frame, anillo = ring, perfil = profile, pila = stack.

## Commands

```sh
# Recompiler (once): see README. Codegen, from app/:
../sdk/out/linux-amd64/rexglue codegen cott_manifest.toml > codegen.log 2>&1

# PC (Linux): build and run; logs in logs/pc/run_*.log
python3 tools/llamadas_directas.py        # after codegen or a new hook
cmake --build app/out/linux
tools/pc/run.sh [--cvar=value ...]        # passes game_root, shader paths, log file; add --native_dynamic_resolution=true

# Switch NRO (~5 min with LTO), in the devkitA64 podman container
NVK_SDK=<mesa-switch>/mesa-linux-install/opt/devkitpro/portlibs/switch tools/switch/build_nro.sh
cp app/out/switch/cott logs/switch/elf/cott_<name>.elf     # keep the ELF of every tested build
SWITCH_IP=<ip> tools/switch/send.sh [--cvar=value ...]    # nxlink; hbmenu NetLoader (Y) must be open
SWITCH_IP=<ip> tools/switch/pull_logs.sh                  # ftpd on port 5000: logs, crash reports

# Shaders: capture missing microcode on the PC (build/shaders/ucode), then translate
tools/shaders/build_translator.sh
MOJORECOMP_DXC_LIB=build/shaders/mojo_xlate/libdxcompiler.so build/shaders/mojo_xlate/mojo_xlate \
  build/shaders/ucode build/shaders/spv   # copy spv/ to sdmc:/switch/cott-nx/shaders

# Translation catalog and icon
python3 tools/text/export_po.py && python3 tools/text/compile_po.py
python3 tools/switch/make_icon.py && touch app/CMakeLists.txt
```

On the SD card: `sdmc:/switch/cott-nx/{cott.nro, game_root/, shaders/, logs/, cott.toml}`. Start hbmenu by title
takeover (hold R on a game) or a forwarder; the Album applet has too little memory.

## Architecture

- **Recompiled game**: `rexglue codegen` writes one C++ function per PowerPC function (19,774 found by the
  analyzer; ~25 pointer-only ones and 2 tail-call-only ones added by hand from MojoRecomp's map via
  `tools/mojo_lookup.py --missing`). XAPI fibers mapped in `[rexcrt]`. Every `REX_LOAD`/`REX_STORE` is
  `volatile`, registers live in `ctx`: this is why native rewrites of hot functions pay off.
- **Calls**: `tools/llamadas_directas.py` turns calls between unhooked functions into direct calls to the
  non-weak `__imp__sub_X` (LTO can inline them); hooked ones keep calling `sub_X`. LTO: `-flto=2
  -flto-partition=balanced`.
- **Hooks**: `REX_HOOK_RAW(sub_X)` + `REX_EXTERN(__imp__sub_X)` replaces a function; the original is
  `__imp__sub_X(ctx, base)`. Guest memory is `base + addr`, big-endian; addresses are 32-bit (sum in `uint32_t`).
- **Native renderer** (`sdk/src/graphics/native/`, `gpu_backend=native`, default): the game's D3D keeps running
  and writes its PM4 ring; the SDK still parses PM4, registers, MMIO, interrupts and the read pointer. A
  command thread redraws each frame with Vulkan (dynamic rendering, BDA) in 3 frame slots.
  - `targets.cpp`: EDRAM surfaces as Vulkan images keyed by base/pitch/format/MSAA; resolves into textures by
    address; 4X shares the 1X/2X image last used at that base; depth-only clear rects also clear RGBA8 images at
    the same base. Resolution scale and dynamic rescale (`PrepareRescale` on a worker thread, `FinishRescale`
    blits between frames).
  - `draw.cpp`: pipelines, fixed state decoded like MojoRecomp's `vk_presenter.cpp`, rect/quad lists expanded
    on the CPU, NEON vertex byte-swap and index reads, depth copies by a draw (`shaders/depth_copy.*`).
  - `textures.cpp`: untile + endian swap by runs of 8/16 bytes; content hash (small textures every frame,
    big ones every 2–16).
  - `command_processor.cpp`: `native_untile` draws the scene once instead of the 3 predicated-tiling strips
    (learns the strip layout, widens targets to 1280, skips strips 2–3 and the replayed indirect buffers);
    `native_coalesce_copies`; 10 s report; GPU timestamps calibrated against the CPU clock;
    `UpdateDynamicResolution` (every 15 frames vs `native_dynamic_resolution_target_ms`, steps of 0.05,
    0.6–1.0).
  - Shaders: translated ahead of time on the PC (MojoRecomp's translator + XenosRecomp, DXC → SPIR-V,
    constants via UBO set 4, forward-jump shaders linearized, predicated loop breaks honoured), keyed by the
    ring microcode hash. A missing shader skips the draw and its microcode is saved to
    `native_shader_capture_path`. The game uses only ~32 shaders.
- **Audio**: XMA in a worker thread; 5.1 dialogue is 3 interleaved stereo streams, each context restarts every
  64 KB block at its own stream's packet (`XmaContext::LearnStreamIndex`).
- **Saves**: `HostPathEntry` keeps its own size after writes (on the Switch a stat of a file open for writing
  does not see the writes; the game stores the file size in the first dword).
- **Input**: `input_xbox_layout=true` on the Switch (bottom button = Xbox A); prompt glyphs follow by position.
- **Text and language**: `pure3d::TextBible` per bank (chunk 0x1800D) holds `pure3d::Language`s (0x1800E);
  `sub_823271F8` loads each. Translations live in one system-heap block; the English Language's offsets are
  repointed at it (game reads `text + (offset & ~1)` in 32 bits, `sub_822F2140`). Keys: FNV-1a 64 of the
  English UTF-16 (hashes are only unique per bank). õ/Õ use the ø/Ø glyphs.

## Switch / NVK lessons

- 3 cores at 1020 MHz; GPU 307.2 MHz handheld by default, 460.8 MHz via the official apm configuration
  `0x92220008` (`cott_switch_gpu_mhz`, default 460), 768 MHz docked. The apm request only applies in handheld:
  if started docked, it is made on undock and repeated after every undock. The memory-clock watchdog must not
  touch the docked 1600 MHz.
- NVK memsets and cache-flushes every new allocation on the CPU (`nouveau_horizon_memory_create`): never create
  big images between frames (27–100 ms hitches); do it on a worker thread.
- NVK GPU timestamps tick at 1.627 ns (1000/614.4). Per-kind GPU splits are unreliable; measure by removing
  work (`native_debug_skip_copies`).
- Each fence check and `vkQueueSubmit` is a syscall; GPU allocations are slow and limited (~4,000 NvMap
  handles); uncached pipeline creation ~59 ms; the CPU sees upload memory uncached (write it in big NEON
  stores).
- Only priority 0x3B time-slices; a busy thread starves same-priority ones even with free cores. libnx has one
  exception stack. `std::thread::detach()` can kill the process: use persistent threads.
- Memory mappings are limited (2001-0103, not RAM): map guest memory on demand.
- Render targets need FSI (`render_target_path_vulkan=fsi`) for the emulated path; FBO shows nothing.

## Game facts

- XEX (USA, `e76824b2…6373`) = MojoRecomp's, so its addresses apply. Base 0x82000000, entry 0x82264F40, XDK
  2.0.5426. Engine Pure3D/PDDI (xn* backend), RTTI present. Data in Radical "Cement" `.rcf` archives
  (directory at 0x24, big-endian). 30 Hz game: the loop dispatches a frame every 33,333 µs.
- Scene: 1280x720, 2xMSAA, 3 vertical strips (x 0/416/832, 448 wide); color at EDRAM base 0, depth (D24FS8) at
  540, velocity (motion blur) at 1080. Resolves use `copy_dest_swap` (R/B swapped). The game draws 720p or its
  640x480 widescreen mode, nothing in between (intermediate scales come from the renderer).

| Address | What |
|---|---|
| `sub_8226D968` / `sub_8226C958` | D3D busy-wait loop / wait for ring room (read pointer at `[device+10768]`) |
| `sub_82274080` | D3D Present; `sub_8226CCC8` waits for the command thread to pass the previous Present (`[device+14788]`): the game runs at most one frame ahead |
| `sub_82353478` | CPU skinning (native) |
| `sub_8243EA58`, `sub_8243E370`, `sub_8243EDE0` | Bink IDCT add / put, coefficient reader (native); `sub_82436E38` block copies (not yet) |
| `sub_823271F8` | Loads one `pure3d::Language` |
| `sub_82339970` | `TextureFont` loader (fonts with button glyphs U+00B1–U+00BE) |
| `sub_82312458` | `TextureLoader` |
| `0x824736C4` | Game language letter (`sub_822882F8` get, `sub_8228AAC8` set) |
| `0x823E8B50…0x823E8D70` | XAPI fibers (Convert/Create/Delete/SwitchToFiber); scheduler switch at 0x823E9C10 |
| `0x823CEC90` | libc memcpy |

Pure3D texture data used by the GPU sits 0x1000 after the fetch-constant address (indices and palette).

## Measuring and debugging

- `logs/rex/rex_perfil.log` (SD, pulled by `pull_logs.sh`): 10 s blocks with FPS, mode (handheld/docked),
  clocks, per-thread CPU. `logs/rex/perfil_pilas.flag` on the SD turns on 1 ms stack sampling; resolve
  `imagen+0x…` with `nm -C` on the saved ELF of that build. The main game thread is `Main XThread`, the renderer
  is `GPU Commands`.
- Renderer report: `[native] N swaps/s, draws, copies…` and `dynamic resolution: …` lines in the live log.
- `--native_dump_trigger=<dir>/now` then `touch` it: dumps all targets as PPM and traces the next frame's draws.
- `cott_ring_wait_stats=true`: who waits for the ring and how long. `--driver_env=NVK_SHADER_STATS=1`: shader
  stats to `rex_stderr.log`.
- PC hangs: no gdb; `eu-stack -p $(pgrep -x cott)` (not `pgrep -f`, which matches your own shell).
- Hot lines in the Switch build (no `-g`): `aarch64-none-elf-objdump` inside the devkitA64 container.

## Pitfalls already paid for

- Two NaN inputs: which NaN comes out depends on operand order the compiler picks; compare NaN == NaN in guards.
- simde `vmaxfp`/`vminfp`: NEON keeps the unflushed denormal (vbslq), SSE with DAZ gives the flushed zero.
- fmadds in generated code = `float(std::fma(double, double, double))` (double rounding); FPCR flush-to-zero
  toggles per VMX op (costly); `-ffp-contract=off` everywhere.
- A pointer built as `base + text + offset` in 64 bits went past 4 GB: guest address sums are 32-bit.
- PC: the GPU plugin `.so` loads after the app sets defaults, so plugin cvars need the command line there; the
  game loads `librexgpu-xenosrd.so` from the app build tree (`cmake --build app/out/linux`).
- `ls` is aliased to eza on the maintainer's machine: use `command ls` / `/bin/ls` in scripts.
- nxlink: options after the file need `--args`; `-p` is relative to `sdmc:/switch`; probing the NetLoader TCP
  port kills it (use the UDP ping); `stdbuf -oL` for a live log.
- The icon is only looked up at CMake configure time (`touch app/CMakeLists.txt`).

## Next steps (performance)

- Measure docked after the native skinning (CPU was the docked limit).
- Main thread now waits 16–30% on the ring in handheld levels: the command thread (50–65% of a core) and the GPU
  are the limit. Candidates: command-thread CPU, PGO + function ordering, registers as locals (risky with the
  fiber scheduler, which uses LR/MSR), generated code without `volatile`.
- Dynamic resolution flips between two steps every ~3.5 s; add hysteresis (some switches cost 10–18 ms).
- GPU: depth of field (5.4 ms), motion blur (2.7 ms), distortion-particle copies of the whole strip.
- Open items: smaller level fonts (151/104 glyphs) may lack accents for pt-BR; Dutch/Russian are picked by
  country/menu, not XGetLanguage (auto treats them as English); occasional "XmaContext: There are no bits to
  copy!" warnings (no audible effect).
