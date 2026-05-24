# Device Reference (TRYX cm01_se Tablet)

This document captures known hardware and ADB-level capabilities of the Android device used by this project. It is intended as a stable reference for developers and agents working on `reed-tpse`.

## Device Identity

| Property | Value |
|---|---|
| Model | TRYX cm01_se tablet |
| SoC | Rockchip RK3566 (rk356x platform, rk30board) |
| Android | 11 (build V1.0.3, dated 2025-03-07) |
| Kernel | Linux 4.19.232, armv8l, built 2025-02-27 |
| Serial | BYZL25042700200810 |
| Bootloader | Unlocked (`verifiedbootstate=orange`, `ro.adb.secure=0`) |

## Hardware Features Accessible via ADB

### CPU / Memory

- Quad-core ARM Cortex-A55 (ARMv8), 4 cores identical
- Hardware crypto: AES, SHA1/SHA2, PMULL, CRC32, NEON
- ~2 GB RAM, ~1.15 GB available; 1.5 GB ZRAM swap configured (unused)
- eMMC storage: 3.6 GB `/data` (15% used)

### Display

- 2240x1080 @ 60 Hz, 240 DPI
- Landscape + portrait both supported
- Screen brightness near max (250/255), timeout set to never (max int)

### Wireless (both OFF currently)

- Wi-Fi: hardware present, `wifi_hal_legacy` running, country code `CN`; supports Direct + Passpoint
- Bluetooth: hardware present (`vendor.bluetooth-1-0` HAL running), never enabled in the measured session

### Camera

- Declared: front camera, rear camera, autofocus
- Camera HAL uses lazy loading (`enableLazyHal=true`), camera service starts on demand
- No `/dev/video*` nodes exposed to shell (V4L2 not surfaced at shell level)

### Sensors

- Accelerometer declared in feature list
- `vendor.sensors-hal-1-0` running
- `/dev/iio:device0` present (IIO sensor device accessible)
- `ro.audio.monitorOrientation=true` (audio policy responds to orientation)

### Audio

- Full audio stack running (`audioserver`, `vendor.audio-hal`)
- Active at capture time: `com.baiyi.homeui.tkcfanhomeui` holds `AUDIO_FOCUS` for `USAGE_MEDIA`
- Volume levels are accessible/configurable via `settings put system volume_*`

### GPIO / Hardware I/O

- GPIO chips: `gpiochip0`, `gpiochip32`, `gpiochip64`, `gpiochip96`, `gpiochip128`, `gpiochip511`
- GPIO export/unexport available at `/sys/class/gpio/`
- I2C buses: `/dev/i2c-0`, `/dev/i2c-2`
- RTC: `/dev/rtc0`
- Watchdog: `/dev/watchdog`, `/dev/watchdog0`
- Hardware random: `/dev/hw_random`
- TEE devices: `/dev/tee0`, `/dev/teepriv0`, `/dev/opteearmtz00`
- RGA (Rockchip image processing accelerator): `/dev/rga`
- MPP (Rockchip media processing): `/dev/mpp_service`
- Mali GPU: `/dev/mali0`
- Serial ports: `/dev/ttyFIQ0`, `/dev/ttyGS0`, `/dev/ttyS1`, `/dev/ttyS4`
- USB FFS / Accessory: `/dev/usb-ffs`, `/dev/usb_accessory`

### USB

- Connected in ACM + ADB mode (`persist.sys.usb.config=acm,adb`)
- USB host mode hardware supported (feature declared)
- USB mass storage enabled
- `ttyGS0` is USB CDC ACM serial gadget (host-visible serial port)

### Input

- Single input device visible: `/dev/input/event0`
- No touchscreen, buttons, or keyboard input devices visible at shell level

### Graphics

- Vulkan 1.1 (level 1, version 4198400), with Vulkan compute
- OpenGL ES 3.2 (AEP)
- Hardware composer: `vendor.hwcomposer-2-1`
- Gralloc 4.0

## Software / ADB-Level Access

| Feature | Status |
|---|---|
| Shell access | `uid=2000(shell)` (non-root) |
| Root | No (`su` not available) |
| `/data` access | Denied at shell level |
| `ro.adb.secure` | `0` (no auth required for ADB) |
| Boot unlock | Yes (`orange` verified boot state) |
| Package management | `pm install/uninstall`, `pm grant/revoke` |
| Activity manager | `am start`, `am broadcast`, `am force-stop`, etc. |
| Settings read/write | `settings get/put global/system/secure` |
| Screen control | `wm size`, `wm density`, `input tap/swipe`, `screencap` |
| File push/pull | `adb push/pull` to `/sdcard/` |
| Logcat | Full log access |
| App sideloading | Yes (no signature enforcement at install) |
| Serial data service | `com.baiyi.service.serialservice.serialdataservice` running |
| Factory mode app | `com.baiyi.app.factorymode` present |
| Custom home UI | `com.baiyi.homeui.tkcfanhomeui` running and holding media focus |
| OTA service | `android.rockchip.update.service` present |

## Notable Characteristics

1. Industrial/embedded Android tablet profile, not consumer-oriented (`cm01_se` + `com.baiyi.*` stack).
2. Serial communication is first-class (`acm,adb`, dedicated serial data service), indicating USB serial bridge use cases.
3. Bootloader is unlocked (`verifiedbootstate=orange`, `ro.boot.flash.locked=0`), enabling custom flashing workflows.
4. No cellular stack (`ro.boot.noril=true`, `ril.function.dataonly=1`), likely Wi-Fi/ethernet or offline deployment profile.
5. Always-on kiosk behavior (`screen_off_timeout=2147483647`).
6. Wi-Fi region/build metadata indicates a Chinese industrial firmware origin.

## Operational Notes for `reed-tpse`

- The project depends on both serial transport (CDC ACM) and ADB file operations; this device supports both in parallel.
- Access is shell-only (non-root), so implementation should avoid assumptions about privileged paths (`/data`) or root-only tools.
- ADB security is permissive (`ro.adb.secure=0`) on this unit; do not assume that policy on other firmware variants.
