# reed-tpse

Linux CLI for controlling the Tryx Panorama SE AIO cooler display.

Protocol reverse-engineered from KANALI Windows software and Android APKs.

## Currently supported features

- Upload images, videos, and GIFs (auto-converts to MP4)
- Set display content and brightness
- List and delete media files on device
- systemd user service for persistent display across reboots
- Auto-detects device (scans /dev/ttyACM*)
- Minimal dependencies (picojson header-only)

## TODO

- [ ] CPU stats overlay (temperature, usage, clock speed)
- [ ] GPU stats (temperature, usage, VRAM, clock speed)
- [ ] RAM usage
- [ ] Fan/pump RPM display
- [ ] Network throughput
- [ ] Custom overlay layouts

I believe the stats features bove (like CPU stats) should not be too difficult, but I think the image generation (given stats, how do we generate images dynamically with stats rendered on them?) part will be quite challenging. Hopefully the team at Tryx release a Linux Kanali build before we have to implement these features lol.

## Requirements

**Build:**
- CMake >= 3.16
- C++17 compiler (GCC 8+ / Clang 7+)

**Runtime:**
- `adb` - for file transfer (android-tools on Arch, adb on Debian/Ubuntu)
- `ffmpeg` - for GIF to MP4 conversion (.gif don't seem to work, so we convert any .gif uploaded to mp4 under-the-hood)

**Permissions:**
- User must be in `uucp` group (Arch) or `dialout` (Debian/Ubuntu) for serial access
- Or run with sudo

## Build

```bash
cd reed-tpse
mkdir build && cd build
cmake ..
make
sudo make install
```

## Usage

```bash
# Upload media to device
reed-tpse upload video.mp4
reed-tpse upload animation.gif  # Auto-converts to MP4

# Set display and start daemon (recommended)
reed-tpse display video.mp4 --brightness 80
reed-tpse daemon start

# That's it. Display persists across reboots.
```

### All commands

```bash
reed-tpse info                   # Show device info
reed-tpse upload <file>          # Upload media file
reed-tpse display <file>         # Set display content
reed-tpse brightness <0-100>     # Adjust brightness
reed-tpse list                   # List files on device
reed-tpse delete <file>          # Delete file from device
reed-tpse daemon start           # Start background keepalive
reed-tpse daemon stop            # Stop daemon
reed-tpse daemon status          # Check daemon status
```

## Configuration

Config: `~/.config/reed-tpse/config.json`

```json
{"brightness":100,"keepalive_interval":10}
```

Port is auto-detected by default. To pin a specific port:
```json
{"port":"/dev/ttyACM1","brightness":100,"keepalive_interval":10}
```

Display state (for daemon): `~/.local/state/reed-tpse/display.json`

## Architecture

```
reed-tpse/
├── include/reed/      # Public headers (libreed)
│   ├── picojson.h     # JSON parser (header-only, third-party)
│   ├── protocol.hpp   # Frame protocol
│   ├── device.hpp     # Serial device communication
│   ├── adb.hpp        # ADB wrapper
│   ├── media.hpp      # Media type detection, GIF conversion
│   └── config.hpp     # XDG config/state management
├── src/               # Library implementation
├── cli/               # CLI frontend
└── systemd/           # systemd user service
```

The core functionality is in `libreed.a`. The CLI links against it. Future GUI (maybe in v2.0.0?) will also link against the same library.

## How it works

This is a TLDR on how it works. I plan to write a blog post on this some time in the future. Will update this section once the blog post is live.

The Tryx Panorama SE exposes:
1. **USB CDC ACM** (`/dev/ttyACM0`): Serial interface for display commands
2. **ADB**: Android Debug Bridge for file transfer to `/sdcard/pcMedia/`

The device requires periodic keepalive (~60s timeout) or it reverts to the default screen. The daemon runs in the background (~1MB RAM, negligible CPU, I bet you could run this on a potato and not notice it) and handles this automatically.

## Tested on

| Distro | Kernel | CPU | GPU | Contributor |
|--------|--------|-----|-----|-------------|
| Arch Linux | 6.17.9 | Intel Core Ultra 9 285K | NVIDIA RTX 5080 | [@fadli0029](https://github.com/fadli0029) |

If you've tested on a different system, feel free to add yours via PR.

## License

MIT

## Contributing

Issues and pull requests welcome at https://github.com/fadli0029/reed-tpse
