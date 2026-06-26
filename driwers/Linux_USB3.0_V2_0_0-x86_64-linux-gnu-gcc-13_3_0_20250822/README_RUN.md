# PLUG617R USB3.0 Linux SDK runner

This repository keeps the vendor `sdk/` directory unchanged. The local files under `src/`, `scripts/`, `bin/`, and `outputs/` provide a small runner and diagnostics around the vendor SDK.

## 1. Check video devices

```bash
./scripts/list_video_devices.sh
```

The camera must appear as a Linux V4L2 device, for example `/dev/video0` or `/dev/video4`.

## 2. Check video permissions

```bash
./scripts/setup_video_permissions.sh
```

If your user is not in the `video` group, the script prints the command to add it. It does not run `usermod` automatically.

## 3. Build

```bash
./scripts/build.sh
```

This builds:

```text
bin/plug617_stream_probe
```

## 4. Run one probe

Example with an explicit device:

```bash
./scripts/run_probe.sh --device /dev/video4 --mode 0 --version 1 --width 640 --height 512
```

`run_probe.sh` opens a live preview window by default. The preview uses `ffplay` and auto-detects the display format from the received buffer size/content:

- `gray8` when the SDK reports one byte per pixel, for example `640*512 = 327680`;
- `uyvy-luma` when the SDK reports UYVY-like data where U/V are nearly constant and the visible image is in the Y/luma bytes;
- `gray16be` when the SDK reports one 16-bit big-endian value per pixel, also commonly reported as `640*512 = 327680` because the SDK length is counted in `short` elements;
- `yuyv422` when the SDK reports enough bytes for YUV422.

For `gray16be`/`gray16le`, the runner converts each 16-bit frame to visible 8-bit grayscale before sending it to `ffplay`. Preview contrast normalization is enabled by default so 16-bit thermal values are visible instead of nearly black.

The live preview can also colorize grayscale/luma output:

```bash
./scripts/run_probe.sh --device /dev/video4 --mode 5 --version 1 --width 640 --height 512 --preview-format yuyv-luma --preview-palette blue-red
```

`blue-red` maps cold/dark values to blue and hot/bright values to red before sending `rgb24` frames to `ffplay`.

Disable the preview for headless diagnostics:

```bash
./scripts/run_probe.sh --device /dev/video4 --mode 0 --version 1 --no-preview
```

If the preview looks like a flat green image, the data is probably being interpreted as YUV422 when the SDK is actually returning one-byte grayscale. The default `auto` mode should avoid that. You can also force formats:

```bash
./scripts/run_probe.sh --device /dev/video4 --mode 0 --version 1 --preview-format uyvy-luma
./scripts/run_probe.sh --device /dev/video4 --mode 0 --version 1 --preview-format gray8
./scripts/run_probe.sh --device /dev/video4 --mode 0 --version 1 --preview-format gray16be --preview-length-units shorts
./scripts/run_probe.sh --device /dev/video4 --mode 0 --version 1 --preview-format yuyv422 --preview-length-units shorts
./scripts/run_probe.sh --device /dev/video4 --mode 0 --version 1 --preview-format uyvy422 --preview-length-units shorts
```

If `--device` is omitted, the script tries `/dev/v4l/by-id/*Infrared*`, then `v4l2-ctl --list-devices`, then falls back to `/dev/video0` with a warning.

The runner defaults are:

```text
device=/dev/video0
width=640
height=512
mode=0
version=1
frames=100
timeout-sec=10
save-frames=5
log-level=15
```

Direct runner example:

```bash
./bin/plug617_stream_probe --device /dev/video4 --width 640 --height 512 --mode 0 --version 1 --frames 100 --preview
```

## 5. Sweep SDK modes

```bash
./scripts/sweep_modes.sh --device /dev/video4 --width 640 --height 512
```

The sweep tries:

```text
video_mode: 0 1 2 3 5
device_version: 1 2 3
```

Each attempt runs with:

```text
--frames 20 --timeout-sec 5 --save-frames 1
```

Logs and saved frames are placed in `outputs/sweep_YYYYmmdd_HHMMSS/`.

## Output files

For the first saved frames, the runner writes files like:

```text
frame_000001_src.raw
frame_000001_yuv.raw
frame_000001_param.raw
frame_000001_param.txt
```

`frame_000001_param.txt` contains a conservative human-readable parse of the parameter line: headers, distance, emissivity, reflectivity, shutter status, hot/cold/cursor points, regional mean temperature, and the position of `0x6666` if present.

Important: the vendor `Demo.c` does not show whether `frame_src_data_length`, `frame_yuv_data_length`, and `paramLine_length` are measured in bytes or 16-bit words. This runner saves raw buffers by treating those length values as bytes to avoid reading past SDK buffers. If the SDK reports lengths in 16-bit words, raw dumps and parameter parsing may be conservative/truncated.

## Stopping the probe

Press `Ctrl+C` once to request a clean SDK shutdown. Press `Ctrl+C` again to force immediate process exit. During clean shutdown the runner also uses a short watchdog around `guide_usb_closeStream()` and `guide_usb_exit()` so a stuck SDK call does not leave the process running forever.
