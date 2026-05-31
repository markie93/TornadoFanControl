import collections
import ctypes
import mmap
import time
import tkinter as tk
from tkinter import ttk

import matplotlib
matplotlib.use("TkAgg")
import matplotlib.pyplot as plt
from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg

# ---------------------------------------------------------------------------
# Shared memory layout -- must mirror SharedData in FanControl.cpp exactly
# ---------------------------------------------------------------------------

class SharedData(ctypes.Structure):
    _fields_ = [
        ("cpuTemp",        ctypes.c_double),
        ("gpuTemp",        ctypes.c_double),
        ("cpuFanSpeed",    ctypes.c_int),
        ("gpuFanSpeed",    ctypes.c_int),

        ("cpu_hysteresis", ctypes.c_int),
        ("gpu_hysteresis", ctypes.c_int),

        ("cpu_temp_point_2", ctypes.c_int),
        ("cpu_temp_point_3", ctypes.c_int),
        ("cpu_temp_point_4", ctypes.c_int),

        ("gpu_temp_point_2", ctypes.c_int),
        ("gpu_temp_point_3", ctypes.c_int),
        ("gpu_temp_point_4", ctypes.c_int),

        ("cpu_fan_point_1", ctypes.c_int),
        ("cpu_fan_point_2", ctypes.c_int),
        ("cpu_fan_point_3", ctypes.c_int),
        ("cpu_fan_point_4", ctypes.c_int),
        ("cpu_fan_point_5", ctypes.c_int),

        ("gpu_fan_point_1", ctypes.c_int),
        ("gpu_fan_point_2", ctypes.c_int),
        ("gpu_fan_point_3", ctypes.c_int),
        ("gpu_fan_point_4", ctypes.c_int),
        ("gpu_fan_point_5", ctypes.c_int),

        ("write_sync", ctypes.c_bool),

        # PID mode fields (appended for backward compat)
        ("fan_ctrl_mode",   ctypes.c_int),  # 0 = curve, 1 = PID
        ("cpu_target_temp", ctypes.c_int),
        ("gpu_target_temp", ctypes.c_int),
        ("cpu_min_fan_pid", ctypes.c_int),
        ("gpu_min_fan_pid", ctypes.c_int),
    ]

try:
    shm = mmap.mmap(-1, ctypes.sizeof(SharedData), tagname="MySharedMemory",
                    access=mmap.ACCESS_WRITE)
except Exception as e:
    import tkinter.messagebox as mb
    _r = tk.Tk(); _r.withdraw()
    mb.showerror("TornadoFanControl",
                 f"Could not open shared memory.\nIs FanControl.exe running?\n\n{e}")
    raise SystemExit(1)


def read_shared_data() -> SharedData:
    shm.seek(0)
    buf = shm.read(ctypes.sizeof(SharedData))
    return SharedData.from_buffer_copy(buf)

def write_shared_data(data):
    shm.seek(0)
    shm.write(bytes(data))


# ---------------------------------------------------------------------------
# Colour palette
# ---------------------------------------------------------------------------
BG        = "#1e1e2e"
BG_FRAME  = "#2a2a3e"
FG        = "#cdd6f4"
ACCENT    = "#89b4fa"
ACCENT2   = "#a6e3a1"
RED       = "#f38ba8"
ORANGE    = "#fab387"
CYAN      = "#89dceb"
MUTED     = "#6c7086"
BUTTON_BG = "#313244"

# ---------------------------------------------------------------------------
# Main GUI class
# ---------------------------------------------------------------------------

