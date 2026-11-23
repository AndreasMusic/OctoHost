# OctoHost

A minimal, fast, native **C++17 / JUCE** VST3 & Audio Unit workstation for **macOS** and **Windows**, designed for bassists, guitarists, and songwriters who want to program a drum groove in seconds and layer instruments through amp sims, effects, and send busses without DAW bloat.

## Download & Install

Grab the latest pre-built package for your platform from the **[Releases](../../releases)** page:

### macOS (Universal: Apple Silicon & Intel, macOS 11.0+)
1. Download and unzip **`OctoHost-macOS-Universal.zip`**.
2. Drag **`OctoHost.app`** into `/Applications`.
3. Because community GitHub builds are ad-hoc signed, clear the macOS quarantine attribute once in Terminal before launching:
   ```bash
   xattr -cr /Applications/OctoHost.app
   ```
   *(Alternatively, right-click `OctoHost.app` -> **Open** -> click **Open**.)*
4. Launch OctoHost, click **Plugins** -> **Options** -> **Scan for new or updated plug-ins** to index your VST3 (`/Library/Audio/Plug-Ins/VST3`) and Audio Unit (`/Library/Audio/Plug-Ins/Components`) folders.

### Windows (64-bit, Windows 10/11)
1. Download and unzip **`OctoHost-Windows-x64.zip`**.
2. Run **`OctoHost.exe`** (statically linked — no extra Visual C++ runtime install required).
3. If Windows SmartScreen warns about an unrecognized app, click **More info** -> **Run anyway**.
4. Click **Plugins** -> **Options** -> **Scan for new or updated plug-ins** (scans `C:\Program Files\Common Files\VST3` automatically).

---

## Core Features

- **Dynamic Track Console (1 to 24 Tracks)**: Starts with an 8-track Guitar/Bass/Drum template (`DRUMS`, `BASS`, `GTR L`, `GTR R`, `LEAD`, `CLEAN`, `SYNTH`, `FX/VOX`), with **`+ Track`** (Audio, MIDI, Double-Tracked Guitar L/R Pair, Bass, Pad), **Duplicate Track** (clones clips + all 4 plugin states), and **Remove Track**.
- **4 Inserts per Track & Bus + 2 FX Send Busses**: 4 serial VST3/AU insert slots on every track, **FX Bus A**, **FX Bus B**, and **MASTER**, with **Copy / Paste 4-Slot Chain** and right-click **Pre-Fader / Post-Fader** sends.
- **Right-Click Studio Control**: Right-click any track strip or I/O badge to assign Hardware Mono/Stereo Input, MIDI Channel, or Direct Hardware Output (re-amp / cue feeds), plus **Phase Invert**, **HPF (35 / 80 / 120 Hz)**, **Noise Gate (Tight / Metal)**, **Console Drive (Warm Tape / Grit)**, and **Input Trim (-6 to +12 dB)**.
- **4-Tab Studio Editor Deck** (`1`–`4`, `D` to hide):
  1. **Drum Sequencer**: 8 GM lanes, 4 patterns (`A–D`), odd time signatures (`4/4`, `7/8`, `6/8`, `5/8`, `2/4`), velocity cycling, Swing, 7 instant rock/metal/funk grooves, built-in punchy rock drum synth (auto-yields to any VSTi drum plugin), and `Commit -> Trk 1`.
  2. **Piano Roll**: Playable keyboard ruler, scale highlighting, snap grids (`1/4`–`1/16`, `1/8T`), drag move/resize, velocity painting lane, Quantize, Humanize, Transpose, Snap-to-Scale, Legato, and `.mid` Import/Export.
  3. **Audio Editor**: Zoomable waveform editor with 8-level Undo, Trim, Silence, Auto-Trim Silence, Normalize (`-1 dB`), Cosine Fade In/Out, Gain `+/-3 dB`, Reverse, Time-Nudge, and WAV Import/Export.
  4. **Melody Forge**: Musical MIDI auto-creator (**Kick-Locked Bassline Pocket**, **Prog/Metal Riff**, **Lead Melody**, **Power Chords**, **Dark Pads**, **16th Synth Arp**) + built-in 4-patch Poly Synth.
- **Chromatic Bass & Guitar Tuner (`T`)**: Lock-free YIN pitch detector tuned down to 27 Hz (low B/C/E bass ready) with **Silent Tune**.
- **Mastering & Export (`File v`)**: Master Peak + RMS metering, `CLIP` latch, `MONO` check, `LIM` safety limiter, `DIM` (`-12 dB`), full `.octo` Session Save/Load, **24-bit Stereo WAV Mixdown**, and **Multi-Track 24-bit WAV Stem Export**.

## Keyboard Shortcuts

| Key | Action |
| :--- | :--- |
| `Space` | Play / Pause |
| `R` | Record (with optional 1-Bar Count-In) |
| `L` | Toggle Loop |
| `M` | Toggle Metronome Click |
| `T` | Toggle Chromatic Tuner |
| `1` – `4` | Switch Editor Deck (Drums / Piano Roll / Audio Editor / Melody Forge) |
| `D` | Show / Hide Editor Deck |
| `Return` | Rewind to Start |

## Building from Source

Requires **CMake 3.22+** and **Xcode Command Line Tools** (macOS) or **Visual Studio 2022+** (Windows). JUCE 8.0.6 is fetched automatically via CMake `FetchContent`.

* **macOS:** `./build_mac.sh`
* **Windows:** Run `build_win.bat`

## License

Built with [JUCE](https://juce.com/) under the **GNU AGPLv3** open-source license.
