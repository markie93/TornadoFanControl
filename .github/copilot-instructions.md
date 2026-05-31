# TornadoFanControl — Copilot Instructions

## Project Overview
Custom fan controller for the **Acer Predator PHN16-72** laptop. Consists of a C++ Windows backend that controls fans via the Embedded Controller (EC), and a Python tkinter/matplotlib GUI for editing fan curves.

Supported model check is enforced at runtime via BIOS registry keys (`HARDWARE\DESCRIPTION\System\BIOS`).

---

## Architecture

```
TornadoFanControl/
├── CPP/FanControl/FanControl/FanControl.cpp   # C++ backend (WinMain, runs as Windows service)
├── ui.py                                       # Python GUI (tkinter + matplotlib)
├── ui.spec                                     # PyInstaller spec for GUI packaging
└── README.md
```

### IPC: Named Shared Memory
The backend and GUI communicate through a Windows named shared memory region called `"MySharedMemory"` of size `sizeof(SharedData)`.

- **C++ side**: Created with `CreateFileMapping` / `MapViewOfFile`, fields are `std::atomic<T>`.
- **Python side**: Opened with `mmap.mmap(-1, ..., tagname="MySharedMemory")` and read/written via `ctypes.Structure`.

The `SharedData` struct layout **must stay identical** between both languages. Any change to field order or type in one side must be mirrored in the other.

#### SharedData field layout (in order)
| Field | Type | Description |
|---|---|---|
| `cpuTemp` | double / c_double | Live CPU temperature (°C) |
| `gpuTemp` | double / c_double | Live GPU temperature (°C) |
| `cpuFanSpeed` | int / c_int | Current CPU fan speed (%) |
| `gpuFanSpeed` | int / c_int | Current GPU fan speed (%) |
| `cpu_hysteresis` | int / c_int | CPU hysteresis deadband (°C) |
| `gpu_hysteresis` | int / c_int | GPU hysteresis deadband (°C) |
| `cpu_temp_point_2/3/4` | int × 3 | CPU curve: interior temperature setpoints |
| `gpu_temp_point_2/3/4` | int × 3 | GPU curve: interior temperature setpoints |
| `cpu_fan_point_1..5` | int × 5 | CPU curve: fan % at each of the 5 points |
| `gpu_fan_point_1..5` | int × 5 | GPU curve: fan % at each of the 5 points |
| `write_sync` | bool / c_bool | Handshake flag: GUI sets `true` to signal backend to reload config |
| `fan_ctrl_mode` | int / c_int | 0 = curve mode, 1 = PID mode |
| `cpu_target_temp` | int / c_int | PID target CPU temp (°C) |
| `gpu_target_temp` | int / c_int | PID target GPU temp (°C) |
| `cpu_min_fan_pid` | int / c_int | Minimum CPU fan % in PID mode |
| `gpu_min_fan_pid` | int / c_int | Minimum GPU fan % in PID mode |

Fan curve points 1 and 5 have **fixed temperatures** (0°C and 100°C). Only points 2, 3, 4 have editable temperatures.

---

## C++ Backend (`FanControl.cpp`)

### Entry Point
`WinMain` (no console window). Runs silently; errors shown via `MessageBoxA`.

### Key Classes
- **`FanController`** — piecewise-linear interpolation between 5 (temp, fan%) setpoints with hysteresis. Methods:
  - `update(double temp)` → returns fan speed %; only updates when temp rises or drops by more than hysteresis.
  - `syncToSharedData(SharedData*, bool isCpu)` — writes current curve to shared memory.
  - `syncFromSharedData(SharedData*, bool isCpu)` — reloads curve from shared memory.
- **`PidFanController`** — PID controller for temperature-targeting mode. Methods:
  - `update(double currentTemp, double targetTemp, int minFan)` → clamped fan%; derivative on measurement, anti-windup integral clamping.
  - `reset()` — clears integral and prev_error; called on every `write_sync` reload.
  - Hardcoded gains: `Kp=2.0`, `Ki=0.05`, `Kd=1.0` (edit in `PidFanController` class).
- **`LowPassFilter`** — single-pole IIR filter (`alpha=0.1`) for smoothing raw EC temperature readings.

### EC (Embedded Controller) Access
Uses `inpoutx64.dll` (`Inp32` / `Out32`) for direct I/O port access.

