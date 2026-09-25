# Music to mic

A tiny Windows tray switch that sends whatever your PC is playing (music, a video, a game) into your microphone, so everyone in a Discord call hears it together with your voice. Click the tray icon: green = on, grey = off.

- **No echo for the others.** Discord's own output is excluded from the capture (Windows process-loopback API), so people never hear themselves coming back.
- **You hear nothing different.** Your speakers or headphones keep playing as usual; the tool only *copies* the sound into the mic.
- **One click, nothing else to configure.** While ON, the Windows default microphone is switched to the virtual cable; OFF restores your real mic. Discord (or any app) just has to use the *Default* input device.
- Single 340 KB exe, no installer, no background service, ~16 MB RAM, near-zero CPU.

## Requirements

- Windows 11 (or Windows 10 2004+).
- [VB-CABLE](https://vb-audio.com/Cable/) by VB-Audio (free, donationware). It provides the virtual microphone the mix is written into. It is not bundled here because its licence does not allow redistribution; download `VBCABLE_Driver_Pack45.zip` yourself.

## Install

1. Download `MusicToMic.exe` from [Releases](../../releases) (or build it, see below) into a folder, e.g. `D:\music-to-mic`.
2. Unzip the VB-CABLE package into a `vbcable` subfolder and run `install-cable.cmd` **as administrator** (or run `VBCABLE_Setup_x64.exe` from the zip as administrator). Reboot if `CABLE Input` / `CABLE Output` do not appear in Windows sound devices.
3. Run `MusicToMic.exe`. It puts an icon on the taskbar tray and registers itself to start with Windows (right-click the icon to turn that off).
4. In Discord: *Settings → Voice & Video → Input Device* must be **Default**. While sharing music, turn Discord's **Noise Suppression** off, otherwise it filters the music out as "noise".

## Use

- **Left-click** the tray icon: toggle on/off. Green with bars = ON.
- **Right-click**: start with Windows, open the folder (config + log), exit.
- Launching the exe again while it is running toggles it too, so you can bind a hotkey or a Stream Deck button to the exe.

## Settings (`config.txt`)

| key | default | meaning |
|---|---|---|
| `music_gain` | `0.5` | level of the PC sound in the mic (1.0 = as loud as it plays) |
| `mic_gain` | `1.0` | level of your voice |
| `exclude_process` | `Discord.exe` | this app's audio is never sent into the mic (use `Zoom.exe`, `ts3client_win64.exe`, ... for other voice apps) |
| `cable_playback` / `cable_recording` | `CABLE Input` / `CABLE Output` | names of the virtual cable endpoints |
| `verbose` | `0` | `1` logs mic/music levels every 5 s to `music-to-mic.log` |

## How it works

- Captures your current default microphone (WASAPI shared mode with auto-conversion) and everything the PC plays except the excluded process tree (`ActivateAudioInterfaceAsync` with `VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK`, exclude mode).
- Mixes both in float and renders the result to `CABLE Input`; `CABLE Output` is the matching virtual microphone.
- Switches the default recording device (console, multimedia and communications roles) to `CABLE Output` while ON, using the well-known `IPolicyConfig` interface, and restores the previous mic when OFF, on exit, on shutdown, and on next start if it finds the cable was left as default.
- If the excluded app restarts, the loopback stream is reopened with the new process id within 2 seconds.

## Build

Needs Visual Studio 2022 (Community is fine) with the C++ workload. Run `build.cmd`; it produces `MusicToMic.exe`. No third-party libraries.

## Licence

MIT. VB-CABLE is a separate product with its own licence.
