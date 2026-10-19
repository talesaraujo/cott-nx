<h1 align=center>Crash of the Titans — Nintendo Switch port</h1>

<p align=center><b>English</b> · <a href="README.pt-BR.md">Português (Brasil)</a></p>

An unofficial native port of the Xbox 360 version of **Crash of the Titans** (2007) to the Nintendo Switch.

> [!WARNING]
> **In development.** The whole game runs on the Switch and is playable, but performance and polish work remain.
> This repository is private for now and is the working base.

It is not an emulator. The PowerPC code of the game's `default.xex` is statically recompiled to C++ with
[ReXGlue](https://github.com/rexglue/rexglue-sdk) and built for the Switch, following the path opened by
[nfsmw-nx](https://github.com/StevensND/nfsmw-nx), the Need for Speed: Most Wanted port.

> [!NOTE]
> You need your own copy of the Xbox 360 game. No game file, and nothing generated from one, goes into this
> repository.

## Supported edition

| File | Size | SHA-256 |
|---|---|---|
| `default.xex` (USA, En/Fr/De/Es/It/Nl) | 5,382,144 | `e76824b242c6c628738c373bdf052d456945d8e343eb934100617fd423e86373` |
| `default.rcf` | 1,266,263,595 | `42c04b348b02a47ed1fd98ce15b93907843de4d0977aa4d21f0722f1e9283874` |

## Status

1. **Phase 0, extract the disc and identify the executable:** done.
2. **Phase 1, run on the PC (Linux) with ReXGlue:** done.
3. **Phase 2, bring it to the Switch:** done.
4. **Phase 3, native Vulkan renderer with shaders translated ahead of time:** done.
5. **Phase 4, performance:** in progress. In handheld mode the levels run at ~29–31 FPS with dynamic resolution
   (~0.75–0.9 of 720p); docked, at 720p, ~28–30 FPS. All at stock clocks.

Local co-op, saves, sound, videos, Switch buttons in the game's prompts and the language picked from the console's
language (with a Brazilian Portuguese translation) also work.

## Layout

| Folder | Contents |
|---|---|
| `sdk/` | ReXGlue SDK v0.10.0 with nfsmw-nx's Horizon layer and this port's fixes (search for `COTT:`) |
| `tools/` | Disc extraction, third-party library download, tools to study the generated code |
| `assets/` | The extracted disc (ignored by git) |

## Building the recompiler (Linux)

```sh
python3 tools/fetch_thirdparty.py
cmake -S sdk -B out/host -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_C_FLAGS=-march=x86-64-v2 -DCMAKE_CXX_FLAGS=-march=x86-64-v2
cmake --build out/host --target rexglue
# the executable ends up in sdk/out/linux-amd64/rexglue
```

On Fedora, the development packages it needs:

```sh
sudo dnf install gtk3-devel glslc libshaderc-devel libXcursor-devel libXi-devel libXrandr-devel libXScrnSaver-devel \
  libXfixes-devel wayland-protocols-devel libdecor-devel mesa-libEGL-devel mesa-libGL-devel mesa-libgbm-devel \
  pipewire-devel pulseaudio-libs-devel alsa-lib-devel dbus-devel systemd-devel libusb1-devel
```

## Extracting the game

```sh
python3 tools/fase1_extraer.py "Crash of the Titans (USA).iso" -o assets/game_root
```

## Language

The game's language is chosen with the `cott_language` option, in the `cott.toml` next to `cott.nro`:

```toml
cott_language = "en"   # auto (default), en, fr, de, es, it or pt-BR
```

- `auto` follows the Switch's language: English, Spanish, French, German and Italian (with the game's dubbed voices),
  or pt-BR with the Switch in Portuguese. Any other Switch language gives English.
- `pt-BR` is the port's Brazilian Portuguese translation: Portuguese text with the original English voices. No
  language of the game is replaced; with `en`, the game is exactly as it was.

The translation lives in [`translations/`](translations/README.md), which explains (in Portuguese) how to help
improve it.

## Credits

- **Radical Entertainment and Vivendi Games / Activision**, creators of Crash of the Titans. This is a fan port with
  no affiliation.
- **[StevensND](https://github.com/StevensND/nfsmw-nx)**: nfsmw-nx, this port's model (Horizon layer, tools,
  documentation).
- **[Tom Clay](https://github.com/rexglue/rexglue-sdk)**: the ReXGlue SDK, built on the work of the
  **[Xenia](https://xenia.jp)** team.
- **[OAleex](https://github.com/OAleex/MojoRecomp)**: MojoRecomp, the PC port of Crash of the Titans, whose map of the
  executable guides this work.
- **[Maxilure](https://github.com/Maxilure/MindOver-Recomp)**: Mind over Recomp, the PC port of Crash: Mind over
  Mutant, with the SDK fixes and the findings about Radical's engine.
- The libraries listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Legal

Not affiliated with Activision, Radical Entertainment, Microsoft or Nintendo. "Crash Bandicoot" and "Crash of the
Titans" are trademarks of their respective owners. This repository contains no asset or code of the original game.

Source code under the GPL-3.0 (see [LICENSE](LICENSE)). The changes in `sdk/` stay under the SDK's BSD-3-Clause
license (see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)).
