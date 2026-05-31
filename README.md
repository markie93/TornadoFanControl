# TornadoFanControl

Custom fan controller for the **Acer Predator PHN16-72** laptop.

## Supported Models
- Acer Predator PHN16-72

## About

A lightweight C++ Windows backend controls the fans directly via the laptop's Embedded Controller (EC), keeping CPU overhead minimal. A Python GUI (tkinter + matplotlib) lets you edit fan curves or switch to PID temperature-targeting mode.

The backend runs as a Windows startup program and communicates with the GUI through a named shared memory region (`MySharedMemory`).

## Features

- **Curve mode** — piecewise-linear fan curves with configurable hysteresis for both CPU and GPU fans.
- **PID mode** — target a fixed CPU/GPU temperature; the controller adjusts fan speed automatically.
- **Live monitor** — rolling 10-second graph of temperature and fan speed for both CPU and GPU.
- **Fan coupling** — CPU and GPU fans are never allowed to diverge by more than 10 percentage points.
- Compatible with ACER PredatorSense and mode switching; TornadoFanControl simply overrides the fan speed based on current temperatures.

## Architecture

```
TornadoFanControl/
├── CPP/FanControl/FanControl/FanControl.cpp   # C++ backend (WinMain, Windows startup)
├── ui.py                                       # Python GUI (tkinter + matplotlib)
├── ui.spec                                     # PyInstaller spec for GUI packaging
└── README.md
```

### IPC
Backend and GUI share a `SharedData` struct mapped into a Windows named shared memory region. The GUI writes curve/PID settings and sets `write_sync = True`; the backend picks up the change within ~1 second.

## Requirements

- Windows 10/11 x64
- Administrator privileges (required for direct EC I/O port access)
- `inpoutx64.dll` in the same directory as `FanControl.exe`

## Installation

1. Run `dist\TornadoFanControllerInstaller.exe` and reboot.
2. The backend starts automatically with Windows.
3. Launch the GUI to adjust fan curves or switch to PID mode.

## Building

### C++ Backend
Open `CPP/FanControl/FanControl.sln` in Visual Studio and build the **x64 Debug** (or Release) configuration.  
Output: `CPP/FanControl/x64/Debug/FanControl.exe`

### Python GUI
```
pyinstaller ui.spec
```
Output installer: `dist\TornadoFanControllerInstaller.exe`

## Notes

- The system model check (`checkSystem()`) is enforced at runtime via BIOS registry keys — do not remove it; the EC register map is model-specific.
- Fan speed values are **percentages (0–100)**, not RPM.
- Temperature values are **degrees Celsius**.
