# Linscribe

A lightweight Linux desktop app for voice notes and system-wide speech-to-text dictation. Lives in your system tray, records voice memos with one click, and can type transcribed speech directly into any focused application.

Powered by [Mistral's Voxtral](https://docs.mistral.ai/capabilities/audio/) real-time transcription API.

## Features

**Voice Notes**
- Record, save, play back, and delete voice memos from a minimal GTK3 window
- Notes stored as WAV files in `~/.local/share/linscribe/`
- Real-time transcription displayed live as you record
- Batch transcription of saved notes with one click
- Copy transcription text to clipboard
- **Audio device selection** &mdash; choose any input source in Settings, including output monitors (e.g. "Monitor of Built-in Audio") to transcribe system audio playing through speakers or headphones

**Dictation Mode**
- Activate from the system tray menu to start typing speech into any focused application
- Works with terminals, editors, browsers, and any other window
- Tray icon changes to indicate active dictation
- Automatic detection of the best available typing backend:
  - **Wayland** (GNOME, KDE, Sway, etc.): `ydotool`, `wtype`, or `xdotool`
  - **X11**: `libxdo` (direct, no subprocess overhead)
  - **X11 global hotkey**: `Ctrl+Shift+Space` via keybinder (configurable in Settings)

**System Tray**
- Runs as a tray application &mdash; close the window and it keeps running
- Quick access to Transcribe (open window), Start/Stop Dictation, Settings, and Quit

## Requirements

- Ubuntu 22.04+ or similar Linux distribution
- PulseAudio (or PipeWire with PulseAudio compatibility)
- A [Mistral API key](https://console.mistral.ai/api-keys/) (free tier available)

## Dependencies

Install build and runtime dependencies:

```bash
sudo apt install \
  build-essential \
  xmake \
  libgtk-3-dev \
  libayatana-appindicator3-dev \
  libpulse-dev \
  libsoup-3.0-dev \
  libjson-glib-dev \
  libkeybinder-3.0-dev \
  libxdo-dev
```

For dictation mode on **Wayland** (most modern Ubuntu/Fedora desktops), you also need at least one of:

```bash
# Recommended for GNOME Wayland:
sudo apt install ydotool

# Alternative for wlroots compositors (Sway, Hyprland):
sudo apt install wtype
```

> **Note:** On Wayland, global hotkeys are not available due to compositor security restrictions. Use the tray menu to start/stop dictation. On X11, `Ctrl+Shift+Space` works as a global hotkey (configurable in Settings).

## Build

```bash
xmake
```

The binary is output to `build/linux/x86_64/release/linscribe`.

To build in debug mode:

```bash
xmake f -m debug
xmake
```

## Usage

### Run

```bash
xmake run linscribe
```

Or run the binary directly:

```bash
./build/linux/x86_64/release/linscribe
```

### Set your API key

On first launch, open **Settings** from the tray menu and enter your Mistral API key. Alternatively, set the environment variable:

```bash
export MISTRAL_API_KEY="your-key-here"
```

The saved key is stored in `~/.local/share/linscribe/mistral_api_key`.

### Record a voice note

1. Click the tray icon and select **Transcribe** to open the main window
2. Click **Record** to start recording (live transcription appears as you speak)
3. Click **Stop** when finished
4. Click **Save** to keep the note or **Discard** to throw it away
5. Saved notes appear in the list with playback, transcribe, copy, and delete controls

### Dictation mode

1. Click the tray icon and select **Start Dictation**
2. Focus the window where you want text to appear (terminal, editor, browser, etc.)
3. Speak naturally &mdash; transcribed text is typed into the focused application in real time
4. Click **Stop Dictation** in the tray menu to finish

## Data storage

All data is stored in `~/.local/share/linscribe/`:

| File | Purpose |
|------|---------|
| `note_*.wav` | Recorded voice notes |
| `note_*.txt` | Transcription sidecar files |
| `mistral_api_key` | Saved API key |
| `dictation_hotkey` | Custom hotkey binding (default: `<Ctrl><Shift>space`) |
| `audio_device` | Selected PulseAudio source name (empty = system default) |

## Tech stack

- **C++17** with GTK3 for the UI
- **PulseAudio** for audio capture and playback
- **libsoup 3.0** for HTTP and WebSocket communication
- **json-glib** for JSON parsing
- **Ayatana AppIndicator** for system tray integration
- **keybinder** for global hotkeys (X11)
- **libxdo** / **ydotool** / **wtype** for keystroke simulation
- **xmake** build system

## License

MIT
