#!/usr/bin/env python3
"""GUI front-end and tray daemon for the quadcastrgb CLI."""
import glob
import grp
import json
import os
import shlex
import shutil
import subprocess
import sys
import time

from PySide6.QtCore import QTimer, Qt, QPointF
from PySide6.QtGui import (
    QIcon, QAction, QColor, QPainter, QLinearGradient, QPen,
)
from PySide6.QtWidgets import (
    QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout,
    QFormLayout, QComboBox, QSpinBox, QPushButton, QLabel,
    QCheckBox, QGroupBox, QSystemTrayIcon, QMenu, QColorDialog, QMessageBox,
    QTabWidget,
)

APP_NAME = "quadcastrgb-gui"
CLI_BIN = "quadcastrgb"
RGB_GROUP = "hyperrgb"  # udev rule group; re-exec'd via sg(1) to dodge stale sessions
CONFIG_DIR = os.path.join(
    os.environ.get("XDG_CONFIG_HOME", os.path.expanduser("~/.config")),
    APP_NAME,
)
CONFIG_PATH = os.path.join(CONFIG_DIR, "config.json")

MODES = ["solid", "blink", "cycle", "wave", "lightning", "pulse"]
SINGLE_COLOR_MODES = {"solid", "lightning", "pulse"}

# VID:PID pairs handled by devio.c, used only to detect (re)plug events
KNOWN_IDS = {
    (0x0951, 0x171f),
    (0x03f0, 0x0f8b),
    (0x03f0, 0x028c),
    (0x03f0, 0x048c),
    (0x03f0, 0x068c),
    (0x03f0, 0x098c),
    (0x03f0, 0x09af),
    (0x03f0, 0x02b5),
}

DEFAULT_GROUP = {"mode": "solid", "colors": "", "br": 100, "spd": 81, "dly": 10,
                 "angle": 0, "width": 100}
WAVE_PARAM_MODES = {"wave"}  # modes where angle/width apply (2S only)

DEFAULT_CONFIG = {
    "verbose": False,
    "target": "all",
    "all": dict(DEFAULT_GROUP),
    "upper": dict(DEFAULT_GROUP),
    "lower": dict(DEFAULT_GROUP),
}


def load_config():
    try:
        with open(CONFIG_PATH) as f:
            cfg = json.load(f)
        merged = json.loads(json.dumps(DEFAULT_CONFIG))
        merged.update({k: v for k, v in cfg.items() if k in merged})
        for key in ("all", "upper", "lower"):
            merged[key] = {**DEFAULT_GROUP, **cfg.get(key, {})}
        return merged
    except (FileNotFoundError, json.JSONDecodeError, TypeError):
        return json.loads(json.dumps(DEFAULT_CONFIG))


def save_config(cfg):
    os.makedirs(CONFIG_DIR, exist_ok=True)
    with open(CONFIG_PATH, "w") as f:
        json.dump(cfg, f, indent=2)


def detect_present_ids():
    present = set()
    for vendor_path in glob.glob("/sys/bus/usb/devices/*/idVendor"):
        base = vendor_path[: -len("idVendor")]
        try:
            with open(vendor_path) as f:
                vid = int(f.read().strip(), 16)
            with open(base + "idProduct") as f:
                pid = int(f.read().strip(), 16)
        except (OSError, ValueError):
            continue
        if (vid, pid) in KNOWN_IDS:
            present.add((vid, pid))
    return present


def build_group_args(flag, group):
    args = [flag, "-b", str(group["br"]), "-s", str(group["spd"]),
             "-d", str(group["dly"]), "-g", str(group.get("angle", 0)),
             "-w", str(group.get("width", 100)), group["mode"]]
    colors = [c.strip().lstrip("#") for c in group["colors"].split(",") if c.strip()]
    args += colors
    return args


def build_full_args(cfg):
    if cfg["target"] == "split":
        return build_group_args("-u", cfg["upper"]) + build_group_args("-l", cfg["lower"])
    return build_group_args("-a", cfg["all"])


