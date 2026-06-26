#!/usr/bin/env bash
set -uo pipefail
shopt -s nullglob

section() {
  echo
  echo "==== $* ===="
}

section "User"
whoami

section "Groups"
groups

section "USB devices"
if command -v lsusb >/dev/null 2>&1; then
  lsusb
else
  echo "lsusb is not installed. Try:"
  echo "  sudo apt install usbutils"
fi

section "/dev/video*"
video_devices=(/dev/video*)
if ((${#video_devices[@]} == 0)); then
  echo "No /dev/video* devices found."
else
  ls -l "${video_devices[@]}"
fi

section "/dev/v4l/by-id"
if [[ -d /dev/v4l/by-id ]]; then
  ls -l /dev/v4l/by-id/
else
  echo "/dev/v4l/by-id does not exist."
fi

if ! command -v v4l2-ctl >/dev/null 2>&1; then
  section "v4l2-ctl"
  echo "v4l2-ctl is not installed. Try:"
  echo "  sudo apt install v4l-utils"
  exit 0
fi

section "v4l2-ctl --list-devices"
v4l2-ctl --list-devices || true

for dev in "${video_devices[@]}"; do
  section "$dev --all"
  v4l2-ctl --device="$dev" --all || true

  section "$dev --list-formats-ext"
  v4l2-ctl --device="$dev" --list-formats-ext || true
done
