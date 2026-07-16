#!/usr/bin/env bash
# One-shot installer for the panorama-overlay contrib.
# Assumes reed-tpse itself is already installed and working
# (`reed-tpse list` should succeed).

set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONFIG_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/panorama-overlay"

echo ">> Installing Python dependencies"
pip3 install --user -r "$REPO_DIR/requirements.txt" \
  || pip3 install --break-system-packages -r "$REPO_DIR/requirements.txt"

echo ">> Installing udev rule for CPU RAPL energy counter"
sudo cp "$REPO_DIR/udev/99-rapl-readable.rules" /etc/udev/rules.d/
sudo udevadm control --reload-rules
sudo udevadm trigger --subsystem-match=powercap || true

echo ">> Seeding config at $CONFIG_DIR/config.yaml"
mkdir -p "$CONFIG_DIR"
if [[ ! -f "$CONFIG_DIR/config.yaml" ]]; then
  cp "$REPO_DIR/config.example.yaml" "$CONFIG_DIR/config.yaml"
  echo "   Wrote starter config. Edit it, or run:"
  echo "       python3 $REPO_DIR/panorama_sensor_display.py --detect"
  echo "   to see your fan/pump channel names."
else
  echo "   Kept existing $CONFIG_DIR/config.yaml (not overwritten)."
fi

echo ">> Installing systemd user unit"
mkdir -p "$HOME/.config/systemd/user"
# Rewrite the unit's %h-relative paths to point at THIS checkout.
sed "s|%h/reed-tpse/contrib/panorama-overlay|$REPO_DIR|g" \
    "$REPO_DIR/panorama-overlay.service" \
    > "$HOME/.config/systemd/user/panorama-overlay.service"

systemctl --user daemon-reload
systemctl --user enable --now panorama-overlay.service

echo ">> Enabling linger so the service survives logout"
sudo loginctl enable-linger "$USER" || true

cat <<EOF

Done. Useful commands:
  journalctl --user -u panorama-overlay.service -f    # follow logs
  systemctl --user restart panorama-overlay.service   # apply config changes
  python3 $REPO_DIR/panorama_sensor_display.py --once --out /tmp/preview.png
EOF
