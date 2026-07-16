#!/usr/bin/env python3
"""panorama_sensor_display.py

Live PC telemetry overlay for the Tryx Panorama SE 360 AIO AMOLED screen.
Renders CPU / GPU / RAM / Network / Cooling stats plus a clock onto a base
image every N seconds and pushes it to the panel via `reed-tpse`.

Quick start:
    python3 panorama_sensor_display.py --detect          # find your fan/pump channels
    python3 panorama_sensor_display.py --init-config     # write a starter config
    python3 panorama_sensor_display.py --once --out /tmp/preview.png
    python3 panorama_sensor_display.py                   # run the loop

Config lives at ~/.config/panorama-overlay/config.yaml (override with --config).
See config.example.yaml for all knobs.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import time
from collections import deque
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path
from typing import Optional

import psutil
from PIL import Image, ImageDraw, ImageFont

# ============================== Constants =====================================

SCREEN_W, SCREEN_H = 2240, 1080

REPO_DIR = Path(__file__).resolve().parent
DEFAULT_BASE_IMAGE = REPO_DIR / "assets" / "default_background.png"

DEFAULT_FONT_CANDIDATES = (
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf",
    "/System/Library/Fonts/Supplemental/Arial Bold.ttf",
)

FRAME_PATH_DEFAULT = Path("/tmp/panorama_overlay_frame.png")


def xdg_config_home() -> Path:
    return Path(os.environ.get("XDG_CONFIG_HOME") or (Path.home() / ".config"))


DEFAULT_CONFIG_PATH = xdg_config_home() / "panorama-overlay" / "config.yaml"


# ============================== Config ========================================

@dataclass
class SensorsConfig:
    # Explicit `sensors -j` feature names. Take priority over keywords.
    pump: list = field(default_factory=list)            # e.g. ["fan16"]
    radiator_fans: list = field(default_factory=list)   # e.g. ["fan1", "fan15"]
    cpu_fan: list = field(default_factory=list)         # e.g. ["fan2"]
    coolant_temp: list = field(default_factory=list)    # e.g. ["Thermistor 15"]

    # Keyword fallback used only if the explicit list is empty.
    pump_keywords: list = field(default_factory=lambda: ["pump"])
    fan_keywords: list = field(default_factory=lambda: ["fan"])
    coolant_keywords: list = field(default_factory=lambda: [
        "coolant", "water", "liquid",
    ])


@dataclass
class CpuConfig:
    # First matching temp sensor group wins.
    temp_sensors: list = field(default_factory=lambda: [
        "k10temp", "coretemp", "zenpower", "cpu_thermal",
    ])
    # RAPL µJ energy counters, tried in order. Zen AMD also exposes intel-rapl.
    rapl_paths: list = field(default_factory=lambda: [
        "/sys/class/powercap/intel-rapl:0/energy_uj",
    ])


@dataclass
class AppConfig:
    interval_seconds: float = 30.0
    brightness: int = 80
    gpu: str = "auto"            # nvidia | amd | auto | none
    base_image: Optional[str] = None
    font: Optional[str] = None
    keep_on_device: int = 3
    sensors: SensorsConfig = field(default_factory=SensorsConfig)
    cpu: CpuConfig = field(default_factory=CpuConfig)

    @classmethod
    def load(cls, path: Optional[Path]) -> "AppConfig":
        if path is None or not path.exists():
            return cls()
        try:
            import yaml  # PyYAML
        except ImportError:
            print(
                f"warning: PyYAML not installed, ignoring {path}. "
                f"Install with: pip install pyyaml",
                file=sys.stderr,
            )
            return cls()
        data = yaml.safe_load(path.read_text()) or {}
        sensors_data = data.get("sensors") or {}
        cpu_data = data.get("cpu") or {}
        base = cls()
        return cls(
            interval_seconds=float(data.get("interval_seconds", base.interval_seconds)),
            brightness=int(data.get("brightness", base.brightness)),
            gpu=str(data.get("gpu", base.gpu)),
            base_image=data.get("base_image"),
            font=data.get("font"),
            keep_on_device=int(data.get("keep_on_device", base.keep_on_device)),
            sensors=SensorsConfig(
                pump=list(sensors_data.get("pump", []) or []),
                radiator_fans=list(sensors_data.get("radiator_fans", []) or []),
                cpu_fan=list(sensors_data.get("cpu_fan", []) or []),
                coolant_temp=list(sensors_data.get("coolant_temp", []) or []),
                pump_keywords=list(sensors_data.get(
                    "pump_keywords", base.sensors.pump_keywords)),
                fan_keywords=list(sensors_data.get(
                    "fan_keywords", base.sensors.fan_keywords)),
                coolant_keywords=list(sensors_data.get(
                    "coolant_keywords", base.sensors.coolant_keywords)),
            ),
            cpu=CpuConfig(
                temp_sensors=list(cpu_data.get(
                    "temp_sensors", base.cpu.temp_sensors)),
                rapl_paths=list(cpu_data.get(
                    "rapl_paths", base.cpu.rapl_paths)),
            ),
        )


# Set by main() once the config is loaded; collectors read from here so their
# call sites don't need to thread config through everything.
CFG: AppConfig = AppConfig()


# ============================== Data classes ==================================

@dataclass
class CpuStats:
    temp_c: Optional[float] = None
    load_pct: Optional[float] = None
    clock_mhz: Optional[float] = None
    power_w: Optional[float] = None


@dataclass
class GpuStats:
    temp_c: Optional[float] = None
    util_pct: Optional[float] = None
    clock_mhz: Optional[float] = None
    vram_used_mb: Optional[float] = None
    vram_total_mb: Optional[float] = None
    power_w: Optional[float] = None
    fan_pct: Optional[float] = None


@dataclass
class CoolingStats:
    pump_rpm: Optional[int] = None
    fan_rpms: list = field(default_factory=list)
    cpu_fan_rpm: Optional[int] = None
    coolant_temp_c: Optional[float] = None


@dataclass
class RamStats:
    total_gb: Optional[float] = None
    used_pct: Optional[float] = None
    available_gb: Optional[float] = None


@dataclass
class NetStats:
    down_mbps: Optional[float] = None
    up_mbps: Optional[float] = None


# ============================== Collectors ====================================

# CPU power via RAPL — needs two samples to compute a delta.
_rapl_prev = {"energy": None, "ts": None, "path": None, "max_path": None}


def _read_int(path: str) -> Optional[int]:
    try:
        with open(path, "r") as f:
            return int(f.read().strip())
    except OSError:
        return None


def _resolve_rapl_path() -> Optional[str]:
    """Return the first readable RAPL energy counter from the configured list."""
    for p in CFG.cpu.rapl_paths:
        if _read_int(p) is not None:
            return p
    return None


def cpu_power_watts() -> Optional[float]:
    now = time.monotonic()
    path = _rapl_prev["path"] or _resolve_rapl_path()
    if path is None:
        return None
    energy = _read_int(path)
    if energy is None:
        return None
    if _rapl_prev["path"] != path:
        # First read on this path — cache and figure out the wrap ceiling.
        max_path = str(Path(path).parent / "max_energy_range_uj")
        _rapl_prev["path"] = path
        _rapl_prev["max_path"] = max_path if _read_int(max_path) is not None else None

    prev_e, prev_t = _rapl_prev["energy"], _rapl_prev["ts"]
    _rapl_prev["energy"], _rapl_prev["ts"] = energy, now
    if prev_e is None or prev_t is None:
        return None
    delta_e = energy - prev_e
    if delta_e < 0:  # counter wrap
        max_e = _read_int(_rapl_prev["max_path"]) if _rapl_prev["max_path"] else None
        if max_e is None:
            return None
        delta_e += max_e
    delta_t = now - prev_t
    if delta_t <= 0:
        return None
    return (delta_e / delta_t) / 1_000_000.0  # µW -> W


def cpu_temperature_c() -> Optional[float]:
    try:
        temps = psutil.sensors_temperatures()
    except AttributeError:
        return None
    if not temps:
        return None
    for key in CFG.cpu.temp_sensors:
        entries = temps.get(key)
        if not entries:
            continue
        for e in entries:
            if e.label in ("Tctl", "Tdie", "Package id 0"):
                return e.current
        return entries[0].current
    first = next(iter(temps.values()), None)
    return first[0].current if first else None


def collect_cpu() -> CpuStats:
    freq = psutil.cpu_freq()
    return CpuStats(
        temp_c=cpu_temperature_c(),
        # Blocking sample so every frame gets a real reading, not the delta
        # since the previous 30s-old call (which often reads as 0).
        load_pct=psutil.cpu_percent(interval=0.3),
        clock_mhz=freq.current if freq else None,
        power_w=cpu_power_watts(),
    )


# ---- GPU (NVIDIA) ----

def _to_float(x: Optional[str]) -> Optional[float]:
    try:
        return float(x)
    except (TypeError, ValueError):
        return None


def collect_gpu_nvidia() -> GpuStats:
    if not shutil.which("nvidia-smi"):
        return GpuStats()
    try:
        out = subprocess.check_output(
            [
                "nvidia-smi",
                "--query-gpu=temperature.gpu,utilization.gpu,clocks.gr,"
                "memory.used,memory.total,power.draw,fan.speed",
                "--format=csv,noheader,nounits",
            ],
            text=True,
            timeout=3,
        ).strip().splitlines()
    except (subprocess.SubprocessError, OSError):
        return GpuStats()
    if not out:
        return GpuStats()
    parts = [p.strip() for p in out[0].split(",")]
    parts = (parts + [None] * 7)[:7]
    t, util, clk, used, total, pwr, fan = parts
    return GpuStats(
        temp_c=_to_float(t),
        util_pct=_to_float(util),
        clock_mhz=_to_float(clk),
        vram_used_mb=_to_float(used),
        vram_total_mb=_to_float(total),
        power_w=_to_float(pwr),
        fan_pct=_to_float(fan),
    )


# ---- GPU (AMD via sysfs) ----

def _amdgpu_hwmon() -> Optional[Path]:
    """Return the hwmon directory for the amdgpu device, if any."""
    for h in Path("/sys/class/hwmon").glob("hwmon*"):
        try:
            if (h / "name").read_text().strip() == "amdgpu":
                return h
        except OSError:
            continue
    return None


def _amdgpu_card_device() -> Optional[Path]:
    """Return /sys/class/drm/cardN/device for the first AMD (vendor 0x1002) GPU."""
    for card in sorted(Path("/sys/class/drm").glob("card*")):
        dev = card / "device"
        vendor = dev / "vendor"
        if not vendor.exists():
            continue
        try:
            if vendor.read_text().strip() == "0x1002":
                return dev
        except OSError:
            continue
    return None


def _read_int_file(p: Path) -> Optional[int]:
    try:
        return int(p.read_text().strip())
    except (OSError, ValueError):
        return None


def collect_gpu_amd() -> GpuStats:
    stats = GpuStats()
    hwmon = _amdgpu_hwmon()
    if hwmon is not None:
        # Prefer the "edge" temperature label.
        for i in range(1, 6):
            label_path = hwmon / f"temp{i}_label"
            input_path = hwmon / f"temp{i}_input"
            if label_path.exists() and input_path.exists():
                try:
                    if label_path.read_text().strip() == "edge":
                        v = _read_int_file(input_path)
                        if v is not None:
                            stats.temp_c = v / 1000.0
                        break
                except OSError:
                    continue
        # Power: power1_input is in µW.
        v = _read_int_file(hwmon / "power1_input")
        if v is not None:
            stats.power_w = v / 1_000_000.0
        # Some AMD GPUs also expose freq1_input in Hz.
        v = _read_int_file(hwmon / "freq1_input")
        if v is not None:
            stats.clock_mhz = v / 1_000_000.0
        # Fan pct: pwm1 is 0-255 duty cycle.
        pwm = _read_int_file(hwmon / "pwm1")
        if pwm is not None:
            stats.fan_pct = round(100.0 * pwm / 255.0, 1)

    dev = _amdgpu_card_device()
    if dev is not None:
        v = _read_int_file(dev / "gpu_busy_percent")
        if v is not None:
            stats.util_pct = float(v)
        used = _read_int_file(dev / "mem_info_vram_used")
        total = _read_int_file(dev / "mem_info_vram_total")
        if used is not None:
            stats.vram_used_mb = used / (1024 * 1024)
        if total is not None:
            stats.vram_total_mb = total / (1024 * 1024)
        # Fallback clock via pp_dpm_sclk (line marked with '*' is active).
        if stats.clock_mhz is None:
            try:
                for line in (dev / "pp_dpm_sclk").read_text().splitlines():
                    if "*" in line and ":" in line:
                        token = line.split(":", 1)[1].strip().split()[0]
                        # token like "600Mhz*" or "2200MHz"
                        digits = "".join(c for c in token if c.isdigit() or c == ".")
                        if digits:
                            stats.clock_mhz = float(digits)
                            break
            except OSError:
                pass
    return stats


def collect_gpu_auto() -> GpuStats:
    """Try NVIDIA first (nvidia-smi is the definitive marker), then AMD."""
    if shutil.which("nvidia-smi"):
        stats = collect_gpu_nvidia()
        if stats.temp_c is not None or stats.util_pct is not None:
            return stats
    if _amdgpu_hwmon() is not None or _amdgpu_card_device() is not None:
        return collect_gpu_amd()
    return GpuStats()


# ---- Cooling via lm-sensors ----

def _sensors_json() -> Optional[dict]:
    if not shutil.which("sensors"):
        return None
    try:
        raw = subprocess.check_output(["sensors", "-j"], text=True, timeout=3)
        return json.loads(raw)
    except (subprocess.SubprocessError, OSError, json.JSONDecodeError):
        return None


def _walk_sensor_features(data: dict):
    """Yield (chip, feature, fan_rpm | None, temp_c | None) tuples."""
    for chip, chip_val in data.items():
        if not isinstance(chip_val, dict):
            continue
        for feat_name, feat in chip_val.items():
            if not isinstance(feat, dict):
                continue
            fan_rpm: Optional[int] = None
            temp_c: Optional[float] = None
            for k, v in feat.items():
                if not k.endswith("_input") or not isinstance(v, (int, float)):
                    continue
                if k.startswith("fan"):
                    fan_rpm = int(v)
                elif k.startswith("temp"):
                    temp_c = float(v)
            if fan_rpm is not None or temp_c is not None:
                yield chip, feat_name, fan_rpm, temp_c


def collect_cooling() -> CoolingStats:
    data = _sensors_json()
    if data is None:
        return CoolingStats()

    rpm_by_feature: dict = {}
    temp_by_feature: dict = {}
    for _chip, feat, fan_rpm, temp_c in _walk_sensor_features(data):
        if fan_rpm is not None:
            rpm_by_feature[feat] = fan_rpm
        if temp_c is not None:
            temp_by_feature[feat] = temp_c

    s = CFG.sensors
    pump_rpm: Optional[int] = None
    fan_rpms: list = []
    cpu_fan_rpm: Optional[int] = None
    coolant_temp_c: Optional[float] = None

    for name in s.pump:
        if name in rpm_by_feature:
            pump_rpm = rpm_by_feature[name]
            break
    for name in s.radiator_fans:
        if name in rpm_by_feature:
            fan_rpms.append(rpm_by_feature[name])
    for name in s.cpu_fan:
        if name in rpm_by_feature:
            cpu_fan_rpm = rpm_by_feature[name]
            break
    for name in s.coolant_temp:
        if name in temp_by_feature:
            coolant_temp_c = temp_by_feature[name]
            break

    # Keyword fallback for anything not resolved explicitly.
    if pump_rpm is None and not s.pump:
        for name, val in rpm_by_feature.items():
            if any(w in name.lower() for w in s.pump_keywords):
                pump_rpm = val
                break
    if not fan_rpms and not s.radiator_fans:
        for name, val in rpm_by_feature.items():
            if any(w in name.lower() for w in s.fan_keywords) and val > 0:
                fan_rpms.append(val)
    if coolant_temp_c is None and not s.coolant_temp:
        for name, val in temp_by_feature.items():
            if any(w in name.lower() for w in s.coolant_keywords):
                coolant_temp_c = val
                break

    return CoolingStats(
        pump_rpm=pump_rpm,
        fan_rpms=fan_rpms,
        cpu_fan_rpm=cpu_fan_rpm,
        coolant_temp_c=coolant_temp_c,
    )


# ---- RAM ----

def collect_ram() -> RamStats:
    try:
        vm = psutil.virtual_memory()
    except Exception:
        return RamStats()
    gb = 1024 ** 3
    return RamStats(
        total_gb=vm.total / gb,
        used_pct=vm.percent,
        available_gb=vm.available / gb,
    )


# ---- Network (needs two samples) ----

_net_prev = {"bytes_recv": None, "bytes_sent": None, "ts": None}


def collect_network() -> NetStats:
    try:
        counters = psutil.net_io_counters()
    except Exception:
        return NetStats()
    now = time.monotonic()
    br, bs = counters.bytes_recv, counters.bytes_sent
    prev_br = _net_prev["bytes_recv"]
    prev_bs = _net_prev["bytes_sent"]
    prev_ts = _net_prev["ts"]
    _net_prev["bytes_recv"], _net_prev["bytes_sent"], _net_prev["ts"] = br, bs, now
    if prev_br is None or prev_ts is None:
        return NetStats()
    dt = now - prev_ts
    if dt <= 0:
        return NetStats()
    # Bytes/sec -> megabits/sec (network convention).
    down_mbps = ((br - prev_br) * 8) / (dt * 1_000_000)
    up_mbps   = ((bs - prev_bs) * 8) / (dt * 1_000_000)
    return NetStats(
        down_mbps=max(0.0, down_mbps),
        up_mbps=max(0.0, up_mbps),
    )


# ============================== Rendering =====================================

MARGIN         = 30
PANEL_GAP      = 30
CLOCK_H        = 230
MIDDLE_H       = 250
PANEL_H        = 430
PANEL_RADIUS   = 28
PANEL_FILL     = (0, 0, 0, 165)
PAD            = 30
DIVIDER_COLOR  = (255, 255, 255, 60)


def load_font(path: Optional[str], size: int) -> ImageFont.ImageFont:
    candidates = (path,) if path else DEFAULT_FONT_CANDIDATES
    for c in candidates:
        if c and Path(c).exists():
            try:
                return ImageFont.truetype(c, size)
            except OSError:
                continue
    return ImageFont.load_default()


def fmt(v: Optional[float], unit: str, prec: int = 0) -> str:
    if v is None:
        return f"-- {unit}"
    if prec == 0:
        return f"{int(round(v))} {unit}"
    return f"{v:.{prec}f} {unit}"


def _panel(draw: ImageDraw.ImageDraw, box, radius: int = PANEL_RADIUS,
           fill=PANEL_FILL) -> None:
    draw.rounded_rectangle(box, radius=radius, fill=fill)


def _text_height(font: ImageFont.ImageFont, sample: str = "Ag") -> int:
    bbox = font.getbbox(sample)
    return bbox[3] - bbox[1]


def _fit_font(text: str, base_font_path: Optional[str], start_size: int,
              min_size: int, max_width: int) -> ImageFont.ImageFont:
    """Return a font sized down from `start_size` until `text` fits in
    `max_width`. Ensures values never overflow their panel."""
    size = start_size
    while size > min_size:
        font = load_font(base_font_path, size)
        w = font.getlength(text) if hasattr(font, "getlength") else size * len(text) * 0.5
        if w <= max_width:
            return font
        size -= 4
    return load_font(base_font_path, min_size)


def _stat_block(draw, box, title, rows, font_path,
                accent=(255, 200, 0, 255)) -> None:
    """Panel with a title bar and N label/value rows. Value fonts auto-shrink
    to fit the right-hand column so long strings never overflow."""
    x0, y0, x1, y1 = box
    _panel(draw, box)

    f_title = load_font(font_path, 48)
    f_label = load_font(font_path, 30)

    draw.text((x0 + PAD, y0 + PAD - 8), title, font=f_title, fill=accent)
    title_bottom = y0 + PAD + _text_height(f_title)

    draw.line(
        [(x0 + PAD, title_bottom + 10), (x1 - PAD, title_bottom + 10)],
        fill=DIVIDER_COLOR, width=2,
    )

    rows_top = title_bottom + 28
    rows_area = (y1 - PAD) - rows_top
    n = max(1, len(rows))
    row_h = rows_area / n

    inner_w = (x1 - PAD) - (x0 + PAD)
    label_col_w = int(inner_w * 0.36)
    value_col_w = inner_w - label_col_w - 20
    value_x_right = x1 - PAD

    for i, (label, value) in enumerate(rows):
        row_top = rows_top + i * row_h
        row_center_y = row_top + row_h / 2

        f_val = _fit_font(value, font_path, start_size=54, min_size=26,
                          max_width=value_col_w)
        v_h = _text_height(f_val)
        l_h = _text_height(f_label)

        draw.text(
            (x0 + PAD, row_center_y - l_h / 2 - 2),
            label, font=f_label, fill=(210, 210, 210, 255),
        )
        v_w = f_val.getlength(value)
        draw.text(
            (value_x_right - v_w, row_center_y - v_h / 2 - 2),
            value, font=f_val, fill="white",
        )


def _stat_block_horizontal(draw, box, title, cells, font_path,
                           accent=(255, 200, 0, 255)) -> None:
    """Panel with a title bar and N cells laid out horizontally. Each cell
    is (label, value) stacked vertically."""
    x0, y0, x1, y1 = box
    _panel(draw, box)

    f_title = load_font(font_path, 44)
    f_label = load_font(font_path, 28)

    draw.text((x0 + PAD, y0 + PAD - 6), title, font=f_title, fill=accent)
    title_bottom = y0 + PAD + _text_height(f_title)
    draw.line(
        [(x0 + PAD, title_bottom + 10), (x1 - PAD, title_bottom + 10)],
        fill=DIVIDER_COLOR, width=2,
    )

    cells_top = title_bottom + 22
    cells_bot = y1 - PAD
    inner_x0  = x0 + PAD
    inner_x1  = x1 - PAD
    n = max(1, len(cells))
    cell_w = (inner_x1 - inner_x0) / n

    for i, (label, value) in enumerate(cells):
        cx0 = inner_x0 + i * cell_w

        if i > 0:
            draw.line(
                [(cx0, cells_top + 6), (cx0, cells_bot - 6)],
                fill=DIVIDER_COLOR, width=2,
            )

        avail_w = cell_w - 24
        f_val = _fit_font(value, font_path, start_size=80, min_size=32,
                          max_width=avail_w)
        v_h = _text_height(f_val)
        l_h = _text_height(f_label)

        cell_cy = (cells_top + cells_bot) / 2
        stack_h = v_h + 6 + l_h
        stack_top = cell_cy - stack_h / 2

        v_w = f_val.getlength(value)
        draw.text((cx0 + cell_w / 2 - v_w / 2, stack_top),
                  value, font=f_val, fill="white")
        l_w = f_label.getlength(label)
        draw.text((cx0 + cell_w / 2 - l_w / 2, stack_top + v_h + 6),
                  label, font=f_label, fill=(200, 200, 200, 255))


def _format_fan_list(fan_rpms: list) -> str:
    if not fan_rpms:
        return "-- RPM"
    return " · ".join(str(r) for r in fan_rpms) + " RPM"


def render_frame(base_image_path: Optional[Path], font_path: Optional[str],
                 cpu: CpuStats, gpu: GpuStats, cool: CoolingStats,
                 ram: RamStats, net: NetStats) -> Image.Image:
    if base_image_path and Path(base_image_path).exists():
        bg = Image.open(base_image_path).convert("RGBA")
        if bg.size != (SCREEN_W, SCREEN_H):
            bg = bg.resize((SCREEN_W, SCREEN_H), Image.LANCZOS)
    else:
        bg = Image.new("RGBA", (SCREEN_W, SCREEN_H), (0, 0, 0, 255))

    overlay = Image.new("RGBA", bg.size, (0, 0, 0, 0))
    draw = ImageDraw.Draw(overlay)

    # ---- Clock + date (top center band) ----
    f_clock = load_font(font_path, 128)
    f_date  = load_font(font_path, 40)

    now = datetime.now()
    clock_txt = now.strftime("%H:%M:%S")
    date_txt  = now.strftime("%A, %d %b %Y")

    clock_w = f_clock.getlength(clock_txt)
    date_w  = f_date.getlength(date_txt)
    band_w = max(clock_w, date_w) + 120
    band_x0 = SCREEN_W / 2 - band_w / 2
    band_x1 = SCREEN_W / 2 + band_w / 2
    _panel(draw, (band_x0, MARGIN, band_x1, MARGIN + CLOCK_H))

    ch = _text_height(f_clock)
    dh = _text_height(f_date)
    inner_top = MARGIN + 20
    inner_bot = MARGIN + CLOCK_H - 20
    total_h = ch + 20 + dh
    stack_top = (inner_top + inner_bot) / 2 - total_h / 2
    draw.text((SCREEN_W / 2 - clock_w / 2, stack_top),
              clock_txt, font=f_clock, fill="white")
    draw.text((SCREEN_W / 2 - date_w / 2, stack_top + ch + 20),
              date_txt, font=f_date, fill=(220, 220, 220, 255))

    # ---- Middle strip: [ RAM ] [ NETWORK ] ----
    mid_y0 = MARGIN + CLOCK_H + 30
    mid_y1 = mid_y0 + MIDDLE_H
    mid_panel_w = (SCREEN_W - 2 * MARGIN - PANEL_GAP) // 2
    ram_x0 = MARGIN
    net_x0 = MARGIN + mid_panel_w + PANEL_GAP

    ram_rows = [
        ("Used",   fmt(ram.used_pct,     "%")),
        ("Avail",  fmt(ram.available_gb, "GB", 1)),
        ("Total",  fmt(ram.total_gb,     "GB", 1)),
    ]
    _stat_block_horizontal(
        draw,
        (ram_x0, mid_y0, ram_x0 + mid_panel_w, mid_y1),
        "RAM", ram_rows, font_path,
        accent=(200, 140, 255, 255),
    )

    net_rows = [
        ("Down", fmt(net.down_mbps, "Mbps", 1)),
        ("Up",   fmt(net.up_mbps,   "Mbps", 1)),
    ]
    _stat_block_horizontal(
        draw,
        (net_x0, mid_y0, net_x0 + mid_panel_w, mid_y1),
        "NETWORK", net_rows, font_path,
        accent=(255, 220, 100, 255),
    )

    # ---- Three bottom panels: CPU | COOLING | GPU ----
    panel_w = (SCREEN_W - 2 * MARGIN - 2 * PANEL_GAP) // 3
    panel_y0 = SCREEN_H - MARGIN - PANEL_H
    panel_y1 = SCREEN_H - MARGIN

    cpu_x0 = MARGIN
    cool_x0 = MARGIN + panel_w + PANEL_GAP
    gpu_x0 = MARGIN + 2 * (panel_w + PANEL_GAP)

    _stat_block(
        draw,
        (cpu_x0, panel_y0, cpu_x0 + panel_w, panel_y1),
        "CPU",
        [
            ("Temp",  fmt(cpu.temp_c,   "°C")),
            ("Load",  fmt(cpu.load_pct, "%")),
            ("Clock", fmt(cpu.clock_mhz, "MHz")),
            ("Power", fmt(cpu.power_w,  "W", 1)),
        ],
        font_path,
        accent=(255, 170, 60, 255),
    )

    cooling_rows = [
        ("CPU Fan", fmt(cool.cpu_fan_rpm, "RPM") if cool.cpu_fan_rpm else "-- RPM"),
        ("Pump",    fmt(cool.pump_rpm,    "RPM") if cool.pump_rpm    else "-- RPM"),
        ("Fans",    _format_fan_list(cool.fan_rpms)),
    ]
    if cool.coolant_temp_c is not None:
        cooling_rows.insert(0, ("Coolant", fmt(cool.coolant_temp_c, "°C", 1)))
    _stat_block(
        draw,
        (cool_x0, panel_y0, cool_x0 + panel_w, panel_y1),
        "COOLING", cooling_rows, font_path,
        accent=(120, 190, 255, 255),
    )

    if gpu.vram_used_mb is not None and gpu.vram_total_mb is not None:
        vram_txt = f"{int(gpu.vram_used_mb)} / {int(gpu.vram_total_mb)} MB"
    else:
        vram_txt = "-- MB"
    _stat_block(
        draw,
        (gpu_x0, panel_y0, gpu_x0 + panel_w, panel_y1),
        "GPU",
        [
            ("Temp",  fmt(gpu.temp_c,    "°C")),
            ("Util",  fmt(gpu.util_pct,  "%")),
            ("Clock", fmt(gpu.clock_mhz, "MHz")),
            ("VRAM",  vram_txt),
            ("Fan",   fmt(gpu.fan_pct,   "%")),
        ],
        font_path,
        accent=(120, 220, 120, 255),
    )

    return Image.alpha_composite(bg, overlay).convert("RGB")


# ============================== Device push ===================================

def upload_and_display(frame: Path, brightness: int) -> None:
    subprocess.run(
        ["reed-tpse", "upload", str(frame)],
        check=True, timeout=30,
    )
    subprocess.run(
        ["reed-tpse", "display", frame.name, "--brightness", str(brightness)],
        check=True, timeout=15,
    )


def cleanup_leftover_frames(out_dir: Path, stem: str, suffix: str) -> None:
    """Remove any leftover overlay frames from prior runs, locally and on the
    device. Matches `<stem>_*<suffix>`."""
    for p in out_dir.glob(f"{stem}_*{suffix}"):
        try:
            p.unlink()
        except OSError:
            pass
    try:
        listing = subprocess.check_output(
            ["reed-tpse", "list"], text=True, timeout=15,
        )
    except (subprocess.SubprocessError, OSError):
        return
    prefix = f"{stem}_"
    for line in listing.splitlines():
        name = line.strip()
        if name.startswith(prefix) and name.endswith(suffix):
            subprocess.run(
                ["reed-tpse", "delete", name],
                check=False, timeout=15,
            )


# ============================== --detect / --init-config ======================

def _suggest_from_sensors() -> tuple[dict, str]:
    """Inspect `sensors -j` and heuristically suggest a starter config.
    Returns (suggested_sensors_dict, human_readable_summary)."""
    data = _sensors_json()
    if not data:
        return (
            {},
            "sensors: command not found or returned no data.\n"
            "Install lm-sensors and run `sudo sensors-detect` first:\n"
            "    Debian/Ubuntu: sudo apt install lm-sensors\n"
            "    Arch:          sudo pacman -S lm_sensors\n"
            "    Fedora:        sudo dnf install lm_sensors\n",
        )

    lines: list = []
    all_fans: list = []
    all_temps: list = []

    lines.append("Detected chips and channels (from `sensors -j`):\n")
    for chip, chip_val in data.items():
        if not isinstance(chip_val, dict):
            continue
        chip_features = list(_walk_sensor_features({chip: chip_val}))
        if not chip_features:
            continue
        lines.append(f"  [{chip}]")
        for _c, feat, fan_rpm, temp_c in chip_features:
            if fan_rpm is not None:
                marker = " <- possible pump (high RPM)" if fan_rpm > 2000 else ""
                lines.append(f"    fan  {feat:<20} {fan_rpm:>6} RPM{marker}")
                all_fans.append((feat, fan_rpm))
            if temp_c is not None:
                lines.append(f"    temp {feat:<20} {temp_c:>6.1f} °C")
                all_temps.append((feat, temp_c))

    # Heuristic suggestion.
    pump: list = []
    rad_fans: list = []
    cpu_fan: list = []
    spinning = [(f, r) for f, r in all_fans if r > 0]
    if spinning:
        # Highest-RPM channel is usually the pump.
        pump_candidate = max(spinning, key=lambda kv: kv[1])
        if pump_candidate[1] > 1500:
            pump = [pump_candidate[0]]
        remaining = [f for f, _ in spinning if f != pump_candidate[0] or not pump]
        # Everything else that's spinning is likely a case/rad fan.
        rad_fans = remaining
        # First remaining is a reasonable CPU-fan guess.
        if remaining:
            cpu_fan = [remaining[0]]

    lines.append("")
    lines.append("Suggested sensors config (edit to match your board):")
    lines.append(f"  pump:          {pump}")
    lines.append(f"  radiator_fans: {rad_fans}")
    lines.append(f"  cpu_fan:       {cpu_fan}")

    suggested = {
        "pump": pump,
        "radiator_fans": rad_fans,
        "cpu_fan": cpu_fan,
        "coolant_temp": [],
    }
    return suggested, "\n".join(lines)


def cmd_detect() -> int:
    _, summary = _suggest_from_sensors()
    print(summary)
    print()
    print("Write these into your config with: panorama_sensor_display.py --init-config")
    return 0


CONFIG_TEMPLATE = """# panorama-overlay config
# Regenerate with: panorama_sensor_display.py --detect --init-config
#
# Run `panorama_sensor_display.py --detect` to see your board's fan/temp
# channel names, then paste the ones you want below.