def kill_daemon():
    subprocess.run(["pkill", "-x", "-u", str(os.getuid()), CLI_BIN],
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.4)


def _group_exists(name):
    try:
        grp.getgrnam(name)
        return True
    except KeyError:
        return False


def run_cli(cfg):
    if shutil.which(CLI_BIN) is None:
        return False, f"'{CLI_BIN}' not found on PATH. Install it first."
    kill_daemon()
    cmd = [CLI_BIN]
    if cfg.get("verbose"):
        cmd.append("-v")
    cmd += build_full_args(cfg)
    if _group_exists(RGB_GROUP):
        cmd = ["sg", RGB_GROUP, "-c", shlex.join(cmd)]
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=5)
    except subprocess.TimeoutExpired:
        return False, "Timed out talking to the microphone."
    except OSError as e:
        return False, str(e)
    if proc.returncode != 0:
        return False, (proc.stderr or proc.stdout or
                        f"quadcastrgb exited with code {proc.returncode}")
    return True, "Applied successfully."


NODE_RADIUS = 7


class GradientBar(QWidget):
    """Interactive gradient bar: draggable color-stop nodes, click a node to
    recolor it, double-click empty space to add one."""

    def __init__(self):
        super().__init__()
        self.stops = [[0, "ff0000"]]
        self.selected = 0
        self.single_color_mode = False
        self.setMinimumHeight(50)
        self.setMouseTracking(True)
        self.stops_changed = None  # callback, fn() -> None
        self._drag_index = None
        self._press_x = None
        self._dragged = False

    def set_stops(self, stops):
        self.stops = sorted((list(s) for s in stops), key=lambda s: s[0]) \
                     or [[0, "ff0000"]]
        self.stops[0][0] = 0  # first node is mandatory and pinned to 0
        self.selected = min(self.selected, len(self.stops) - 1)
        self.update()

    def get_stops(self):
        return [list(s) for s in self.stops]

    def set_single_color_mode(self, single):
        self.single_color_mode = single
        if single and len(self.stops) > 1:
            self.stops = [self.stops[0]]
            self.selected = 0
            self._emit_changed()
        self.update()

    def _bar_rect(self):
        r = self.rect()
        return r.adjusted(NODE_RADIUS + 2, 14, -(NODE_RADIUS + 2), -14)

    def _x_for_pos(self, pos):
        bar = self._bar_rect()
        return bar.left() + bar.width()*pos/100

    def _pos_for_x(self, x, min_pos=1):
        bar = self._bar_rect()
        frac = (x - bar.left())/max(1, bar.width())
        return max(min_pos, min(100, round(frac*100)))

    def _emit_changed(self):
        if self.stops_changed:
            self.stops_changed()

    def paintEvent(self, _event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        bar = self._bar_rect()
        if len(self.stops) == 1:
            painter.fillRect(bar, QColor("#" + self.stops[0][1]))
        else:
            gradient = QLinearGradient(bar.left(), 0, bar.right(), 0)
            for pos, color in self.stops:
                gradient.setColorAt(pos/100, QColor("#" + color))
            # the animation always wraps back to the first color; show that
            # blend past the last node instead of hiding it off-screen
            gradient.setColorAt(1.0, QColor("#" + self.stops[0][1]))
            painter.fillRect(bar, gradient)
        painter.setPen(QColor("#555555"))
        painter.drawRect(bar)

        for i, (pos, color) in enumerate(self.stops):
            x = self._x_for_pos(pos)
            y = bar.center().y()
            painter.setBrush(QColor("#" + color))
            width = 3 if i == self.selected else 1
            outline = QColor("#ffffff") if i == self.selected else QColor("#000000")
            painter.setPen(QPen(outline, width))
            painter.drawEllipse(QPointF(x, y), NODE_RADIUS, NODE_RADIUS)

    def _hit_test(self, point):
        for i, (pos, _color) in enumerate(self.stops):
            if abs(self._x_for_pos(pos) - point.x()) <= NODE_RADIUS + 3:
                return i
        return None

    def _pick_color_for(self, index):
        current = QColor("#" + self.stops[index][1])
        color = QColorDialog.getColor(current, self, "Pick a color")
        if color.isValid():
            self.stops[index][1] = color.name().lstrip("#")
            self._emit_changed()
        self.update()

    def mousePressEvent(self, event):
        idx = self._hit_test(event.position())
        self._drag_index = idx
        self._dragged = False
        self._press_x = event.position().x()
        if idx is not None:
            self.selected = idx
            self.update()

    def mouseMoveEvent(self, event):
        if self._drag_index is None or self.single_color_mode:
            return
        if self._drag_index == 0:
            return  # first node is pinned to position 0, click-only to recolor
        if abs(event.position().x() - self._press_x) > 2:
            self._dragged = True
        self.stops[self._drag_index][0] = self._pos_for_x(event.position().x())
        self.update()

    def mouseReleaseEvent(self, event):
        if self._drag_index is None:
            return
        active = self.stops[self._drag_index]
        was_drag = self._dragged
        self._drag_index = None
        active_id = id(active)
        self.stops.sort(key=lambda s: s[0])
        self.selected = next(i for i, s in enumerate(self.stops)
                             if id(s) == active_id)
        self.update()
        if was_drag:
            self._emit_changed()
        else:
            self._pick_color_for(self.selected)

    def mouseDoubleClickEvent(self, event):
        if self.single_color_mode or self._hit_test(event.position()) is not None:
            return
        if not self._bar_rect().contains(event.position().toPoint()):
            return
        pos = self._pos_for_x(event.position().x())
        self.stops.append([pos, "ffffff"])
        self.stops.sort(key=lambda s: s[0])
        self.selected = next(i for i, s in enumerate(self.stops) if s[0] == pos)
        self.update()
        self._emit_changed()

    def remove_selected(self):
        if self.single_color_mode or len(self.stops) <= 1 or self.selected == 0:
            return  # first node is mandatory, can't be removed
        del self.stops[self.selected]
        self.selected = max(0, self.selected - 1)
        self.update()
        self._emit_changed()


class GradientEditor(QWidget):
    def __init__(self):
        super().__init__()
        self.bar = GradientBar()
        self.bar.stops_changed = self._on_bar_changed

        remove_btn = QPushButton("Remove selected node")
        remove_btn.clicked.connect(self.bar.remove_selected)
        hint = QLabel("Double-click the bar to add a color stop, click a "
                      "dot to recolor it, drag a dot to move it.")
        hint.setWordWrap(True)

        layout = QVBoxLayout()
        layout.addWidget(self.bar)
        layout.addWidget(remove_btn)
        layout.addWidget(hint)
        layout.setContentsMargins(0, 0, 0, 0)
        self.setLayout(layout)

    def _on_bar_changed(self):
        self.bar.update()

    def set_single_color_mode(self, single):
        self.bar.set_single_color_mode(single)

    def get_colors_string(self):
        stops = self.bar.get_stops()
        if len(stops) == 1:
            return stops[0][1]
        return ",".join(f"{color}@{pos}" for pos, color in stops)

    def set_colors_string(self, s):
        stops = []
        for i, token in enumerate(c.strip() for c in s.split(",")):
            if not token:
                continue
            if "@" in token:
                color, pos = token.split("@", 1)
                pos = int(pos)
            else:
                color, pos = token, 0
            stops.append([pos, color.lstrip("#")])
        if not stops:
            stops = [[0, "ff0000"]]
        elif len(stops) > 1 and all(p == 0 for p, _c in stops):
            # plain comma list with no explicit positions: space evenly,
            # leaving room after the last stop for the wrap-around blend
            # back to the first color (dividing by n, not n-1)
            n = len(stops)
            for i, s_ in enumerate(stops):
                s_[0] = round(i*100/n)
        self.bar.set_stops(stops)


class GroupPanel(QGroupBox):
    def __init__(self, title):
        super().__init__(title)
        self.mode_box = QComboBox()
        self.mode_box.addItems(MODES)
        self.gradient = GradientEditor()
        self.mode_box.currentTextChanged.connect(self._on_mode_changed)
        self.br_spin = QSpinBox()
        self.br_spin.setRange(0, 100)
        self.br_spin.setValue(100)
        self.spd_spin = QSpinBox()
        self.spd_spin.setRange(0, 100)
        self.spd_spin.setValue(81)
        self.dly_spin = QSpinBox()
        self.dly_spin.setRange(0, 100)
        self.dly_spin.setValue(10)
        self.angle_spin = QSpinBox()
        self.angle_spin.setRange(0, 360)
        self.angle_spin.setValue(0)
        self.angle_spin.setSuffix("°")
        self.width_spin = QSpinBox()
        self.width_spin.setRange(1, 400)
        self.width_spin.setValue(100)
        self.width_spin.setSuffix("%")

        form = QFormLayout()
        form.addRow("Mode", self.mode_box)
        form.addRow("Colors / gradient", self.gradient)
        form.addRow("Brightness (0-100)", self.br_spin)
        form.addRow("Speed (0-100, cycle/wave/lightning/pulse)", self.spd_spin)
        form.addRow("Delay (0-100, blink only)", self.dly_spin)
        form.addRow("Gradient angle (wave, Quadcast 2S only)", self.angle_spin)
        form.addRow("Gradient width (wave, Quadcast 2S only)", self.width_spin)
        self.setLayout(form)
        self._on_mode_changed()

    def _is_single_color_mode(self):
        return self.mode_box.currentText() in SINGLE_COLOR_MODES

    def _on_mode_changed(self, _mode=None):
        self.gradient.set_single_color_mode(self._is_single_color_mode())
        wave_mode = self.mode_box.currentText() in WAVE_PARAM_MODES
        self.angle_spin.setEnabled(wave_mode)
        self.width_spin.setEnabled(wave_mode)

    def to_dict(self):
        return {
            "mode": self.mode_box.currentText(),
            "colors": self.gradient.get_colors_string(),
            "br": self.br_spin.value(),
            "spd": self.spd_spin.value(),
            "dly": self.dly_spin.value(),
            "angle": self.angle_spin.value(),
            "width": self.width_spin.value(),
        }

    def from_dict(self, d):
        self.mode_box.setCurrentText(d.get("mode", "solid"))
        self.gradient.set_colors_string(d.get("colors", ""))
        self.br_spin.setValue(d.get("br", 100))
        self.spd_spin.setValue(d.get("spd", 81))
        self.dly_spin.setValue(d.get("dly", 10))
        self.angle_spin.setValue(d.get("angle", 0))
        self.width_spin.setValue(d.get("width", 100))
        self._on_mode_changed()


class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("QuadcastRGB")
        self.cfg = load_config()

        self.target_box = QComboBox()
        self.target_box.addItems(["Whole mic (all)", "Upper / lower separately"])
        self.target_box.currentIndexChanged.connect(self._on_target_changed)

        self.all_panel = GroupPanel("Whole microphone")
        self.upper_panel = GroupPanel("Upper diode")
        self.lower_panel = GroupPanel("Lower diode")

        self.split_tabs = QTabWidget()
        self.split_tabs.addTab(self.upper_panel, "Upper")
        self.split_tabs.addTab(self.lower_panel, "Lower")

        self.verbose_box = QCheckBox("Verbose CLI output")
        self.status_label = QLabel("")
        self.status_label.setWordWrap(True)

        apply_btn = QPushButton("Apply")
        apply_btn.clicked.connect(self.apply_now)
        off_btn = QPushButton("Turn off")
        off_btn.clicked.connect(self.turn_off)
        quit_btn = QPushButton("Quit")
        quit_btn.clicked.connect(self.quit_app)

        btn_row = QHBoxLayout()
        btn_row.addWidget(apply_btn)
        btn_row.addWidget(off_btn)
        btn_row.addStretch()
        btn_row.addWidget(quit_btn)

        layout = QVBoxLayout()
        layout.addWidget(QLabel("Diode target"))
        layout.addWidget(self.target_box)
        layout.addWidget(self.all_panel)
        layout.addWidget(self.split_tabs)
        layout.addWidget(self.verbose_box)
        layout.addLayout(btn_row)
        layout.addWidget(self.status_label)

        central = QWidget()
        central.setLayout(layout)
        self.setCentralWidget(central)

        self._load_into_ui()
        self._on_target_changed()

        icon = QIcon.fromTheme("audio-input-microphone")
        if icon.isNull():
            icon = self.style().standardIcon(self.style().SP_ComputerIcon)
        self.setWindowIcon(icon)

        self.tray = QSystemTrayIcon(icon, self)
        self.tray.setToolTip("QuadcastRGB")
        menu = QMenu()
        show_action = QAction("Show window", self)
        show_action.triggered.connect(self._show_from_tray)
        reapply_action = QAction("Re-apply last settings", self)
        reapply_action.triggered.connect(lambda: self.apply_now(silent=True))
        off_action = QAction("Turn off", self)
        off_action.triggered.connect(self.turn_off)
        quit_action = QAction("Quit", self)
        quit_action.triggered.connect(self.quit_app)
        menu.addAction(show_action)
        menu.addAction(reapply_action)
        menu.addAction(off_action)
        menu.addSeparator()
        menu.addAction(quit_action)
        self.tray.setContextMenu(menu)
        self.tray.activated.connect(self._tray_activated)
        self.tray.show()

        self._known_present = detect_present_ids()
        self.hotplug_timer = QTimer(self)
        self.hotplug_timer.timeout.connect(self._poll_hotplug)
        self.hotplug_timer.start(3000)

    def _load_into_ui(self):
        self.verbose_box.setChecked(self.cfg.get("verbose", False))
        self.target_box.setCurrentIndex(0 if self.cfg["target"] == "all" else 1)
        self.all_panel.from_dict(self.cfg["all"])
        self.upper_panel.from_dict(self.cfg["upper"])
        self.lower_panel.from_dict(self.cfg["lower"])

    def _on_target_changed(self):
        split = self.target_box.currentIndex() == 1
        self.all_panel.setVisible(not split)
        self.split_tabs.setVisible(split)

    def _collect_cfg(self):
        return {
            "verbose": self.verbose_box.isChecked(),
            "target": "split" if self.target_box.currentIndex() == 1 else "all",
            "all": self.all_panel.to_dict(),
            "upper": self.upper_panel.to_dict(),
            "lower": self.lower_panel.to_dict(),
        }

    def apply_now(self, silent=False):
        self.cfg = self._collect_cfg()
        save_config(self.cfg)
        ok, msg = run_cli(self.cfg)
        self.status_label.setText(msg)
        if not silent:
            self.tray.showMessage("QuadcastRGB", msg,
                                   QSystemTrayIcon.MessageIcon.Information
                                   if ok else QSystemTrayIcon.MessageIcon.Warning,
                                   3000)

    def turn_off(self):
        self.target_box.setCurrentIndex(0)
        off_group = {"mode": "solid", "colors": "000000", "br": 100,
                     "spd": 81, "dly": 10}
        self.all_panel.from_dict(off_group)
        self.apply_now()

    def _poll_hotplug(self):
        present = detect_present_ids()
        if present - self._known_present:
            time.sleep(0.5)
            self.apply_now(silent=True)
        self._known_present = present

    def _show_from_tray(self):
        self.show()
        self.raise_()
        self.activateWindow()

    def _tray_activated(self, reason):
        if reason == QSystemTrayIcon.ActivationReason.Trigger:
            if self.isVisible():
                self.hide()
            else:
                self._show_from_tray()

    def closeEvent(self, event):
        event.ignore()
        self.hide()

    def quit_app(self):
        self.tray.hide()
        QApplication.instance().quit()


def main():
    app = QApplication(sys.argv)
    app.setQuitOnLastWindowClosed(False)
    window = MainWindow()
    window.apply_now(silent=True)
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