class FanControlGUI:
    HISTORY_SAMPLES = 20   # 20 x 500 ms = 10 s

    def __init__(self, root: tk.Tk):
        self.root = root
        self.root.title("Tornado Fan Control")
        self.root.configure(bg=BG)
        self.root.minsize(900, 560)
        self.root.protocol("WM_DELETE_WINDOW", self.on_closing)

        self._apply_theme()

        # State
        self.edit_data = None
        self.dragging = None
        self.last_data_update = 0.0
        self.running = True
        self._after_id = None

        # History buffer  (elapsed_s, cpu_temp, gpu_temp, cpu_fan, gpu_fan)
        self._history = collections.deque(maxlen=self.HISTORY_SAMPLES)
        self._t0 = None

        self._build_ui()
        self.update_ui()

    # ------------------------------------------------------------------
    # Theme
    # ------------------------------------------------------------------

    def _apply_theme(self):
        style = ttk.Style(self.root)
        style.theme_use("clam")

        style.configure(".",
                        background=BG, foreground=FG,
                        fieldbackground=BG_FRAME,
                        troughcolor=BG_FRAME,
                        bordercolor=MUTED,
                        darkcolor=BG, lightcolor=BG_FRAME,
                        insertcolor=FG,
                        font=("Segoe UI", 10))

        style.configure("TFrame",    background=BG)
        style.configure("TLabel",    background=BG, foreground=FG)
        style.configure("TNotebook", background=BG, borderwidth=0)
        style.configure("TNotebook.Tab",
                        background=BUTTON_BG, foreground=FG,
                        padding=[14, 6],
                        font=("Segoe UI", 10, "bold"))
        style.map("TNotebook.Tab",
                  background=[("selected", ACCENT)],
                  foreground=[("selected", BG)])

        style.configure("TButton",
                        background=BUTTON_BG, foreground=FG,
                        relief="flat", padding=[10, 5],
                        font=("Segoe UI", 10))
        style.map("TButton",
                  background=[("active", ACCENT)],
                  foreground=[("active", BG)])

        style.configure("Accent.TButton",
                        background=ACCENT, foreground=BG,
                        font=("Segoe UI", 10, "bold"))
        style.map("Accent.TButton",
                  background=[("active", ACCENT2)])

        style.configure("TSpinbox",
                        background=BG_FRAME, foreground=FG,
                        fieldbackground=BG_FRAME,
                        arrowcolor=FG, bordercolor=MUTED,
                        insertcolor=FG)
        style.configure("TRadiobutton",
                        background=BG, foreground=FG,
                        indicatorcolor=ACCENT,
                        font=("Segoe UI", 10))
        style.map("TRadiobutton",
                  background=[("active", BG)],
                  foreground=[("active", ACCENT)])

        style.configure("TLabelframe",
                        background=BG_FRAME, foreground=FG,
                        bordercolor=MUTED)
        style.configure("TLabelframe.Label",
                        background=BG_FRAME, foreground=ACCENT,
                        font=("Segoe UI", 10, "bold"))

        plt.style.use("dark_background")
        matplotlib.rcParams.update({
            "axes.facecolor":   BG_FRAME,
            "figure.facecolor": BG,
            "axes.edgecolor":   MUTED,
            "axes.labelcolor":  FG,
            "xtick.color":      FG,
            "ytick.color":      FG,
            "text.color":       FG,
            "grid.color":       MUTED,
            "grid.alpha":       0.4,
            "legend.facecolor": BG_FRAME,
            "legend.edgecolor": MUTED,
        })

    # ------------------------------------------------------------------
    # Layout
    # ------------------------------------------------------------------

    def _build_ui(self):
        self.notebook = ttk.Notebook(self.root)
        self.notebook.pack(fill=tk.BOTH, expand=True, padx=10, pady=10)
        self._build_curves_tab()
        self._build_monitor_tab()

    # ---- Curves tab ---------------------------------------------------

    def _build_curves_tab(self):
        tab = ttk.Frame(self.notebook)
        self.notebook.add(tab, text="  Fan Curves  ")

        # Top control bar
        ctrl = ttk.Frame(tab)
        ctrl.pack(fill=tk.X, pady=(0, 8))

        ttk.Button(ctrl, text="Apply Changes", style="Accent.TButton",
                   command=self.apply_changes).pack(side=tk.LEFT)
        ttk.Button(ctrl, text="Reset",
                   command=self.reset_curve).pack(side=tk.LEFT, padx=(8, 0))

        ttk.Separator(ctrl, orient=tk.VERTICAL).pack(side=tk.LEFT, fill=tk.Y, padx=12, pady=3)

        ttk.Label(ctrl, text="CPU Hyst:").pack(side=tk.LEFT, padx=(0, 4))
        self.cpu_hysteresis_var = tk.IntVar(value=3)
        ttk.Spinbox(ctrl, from_=0, to=10, width=4,
                    textvariable=self.cpu_hysteresis_var,
                    command=self.on_hysteresis_change).pack(side=tk.LEFT)
        ttk.Label(ctrl, text="\u00b0C").pack(side=tk.LEFT, padx=(2, 12))

        ttk.Label(ctrl, text="GPU Hyst:").pack(side=tk.LEFT, padx=(0, 4))
        self.gpu_hysteresis_var = tk.IntVar(value=3)
        ttk.Spinbox(ctrl, from_=0, to=10, width=4,
                    textvariable=self.gpu_hysteresis_var,
                    command=self.on_hysteresis_change).pack(side=tk.LEFT)
        ttk.Label(ctrl, text="\u00b0C").pack(side=tk.LEFT, padx=(2, 12))

        ttk.Separator(ctrl, orient=tk.VERTICAL).pack(side=tk.LEFT, fill=tk.Y, padx=12, pady=3)

        ttk.Label(ctrl, text="Mode:", font=("Segoe UI", 10, "bold")).pack(side=tk.LEFT, padx=(0, 6))
        self.mode_var = tk.IntVar(value=0)
        ttk.Radiobutton(ctrl, text="Curve", variable=self.mode_var, value=0,
                        command=self._on_mode_change).pack(side=tk.LEFT)
        ttk.Radiobutton(ctrl, text="PID",   variable=self.mode_var, value=1,
                        command=self._on_mode_change).pack(side=tk.LEFT, padx=(8, 0))

        # Panel container (swaps curve figure <-> PID settings)
        self.panel_container = ttk.Frame(tab)
        self.panel_container.pack(fill=tk.BOTH, expand=True)

        self._build_curve_panel()
        self._build_pid_panel()
        self._show_curve_panel()

    def _build_curve_panel(self):
        self.curve_panel = ttk.Frame(self.panel_container)

        fig, (self.ax1, self.ax2) = plt.subplots(1, 2, figsize=(14, 5.5))
        fig.suptitle("Fan Control Curves  \u2013  drag green points to edit",
                     color=FG, fontsize=11)
        fig.tight_layout(rect=[0, 0, 1, 0.95])

        for ax, title in [(self.ax1, "CPU Fan Curve"), (self.ax2, "GPU Fan Curve")]:
            ax.set_title(title, color=ACCENT, fontsize=10, fontweight="bold")
            ax.set_xlabel("Temperature (\u00b0C)")
            ax.set_ylabel("Fan Speed (%)")
            ax.set_xlim(0, 100)
            ax.set_ylim(0, 105)
            ax.grid(True)

        self.cpu_line,        = self.ax1.plot([], [], color=CYAN,    lw=2.5, label="Curve")
        self.cpu_point,       = self.ax1.plot([], [], "o", color=RED, ms=9,  label="Current", zorder=5)
        self.cpu_edit_points, = self.ax1.plot([], [], "o", color=ACCENT2, ms=11,
                                              picker=True, pickradius=8, label="Edit", zorder=6)

        self.gpu_line,        = self.ax2.plot([], [], color=ACCENT,  lw=2.5, label="Curve")
        self.gpu_point,       = self.ax2.plot([], [], "o", color=RED, ms=9,  label="Current", zorder=5)
        self.gpu_edit_points, = self.ax2.plot([], [], "o", color=ORANGE, ms=11,
                                              picker=True, pickradius=8, label="Edit", zorder=6)

        self.ax1.legend(facecolor=BG_FRAME, edgecolor=MUTED)
        self.ax2.legend(facecolor=BG_FRAME, edgecolor=MUTED)

        self.canvas = FigureCanvasTkAgg(fig, self.curve_panel)
        self.canvas.get_tk_widget().pack(fill=tk.BOTH, expand=True)

        self.canvas.mpl_connect("button_press_event",   self.on_press)
        self.canvas.mpl_connect("motion_notify_event",  self.on_motion)
        self.canvas.mpl_connect("button_release_event", self.on_release)

    def _build_pid_panel(self):
        self.pid_panel = ttk.Frame(self.panel_container)

        inner = ttk.Frame(self.pid_panel)
        inner.place(relx=0.5, rely=0.5, anchor=tk.CENTER)

        ttk.Label(inner, text="PID Fan Control Settings",
                  font=("Segoe UI", 13, "bold"),
                  foreground=ACCENT).grid(row=0, column=0, columnspan=2, pady=(0, 20))

        for col, (label, prefix, default_target) in enumerate(
                [("CPU", "cpu", 70), ("GPU", "gpu", 65)]):
            frame = ttk.LabelFrame(inner, text=f"  {label} PID  ", padding=16)
            frame.grid(row=1, column=col, padx=24, sticky="nsew")

            ttk.Label(frame, text="Target Temperature (\u00b0C):").grid(
                row=0, column=0, sticky="w", pady=6)
            tvar = tk.IntVar(value=default_target)
            setattr(self, f"{prefix}_target_var", tvar)
            ttk.Spinbox(frame, from_=30, to=90, width=6,
                        textvariable=tvar).grid(row=0, column=1, padx=(10, 0))

            ttk.Label(frame, text="Minimum Fan Speed (%):").grid(
                row=1, column=0, sticky="w", pady=6)
            mvar = tk.IntVar(value=10)
            setattr(self, f"{prefix}_min_pid_var", mvar)
            ttk.Spinbox(frame, from_=0, to=50, width=6,
                        textvariable=mvar).grid(row=1, column=1, padx=(10, 0))

        ttk.Label(inner,
                  text="PID gains: Kp=2.0  Ki=0.05  Kd=1.0\n"
                       "(edit in FanControl.cpp \u2192 PidFanController)",
                  foreground=MUTED, font=("Segoe UI", 9)).grid(
                      row=2, column=0, columnspan=2, pady=(18, 0))

    def _show_curve_panel(self):
        self.pid_panel.pack_forget()
        self.curve_panel.pack(fill=tk.BOTH, expand=True)

    def _show_pid_panel(self):
        self.curve_panel.pack_forget()
        self.pid_panel.pack(fill=tk.BOTH, expand=True)

    def _on_mode_change(self):
        if self.mode_var.get() == 1:
            self._show_pid_panel()
        else:
            self._show_curve_panel()
        if self.edit_data is not None:
            self.edit_data.fan_ctrl_mode = self.mode_var.get()

    # ---- Live Monitor tab ---------------------------------------------

    def _build_monitor_tab(self):
        tab = ttk.Frame(self.notebook)
        self.notebook.add(tab, text="  Live Monitor  ")

        fig, (self.mon_ax1, self.mon_ax2) = plt.subplots(1, 2, figsize=(14, 5.5))
        fig.suptitle("10-Second Rolling History", color=FG, fontsize=11)
        fig.tight_layout(rect=[0, 0, 1, 0.95])

        # CPU subplot -- dual y-axis
        self.mon_ax1.set_title("CPU", color=ACCENT, fontsize=10, fontweight="bold")
        self.mon_ax1.set_xlabel("Time (s)")
        self.mon_ax1.set_ylabel("Temperature (\u00b0C)", color=RED)
        self.mon_ax1.set_xlim(0, 10)
        self.mon_ax1.set_ylim(0, 110)
        self.mon_ax1.tick_params(axis="y", labelcolor=RED)
        self.mon_ax1.grid(True)

        self.mon_ax1r = self.mon_ax1.twinx()
        self.mon_ax1r.set_ylabel("Fan Speed (%)", color=ORANGE)
        self.mon_ax1r.set_ylim(0, 110)
        self.mon_ax1r.tick_params(axis="y", labelcolor=ORANGE)

        self.mon_cpu_temp_line, = self.mon_ax1.plot( [], [], color=RED,    lw=2, label="CPU Temp")
        self.mon_cpu_fan_line,  = self.mon_ax1r.plot([], [], color=ORANGE, lw=2, label="CPU Fan %")
        lines1  = [self.mon_cpu_temp_line, self.mon_cpu_fan_line]
        self.mon_ax1.legend(lines1, [l.get_label() for l in lines1],
                            facecolor=BG_FRAME, edgecolor=MUTED, loc="upper left")

        # GPU subplot -- dual y-axis
        self.mon_ax2.set_title("GPU", color=ACCENT, fontsize=10, fontweight="bold")
        self.mon_ax2.set_xlabel("Time (s)")
        self.mon_ax2.set_ylabel("Temperature (\u00b0C)", color=ACCENT)
        self.mon_ax2.set_xlim(0, 10)
        self.mon_ax2.set_ylim(0, 110)
        self.mon_ax2.tick_params(axis="y", labelcolor=ACCENT)
        self.mon_ax2.grid(True)

        self.mon_ax2r = self.mon_ax2.twinx()
        self.mon_ax2r.set_ylabel("Fan Speed (%)", color=CYAN)
        self.mon_ax2r.set_ylim(0, 110)
        self.mon_ax2r.tick_params(axis="y", labelcolor=CYAN)

        self.mon_gpu_temp_line, = self.mon_ax2.plot( [], [], color=ACCENT, lw=2, label="GPU Temp")
        self.mon_gpu_fan_line,  = self.mon_ax2r.plot([], [], color=CYAN,   lw=2, label="GPU Fan %")
        lines2 = [self.mon_gpu_temp_line, self.mon_gpu_fan_line]
        self.mon_ax2.legend(lines2, [l.get_label() for l in lines2],
                            facecolor=BG_FRAME, edgecolor=MUTED, loc="upper left")

        self.monitor_canvas = FigureCanvasTkAgg(fig, tab)
        self.monitor_canvas.get_tk_widget().pack(fill=tk.BOTH, expand=True)

    # ------------------------------------------------------------------
    # Update loop
    # ------------------------------------------------------------------

    def update_ui(self):
        if not self.running or not self.root.winfo_exists():
            return

        current_time = time.time()

        try:
            if current_time - self.last_data_update > 0.5 or self.edit_data is None:
                data = read_shared_data()

                if self.edit_data is None:
                    self.edit_data = data
                    self.cpu_hysteresis_var.set(data.cpu_hysteresis)
                    self.gpu_hysteresis_var.set(data.gpu_hysteresis)
                    self.mode_var.set(data.fan_ctrl_mode)
                    self.cpu_target_var.set(data.cpu_target_temp)
                    self.gpu_target_var.set(data.gpu_target_temp)
                    self.cpu_min_pid_var.set(data.cpu_min_fan_pid)
                    self.gpu_min_pid_var.set(data.gpu_min_fan_pid)
                    if data.fan_ctrl_mode == 1:
                        self._show_pid_panel()

                mode_tag = "[PID]" if self.edit_data.fan_ctrl_mode == 1 else "[Curve]"
                self.ax1.set_title(
                    f"CPU {mode_tag}  {data.cpuTemp:.1f}\u00b0C  {data.cpuFanSpeed}%  "
                    f"(Hyst: {self.edit_data.cpu_hysteresis}\u00b0C)",
                    color=ACCENT, fontsize=9)
                self.ax2.set_title(
                    f"GPU {mode_tag}  {data.gpuTemp:.1f}\u00b0C  {data.gpuFanSpeed}%  "
                    f"(Hyst: {self.edit_data.gpu_hysteresis}\u00b0C)",
                    color=ACCENT, fontsize=9)

                self.cpu_point.set_data([data.cpuTemp], [data.cpuFanSpeed])
                self.gpu_point.set_data([data.gpuTemp], [data.gpuFanSpeed])

                # History buffer
                if self._t0 is None:
                    self._t0 = current_time
                elapsed = current_time - self._t0
                self._history.append((elapsed, data.cpuTemp, data.gpuTemp,
                                       data.cpuFanSpeed, data.gpuFanSpeed))

                self.last_data_update = current_time

                if self.notebook.index("current") == 1:
                    self._update_monitor()

            self._update_curves()

        except Exception as e:
            print(f"Error in update_ui: {e}")
            return

        interval = 16 if self.dragging is not None else 100
        self._after_id = self.root.after(interval, self.update_ui)

    def _update_curves(self):
        if self.edit_data is None:
            return
        cpu_temps = [0, self.edit_data.cpu_temp_point_2, self.edit_data.cpu_temp_point_3,
                     self.edit_data.cpu_temp_point_4, 100]
        cpu_fans  = [self.edit_data.cpu_fan_point_1, self.edit_data.cpu_fan_point_2,
                     self.edit_data.cpu_fan_point_3, self.edit_data.cpu_fan_point_4,
                     self.edit_data.cpu_fan_point_5]
        gpu_temps = [0, self.edit_data.gpu_temp_point_2, self.edit_data.gpu_temp_point_3,
                     self.edit_data.gpu_temp_point_4, 100]
        gpu_fans  = [self.edit_data.gpu_fan_point_1, self.edit_data.gpu_fan_point_2,
                     self.edit_data.gpu_fan_point_3, self.edit_data.gpu_fan_point_4,
                     self.edit_data.gpu_fan_point_5]

        self.cpu_line.set_data(cpu_temps, cpu_fans)
        self.cpu_edit_points.set_data(cpu_temps, cpu_fans)
        self.gpu_line.set_data(gpu_temps, gpu_fans)
        self.gpu_edit_points.set_data(gpu_temps, gpu_fans)
        self.canvas.draw_idle()

    def _update_monitor(self):
        if not self._history:
            return
        h = self._history
        last_t = h[-1][0]
        xs = [10.0 - (last_t - s[0]) for s in h]
        xs = [max(0.0, x) for x in xs]

        cpu_temps = [s[1] for s in h]
        gpu_temps = [s[2] for s in h]
        cpu_fans  = [s[3] for s in h]
        gpu_fans  = [s[4] for s in h]

        self.mon_cpu_temp_line.set_data(xs, cpu_temps)
        self.mon_cpu_fan_line.set_data(xs, cpu_fans)
        self.mon_gpu_temp_line.set_data(xs, gpu_temps)
        self.mon_gpu_fan_line.set_data(xs, gpu_fans)
        self.monitor_canvas.draw_idle()

    # ------------------------------------------------------------------
    # Curve drag interaction
    # ------------------------------------------------------------------

    def on_press(self, event):
        if event.inaxes not in (self.ax1, self.ax2) or self.edit_data is None:
            return
        is_cpu = event.inaxes == self.ax1
        temps, fans = self._get_curve_points(is_cpu)
        closest, min_dist = None, float("inf")
        for i in range(1, 4):
            d = ((temps[i] - event.xdata) ** 2 + (fans[i] - event.ydata) ** 2) ** 0.5
            if d < min_dist and d < 6:
                min_dist, closest = d, i
        if closest is not None:
            self.dragging = ("cpu" if is_cpu else "gpu", closest)

    def on_motion(self, event):
        if self.dragging is None or event.inaxes not in (self.ax1, self.ax2):
            return
        curve, idx = self.dragging
        nx = int(max(0, min(100, event.xdata)))
        ny = int(max(0, min(100, event.ydata)))
        self._set_curve_point(curve, idx, nx, ny)

    def on_release(self, _event):
        self.dragging = None

    def _get_curve_points(self, is_cpu):
        d = self.edit_data
        if is_cpu:
            return ([0, d.cpu_temp_point_2, d.cpu_temp_point_3, d.cpu_temp_point_4, 100],
                    [d.cpu_fan_point_1, d.cpu_fan_point_2, d.cpu_fan_point_3,
                     d.cpu_fan_point_4, d.cpu_fan_point_5])
        return ([0, d.gpu_temp_point_2, d.gpu_temp_point_3, d.gpu_temp_point_4, 100],
                [d.gpu_fan_point_1, d.gpu_fan_point_2, d.gpu_fan_point_3,
                 d.gpu_fan_point_4, d.gpu_fan_point_5])

    def _set_curve_point(self, curve, idx, temp, fan):
        d = self.edit_data
        if curve == "cpu":
            if idx == 1: d.cpu_temp_point_2 = temp; d.cpu_fan_point_2 = fan
            elif idx == 2: d.cpu_temp_point_3 = temp; d.cpu_fan_point_3 = fan
            elif idx == 3: d.cpu_temp_point_4 = temp; d.cpu_fan_point_4 = fan
        else:
            if idx == 1: d.gpu_temp_point_2 = temp; d.gpu_fan_point_2 = fan
            elif idx == 2: d.gpu_temp_point_3 = temp; d.gpu_fan_point_3 = fan
            elif idx == 3: d.gpu_temp_point_4 = temp; d.gpu_fan_point_4 = fan

    # ------------------------------------------------------------------
    # Controls
    # ------------------------------------------------------------------

    def on_hysteresis_change(self):
        if self.edit_data is not None:
            self.edit_data.cpu_hysteresis = self.cpu_hysteresis_var.get()
            self.edit_data.gpu_hysteresis = self.gpu_hysteresis_var.get()

    def apply_changes(self):
        if self.edit_data is None:
            return
        self.edit_data.cpu_hysteresis  = self.cpu_hysteresis_var.get()
        self.edit_data.gpu_hysteresis  = self.gpu_hysteresis_var.get()
        self.edit_data.fan_ctrl_mode   = self.mode_var.get()
        self.edit_data.cpu_target_temp = self.cpu_target_var.get()
        self.edit_data.gpu_target_temp = self.gpu_target_var.get()
        self.edit_data.cpu_min_fan_pid = self.cpu_min_pid_var.get()
        self.edit_data.gpu_min_fan_pid = self.gpu_min_pid_var.get()
        self.edit_data.write_sync = True
        write_shared_data(self.edit_data)

    def reset_curve(self):
        if self.edit_data is None:
            return
        self.edit_data.cpu_temp_point_2 = 60
        self.edit_data.cpu_temp_point_3 = 80
        self.edit_data.cpu_temp_point_4 = 95
        self.edit_data.gpu_temp_point_2 = 50
        self.edit_data.gpu_temp_point_3 = 70
        self.edit_data.gpu_temp_point_4 = 85
        self.edit_data.cpu_fan_point_1  = 0
        self.edit_data.cpu_fan_point_2  = 5
        self.edit_data.cpu_fan_point_3  = 10
        self.edit_data.cpu_fan_point_4  = 30
        self.edit_data.cpu_fan_point_5  = 100
        self.edit_data.gpu_fan_point_1  = 0
        self.edit_data.gpu_fan_point_2  = 5
        self.edit_data.gpu_fan_point_3  = 10
        self.edit_data.gpu_fan_point_4  = 30
        self.edit_data.gpu_fan_point_5  = 100
        self.edit_data.cpu_hysteresis   = 3
        self.edit_data.gpu_hysteresis   = 3
        self.cpu_hysteresis_var.set(3)
        self.gpu_hysteresis_var.set(3)

    def on_closing(self):
        self.running = False
        if self._after_id is not None:
            self.root.after_cancel(self._after_id)
            self._after_id = None
        try:
            if self.edit_data is not None:
                self.edit_data.write_sync = True
                write_shared_data(self.edit_data)
        except Exception as e:
            print(f"Error saving on close: {e}")
        finally:
            try:
                import matplotlib.pyplot as plt
                plt.close("all")
            except Exception:
                pass
            self.root.destroy()


if __name__ == "__main__":
    root = tk.Tk()
    app = FanControlGUI(root)
    root.mainloop()
