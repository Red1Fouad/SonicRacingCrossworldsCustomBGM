# Sonic Racing CrossWorlds Custom BGM

Replace the in-game music in **Sonic Racing CrossWorlds** with your own tracks.

Drop your audio files into folders next to the EXE, launch the app, and play the game — custom music plays automatically during races, in the lobby, and on the title screen.

## Quick Start

1. Download the latest release from [Releases](https://github.com/Red1Fouad/SonicRacingCrossworldsCustomBGM/releases)
2. Extract `SonicCustomBGM.exe`, `SonicCustomBGM.dll` and `SDL2.dll` into a folder
3. Create music folders next to the EXE:
   - `music/` — race BGM
   - `music_lobby/` — lobby BGM
   - `music_title/` — title screen BGM
4. Drop your audio files into the folders
5. Launch `SonicCustomBGM.exe`
6. Start the game — the app auto-detects and injects into the game process

## Supported Formats

| Format | Extensions | Notes |
|--------|-----------|-------|
| WAV | `.wav` | PCM |
| MP3 | `.mp3` | |
| OGG Vorbis | `.ogg` | |
| FLAC | `.flac` | |
| CRI ADX | `.adx` | With loop points |
| CRI AAX | `.aax` | ADX in @UTF container |
| AAC / M4A | `.aac`, `.m4a` | ADTS and MP4 container |
| Nintendo BRSTM | `.brstm` | DSP-ADPCM, PCM, IMA-ADPCM |
| Nintendo BCSTM | `.bcstm` | |

## Music Folders

| Folder | When it plays |
|--------|--------------|
| `music/` | During races (main gameplay BGM) |
| `music_lobby/` | In the online lobby |
| `music_title/` | On the title screen |

Folders are auto-created on first launch. Each folder can contain an optional `tracks.txt` for per-track settings:

```
; filename: weight, volumeMul
MySong.mp3: 1.5, 0.8
AnotherTrack.ogg: 0.5, 1.0
```

- **weight** — Controls shuffle probability. Higher = more likely to be picked. Default: `1.0`
- **volumeMul** — Per-track volume multiplier. Default: `1.0`

## Features

- **Auto-injection** — Detects the game process and injects automatically
- **Shuffle mode** — Plays tracks in a randomized order with weighted probability
- **Loop mode** — Loops the current track infinitely (supports embedded loop points in ADX/BRSTM)
- **Favorites** — Star tracks to include them in favorites-only shuffle
- **Search & filter** — Real-time search across all tracks
- **Volume control** — Global volume slider (0%-500%)
- **Per-track volume** — Set via `tracks.txt`
- **Skip leading silence** — Optional trimming of silence at the start of tracks
- **Mute on unfocus** — Mutes custom BGM when neither the app nor game has focus
- **System tray** — Minimize to tray with context menu controls
- **Start with Windows** — Optional registry autostart
- **Hotkeys** — Configurable keyboard and gamepad bindings
- **Themes** — Dark, Light, High Contrast
- **Update checker** — Checks GitHub for new releases on startup
- **Hot-reload** — Refresh button to reload music from disk without restarting

## Hotkeys

| Action | Default Keyboard | Default Gamepad |
|--------|-----------------|-----------------|
| Skip Track | `Ctrl + Right` | Unbound |
| Previous Track | `Ctrl + Left` | Unbound |
| Volume Up | `Ctrl + Up` | Unbound |
| Volume Down | `Ctrl + Down` | Unbound |

Hotkeys are configurable in the Settings popup. Supports key combos (up to 5 keys). Press `ESC` to clear a binding. Bindings are saved to `hotkeys.txt`.

## Configuration Files

All stored next to the EXE:

| File | Purpose |
|------|---------|
| `settings.txt` | Playback and UI settings |
| `hotkeys.txt` | Keyboard and gamepad bindings |
| `favorites.txt` | Favorited track filenames |
| `recent.txt` | Recently played tracks (max 50) |
| `music/tracks.txt` | Per-track weight and volume |
| `SonicCustomBGM.log` | Runtime log |

## Building

Requires Visual Studio 2022 Build Tools (MSVC v144) and Python 3 (for sprite generation).

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1
```

Produces `SonicCustomBGM.exe` and `SonicCustomBGM.dll` in the project root.

### Dependencies

| Library | Purpose |
|---------|---------|
| [Dear ImGui](https://github.com/ocornut/imgui) | GUI (DX9 + Win32 backends) |
| [MinHook](https://github.com/TsudaKageworks/minhook) | API hooking |
| [SDL2](https://www.libsdl.org/) | Gamepad input |
| [dr_mp3](https://github.com/mackron/dr_libs) | MP3 decoding |
| [dr_flac](https://github.com/mackron/dr_libs) | FLAC decoding |
| [stb_vorbis](https://github.com/nothings/stb) | OGG Vorbis decoding |
| [libhelix-aac](https://github.com/nicklausw/libhelix-aac) | AAC/M4A decoding |
| XAudio2 | Audio playback |
| Direct3D 9 | Rendering |
| WinHTTP | Update checking |

## Credits

- **Developer**: RED1
- **Special Thanks**: RyoTune — CRI Atom / ACB information (Ryo Framework)

## License

See individual library files for their respective licenses.