# --- Update loop -------------------------------------------------------------
interval_seconds: 30
brightness: 80          # 0-100

# GPU collector: nvidia | amd | auto | none
gpu: auto

# Background image (2240x1080 PNG). null = use bundled default_background.png.
base_image: null

# TTF font path. null = auto-search DejaVuSans-Bold / Arial Bold.
font: null

# How many frames to keep on the device at once. Higher = safer race margin,
# more USB churn. 3 is a good default.
keep_on_device: 3

# --- lm-sensors channel mapping ---------------------------------------------
# Feature names come from `sensors -j`. See --detect for a nicer view.
sensors:
  pump:          {pump}
  radiator_fans: {radiator_fans}
  cpu_fan:       {cpu_fan}
  coolant_temp:  {coolant_temp}    # often unavailable

  # Fallback keyword search — used only when the explicit list above is empty.
  pump_keywords:    ["pump"]
  fan_keywords:     ["fan"]
  coolant_keywords: ["coolant", "water", "liquid"]

# --- CPU ---------------------------------------------------------------------
cpu:
  # Preferred psutil sensor group for CPU temp (first match wins).
  temp_sensors: ["k10temp", "coretemp", "zenpower", "cpu_thermal"]
  # RAPL energy counters (µJ). Tried in order; first readable one wins.
  # AMD Zen exposes intel-rapl:0 via the same kernel driver as Intel.
  rapl_paths:
    - /sys/class/powercap/intel-rapl:0/energy_uj
