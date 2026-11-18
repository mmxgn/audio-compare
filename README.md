<p align="center"><img src="data/icons/hicolor/scalable/apps/org.mmxgn.audiocompare.svg" width="96" alt="audio-compare logo"></p>

# audio-compare

Compare two or more audio files by ear on GNOME.

Load some files, see their waveforms stacked, and A/B them at the same playback position — switch which one you hear without losing your place.

![screenshot](img/main.png)

## Features

- Open `mp3`, `wav`, `ogg` files (mono or stereo) via the Open button or drag-and-drop.
- One waveform pane per file, following the light/dark scheme and system accent colour.
- Switching keeps the playback position, so you always compare the same moment.
- Playback loops from the start on end.
- Each pane shows integrated loudness (LUFS) and max true peak (dBTP).
- Group tracks into **busses**: hover a track and press `0`–`9` to assign it.
  All tracks on the active bus play together, mixed and in sample-accurate sync,
  so you can A/B two busses. Busses are shown by a coloured border and a
  numbered badge; ungrouped tracks play on their own.
  The mix runs in floating point with a fixed -12 dB of master headroom, so a
  bus of several tracks sums without clipping and busses keep their true
  relative loudness. Everything plays quieter than the source files as a result.
- Invert a track's polarity (×−1) to line it up against another for null tests;
  the waveform flips to show it.
- Mute a track, or solo it within its bus; soloed tracks show an `S` badge so a
  solo left on another bus is visible before you switch back to it.

| Shortcut | Action |
|---|---|
| `Space` | Play / pause |
| `Alt+↑` / `Alt+↓` | Switch to the file above / below (keeps position) |
| `←` / `→` | Seek ∓5 s (`Shift` ∓1 s, `Ctrl` ∓100 ms) |
| `0`–`9` (over a pane) | Assign the track to that bus (same key again unassigns) |
| `-` / `i` (over a pane) | Invert the track's polarity (×−1) |
| `m` (over a pane) | Mute / unmute the track |
| `s` (over a pane) | Solo the track within its bus |
| Click a pane | Switch to it, keeping the position |
| Click + drag | Scrub the active pane |

## Install

**Nix** (Linux and macOS, x86_64 and Apple Silicon)
```sh
nix run github:mmxgn/audio-compare
```

**AppImage** (Linux) — grab the latest from the [releases page](https://github.com/mmxgn/audio-compare/releases/latest).
Each release also carries a `SHA256SUMS` file. Download it next to the
AppImage and check before running:
```sh
sha256sum --check --ignore-missing SHA256SUMS
chmod +x audio-compare-*.AppImage && ./audio-compare-*.AppImage
```

### macOS

The app runs natively on macOS (GTK's macOS backend, audio through Core Audio).
Download the `.dmg`, run it with Nix, or build it with Homebrew.

**Download** — get `audio-compare-<version>-arm64.dmg` from the
[releases page](https://github.com/mmxgn/audio-compare/releases/latest)
(Apple Silicon, macOS 15 or newer), open it and drag **Audio Compare** to
Applications.

The app is ad-hoc signed but not notarized by Apple, so macOS blocks it the
first time ("Apple could not verify…" or "…is damaged"). On macOS 15 and newer,
right-click → Open no longer gets past this; use one of these once:

- **System Settings**: try to open the app and dismiss the warning, then go to
  **System Settings → Privacy & Security**, click **Open Anyway** next to
  "Audio Compare was blocked", and confirm.
- **Terminal**: remove the download quarantine flag:
  ```sh
  xattr -dr com.apple.quarantine "/Applications/Audio Compare.app"
  ```

After that it opens normally.

**With Nix** — install [Nix](https://nixos.org/download/) with flakes enabled, then:
```sh
nix run github:mmxgn/audio-compare
```

**With Homebrew** — build from a checkout:
```sh
brew install meson ninja pkg-config gtk4 libadwaita gstreamer libebur128
meson setup build && ninja -C build && ./build/audio-compare
```

Notes:
- `Alt` in the shortcuts table is the `Option` (⌥) key, so switching files is `⌥↑` / `⌥↓`.
- The first launch after installing GStreamer prints plugin-scanner warnings
  (`gi.repository.Gst`, duplicate `objc` classes) while GStreamer builds its
  plugin cache. They are harmless and do not reappear.

**`.app` and `.dmg`** — build a self-contained `Audio Compare.app` from the
Homebrew stack (all libraries and GStreamer plugins are copied into the bundle,
so it runs without Homebrew installed):
```sh
brew install meson ninja pkg-config gtk4 libadwaita gstreamer libebur128 librsvg
packaging/macos/make-app.py          # dist/Audio Compare.app
packaging/macos/make-app.py --dmg    # also dist/audio-compare-<version>-<arch>.dmg
```
A locally built app isn't quarantined, so it opens directly on the Mac that
built it. Copied to another Mac it is blocked like a download; see the steps
above. It runs on the CPU architecture and macOS version it was built on and newer,
because the Homebrew bottles it copies target that macOS release.

## Develop

```sh
nix develop
meson setup build && ninja -C build && ./build/audio-compare
```

The dev shell works the same on Linux and macOS (it uses gcc on Linux, clang on macOS).

## Requirements

GTK 4 · libadwaita · GStreamer · libebur128
