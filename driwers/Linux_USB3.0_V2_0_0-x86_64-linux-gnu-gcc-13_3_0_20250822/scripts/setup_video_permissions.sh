#!/usr/bin/env bash
set -uo pipefail
shopt -s nullglob

echo "Current user: $(whoami)"
echo "Current groups:"
groups

echo
echo "/dev/video* permissions:"
video_devices=(/dev/video*)
if ((${#video_devices[@]} == 0)); then
  echo "No /dev/video* devices found."
else
  ls -l "${video_devices[@]}"
fi

echo
if id -nG "$USER" | tr ' ' '\n' | grep -qx video; then
  echo "User $USER is already in the video group."
else
  echo "User $USER is not in the video group."
  echo
  echo "To add the user to the video group, run:"
  echo "  sudo usermod -aG video $USER"
  echo
  echo "After that, log out and log in again, or reboot, so the new group membership is applied."
fi