"""


def cmd_init_config(path: Path, force: bool) -> int:
    if path.exists() and not force:
        print(f"{path} already exists. Pass --force to overwrite.", file=sys.stderr)
        return 1
    suggested, summary = _suggest_from_sensors()
    print(summary, file=sys.stderr)
    print(file=sys.stderr)
    path.parent.mkdir(parents=True, exist_ok=True)
    rendered = CONFIG_TEMPLATE.format(
        pump=suggested.get("pump", []),
        radiator_fans=suggested.get("radiator_fans", []),
        cpu_fan=suggested.get("cpu_fan", []),
        coolant_temp=suggested.get("coolant_temp", []),
    )
    path.write_text(rendered)
    print(f"Wrote {path}")
    print(f"Edit it, then run: panorama_sensor_display.py")
    return 0


# ============================== Main ==========================================

def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("--config", type=Path, default=DEFAULT_CONFIG_PATH,
                    help=f"Config file (default: {DEFAULT_CONFIG_PATH})")
    ap.add_argument("--detect", action="store_true",
                    help="Print detected sensors + suggested config, then exit.")
    ap.add_argument("--init-config", action="store_true",
                    help="Write a starter config file (uses --config path).")
    ap.add_argument("--force", action="store_true",
                    help="Overwrite --config file if it exists (with --init-config).")

    # Runtime overrides.
    ap.add_argument("--base-image", type=Path, default=None,
                    help="Override config's base_image.")
    ap.add_argument("--font", type=str, default=None,
                    help="Override config's font (TTF path).")
    ap.add_argument("--interval", type=float, default=None,
                    help="Override config's interval_seconds.")
    ap.add_argument("--brightness", type=int, default=None,
                    help="Override config's brightness (0-100).")
    ap.add_argument("--gpu", choices=("auto", "nvidia", "amd", "none"),
                    default=None, help="Override config's gpu collector.")
    ap.add_argument("--out", type=Path, default=FRAME_PATH_DEFAULT,
                    help="Path to write the rendered PNG.")
    ap.add_argument("--once", action="store_true",
                    help="Render one frame and exit (no device push).")
    args = ap.parse_args()

    if args.detect and not args.init_config:
        return cmd_detect()
    if args.init_config:
        return cmd_init_config(args.config, args.force)

    global CFG
    CFG = AppConfig.load(args.config)

    # Resolve effective values (CLI > config > default).
    interval   = args.interval   if args.interval   is not None else CFG.interval_seconds
    brightness = args.brightness if args.brightness is not None else CFG.brightness
    gpu_mode   = args.gpu        if args.gpu        is not None else CFG.gpu
    font_path  = args.font       if args.font       is not None else CFG.font
    if args.base_image is not None:
        base_image = args.base_image
    elif CFG.base_image:
        base_image = Path(CFG.base_image).expanduser()
    elif DEFAULT_BASE_IMAGE.exists():
        base_image = DEFAULT_BASE_IMAGE
    else:
        base_image = None

    # Prime counters that need two samples.
    psutil.cpu_percent(interval=None)
    cpu_power_watts()
    collect_network()
    time.sleep(0.5)

    def render_once() -> Image.Image:
        cpu = collect_cpu()
        if gpu_mode == "nvidia":
            gpu = collect_gpu_nvidia()
        elif gpu_mode == "amd":
            gpu = collect_gpu_amd()
        elif gpu_mode == "auto":
            gpu = collect_gpu_auto()
        else:
            gpu = GpuStats()
        cool = collect_cooling()
        ram  = collect_ram()
        net  = collect_network()
        return render_frame(base_image, font_path, cpu, gpu, cool, ram, net)

    if args.once:
        img = render_once()
        args.out.parent.mkdir(parents=True, exist_ok=True)
        img.save(args.out)
        print(f"Wrote {args.out}")
        return 0

    out_dir = args.out.parent
    stem = args.out.stem
    suffix = args.out.suffix or ".png"
    cleanup_leftover_frames(out_dir, stem, suffix)

    frames_on_device: deque = deque()
    cycle = 0

    while True:
        try:
            cycle += 1
            stamp = datetime.now().strftime("%Y%m%d_%H%M%S_%f")[:-3]
            frame_name = f"{stem}_{stamp}{suffix}"
            frame_path = out_dir / frame_name

            img = render_once()
            img.save(frame_path)
            size = frame_path.stat().st_size
            print(f"[cycle {cycle}] rendered {frame_name} ({size} bytes)",
                  flush=True)

            upload_and_display(frame_path, brightness)
            frames_on_device.append(frame_name)
            print(f"[cycle {cycle}] displayed {frame_name}", flush=True)

            while len(frames_on_device) > CFG.keep_on_device:
                old = frames_on_device.popleft()
                subprocess.run(
                    ["reed-tpse", "delete", old],
                    check=False, timeout=15,
                )
                try:
                    (out_dir / old).unlink()
                except OSError:
                    pass
                print(f"[cycle {cycle}] retired {old}", flush=True)
        except subprocess.CalledProcessError as e:
            print(f"reed-tpse failed: {e}", file=sys.stderr)
        except Exception as e:  # keep the loop alive on transient errors
            print(f"frame error: {e}", file=sys.stderr)
        time.sleep(interval)


if __name__ == "__main__":
    sys.exit(main())