| Symbol | Port/Register | Purpose |
|---|---|---|
| `EC_DATA_PORT` | `0x62` | EC data read/write |
| `EC_COMMAND_PORT` | `0x66` | EC command write / status read |
| `EC_READ_CMD` | `0x80` | EC read command byte |
| `EC_WRITE_CMD` | `0x81` | EC write command byte |
| `cpu_temp_reg_1` | `176` | CPU temperature register |
| `gpu_temp_reg_1` | `180` | GPU temperature register |
| `cpu_fan_speed_reg` | `55` | CPU fan speed register |
| `gpu_fan_speed_reg` | `58` | GPU fan speed register |
| `cpu_fan_mode_reg` | `34` | CPU fan mode (auto `0x04` / manual `0x0c`) |
| `gpu_fan_mode_reg` | `33` | GPU fan mode (auto `0x10` / manual `0x30`) |

EC access requires polling the IBF (input buffer full) bit on `EC_COMMAND_PORT` before each write.

### Fan Coupling
In the main loop, CPU and GPU fan speeds are coupled: they are **not allowed to differ by more than 10%**. If one exceeds the other by >10%, the lower one is raised to within 10 points.

### Persistence
Settings are saved/loaded as a raw binary dump (field-by-field, no header) to:
`%APPDATA%\FanControl\fanctrl_settings.dat`

Fallback: same directory as the executable.

### Main Loop
- Runs at ~1-second intervals (10 × 100 ms sleep with `g_running` check).
- Reads temps via EC, applies low-pass filter, dispatches to `FanController` (curve mode) or `PidFanController` (PID mode) based on `fan_ctrl_mode`.
- Writes to EC only on change.
- After each sleep cycle, checks `write_sync`; if set, calls `syncFromSharedData` + `PidFanController::reset()` for both controllers and persists settings.

### Shutdown
`SetConsoleCtrlHandler` / `ConsoleHandler` handles `CTRL_C`, `CTRL_CLOSE`, shutdown signals — saves settings, unmaps shared memory, closes handle.

---

## Python GUI (`ui.py`)

### Key Class: `FanControlGUI`
Built with `tkinter` (ttk, dark `clam` theme) hosting a `ttk.Notebook` with two tabs:

**Tab 1 — Fan Curves:**
- Control bar: Apply Changes, Reset, CPU/GPU hysteresis spinboxes, Mode radio (Curve / PID).
- **Curve Mode**: embedded matplotlib figure (dark style) with two subplots. Drag green edit points to reshape curves; red dot = live position.
- **PID Mode**: settings panel with CPU/GPU target temperature and minimum fan speed spinboxes. Swapped in/out via `pack`/`pack_forget` without recreating widgets.

**Tab 2 — Live Monitor:**
- Two subplots (CPU / GPU) each with dual y-axes: temperature (°C, left) and fan speed (%, right).
- 10-second rolling history via `collections.deque(maxlen=20)` sampled every 500 ms.
- Only redraws when this tab is active.

**Update loop**: timer-based (`root.after`), not `FuncAnimation`:
- Idle: 100 ms interval.
- Dragging a point: 16 ms interval (~60 FPS).

**Interaction**:
- Shared memory is read every 500 ms to refresh live temperature/speed titles and the current-point markers (red dots).
- Drag green edit points to reshape the curve; points 0 and 4 (fixed at 0°C / 100°C) are not draggable.
- **Apply Changes**: sets `write_sync = True` and writes full `SharedData` to shared memory. The C++ backend picks this up within ~1 s.
- **Reset**: restores a default curve locally; requires Apply to take effect in the backend.
- **Hysteresis spinboxes**: 0–10 °C, updates `edit_data` immediately.
- **Window close**: saves current state by setting `write_sync = True` and writing shared memory, then calls `root.quit()`.

### Shared Memory Access
```python
shm = mmap.mmap(-1, ctypes.sizeof(SharedData), tagname="MySharedMemory", access=mmap.ACCESS_WRITE)
```
The backend must already be running and have created the mapping before the GUI opens (otherwise `mmap.mmap` raises and the GUI exits).

---

## Build & Packaging

### C++ Backend
- **Solution**: `CPP/FanControl/FanControl.sln`
- **Platform**: x64 only.
- **Output**: `CPP/FanControl/x64/Debug/FanControl.exe`
- **Dependency**: `inpoutx64.dll` must be in the same directory as the executable at runtime.

### Python GUI
- Packaged with **PyInstaller** using `ui.spec`.
- Output installer: `dist\TornadoFanControllerInstaller.exe`.

---

## Conventions & Constraints

- The C++ project uses `WinMain` (no console); never add `AllocConsole` or `std::cout` to the main process.
- `inpoutx64.dll` requires the process to run with **administrator privileges** to access I/O ports.
- The system check (`checkSystem()`) must remain; the EC register map is model-specific.
- Do not change the order or types of fields in `SharedData` without updating both `FanControl.cpp` and `ui.py`.
- Fan speed values throughout are **percentages (0–100)**, not RPM.
- Temperature values throughout are **degrees Celsius**.
