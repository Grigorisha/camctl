#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ROOT_DIR=$(cd -- "$SCRIPT_DIR/.." && pwd)

device=""
width=640
height=512
frames=20
timeout_sec=5
save_frames=1
log_level=15
modes=(0 1 2 3 5)
versions=(1 2 3)

usage() {
  cat <<USAGE
Usage: $0 [options]

Options:
  --device /dev/videoX
  --width N
  --height N
  --frames N          default: $frames
  --timeout-sec N     default: $timeout_sec
  --save-frames N     default: $save_frames
  --log-level N       default: $log_level
USAGE
}

detect_device() {
  local path

  for path in /dev/v4l/by-id/*Infrared* /dev/v4l/by-id/*infrared*; do
    if [[ -e "$path" ]]; then
      readlink -f "$path"
      return 0
    fi
  done

  if command -v v4l2-ctl >/dev/null 2>&1; then
    v4l2-ctl --list-devices 2>/dev/null | awk '
      /InfraredCamera|Infrared|infrared|PLUG617R|Guide|GUIDE/ { found=1; next }
      found && /\/dev\/video[0-9]+/ { print $1; exit }
      NF == 0 { found=0 }
    '
  fi
}

while (($#)); do
  case "$1" in
    --device)
      device="${2:?missing value for --device}"
      shift 2
      ;;
    --width)
      width="${2:?missing value for --width}"
      shift 2
      ;;
    --height)
      height="${2:?missing value for --height}"
      shift 2
      ;;
    --frames)
      frames="${2:?missing value for --frames}"
      shift 2
      ;;
    --timeout-sec)
      timeout_sec="${2:?missing value for --timeout-sec}"
      shift 2
      ;;
    --save-frames)
      save_frames="${2:?missing value for --save-frames}"
      shift 2
      ;;
    --log-level)
      log_level="${2:?missing value for --log-level}"
      shift 2
      ;;
    --help|-h)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage
      exit 2
      ;;
  esac
done

cd "$ROOT_DIR"

if [[ -z "$device" ]]; then
  device=$(detect_device || true)
  if [[ -z "$device" ]]; then
    device="/dev/video0"
    echo "WARNING: could not auto-detect InfraredCamera, using $device" >&2
  else
    echo "Auto-detected device: $device"
  fi
fi

if [[ ! -x bin/plug617_stream_probe ]]; then
  echo "bin/plug617_stream_probe is missing, building it first..."
  "$ROOT_DIR/scripts/build.sh"
fi

export LD_LIBRARY_PATH="$ROOT_DIR/sdk/lib:${LD_LIBRARY_PATH:-}"

sweep_dir="outputs/sweep_$(date +%Y%m%d_%H%M%S)"
mkdir -p "$sweep_dir"

summary_file="$sweep_dir/summary.tsv"
printf "mode\tversion\texit_code\tframes_received\tstatus\n" > "$summary_file"

echo "Sweep directory: $ROOT_DIR/$sweep_dir"
echo
printf "%-6s %-8s %-9s %-15s %s\n" "mode" "version" "exit" "frames_received" "status"

for mode in "${modes[@]}"; do
  for version in "${versions[@]}"; do
    log_file="$sweep_dir/mode_${mode}_version_${version}.log"
    attempt_out="$sweep_dir/mode_${mode}_version_${version}_frames"
    mkdir -p "$attempt_out"

    cmd=(
      "$ROOT_DIR/bin/plug617_stream_probe"
      --device "$device"
      --width "$width"
      --height "$height"
      --mode "$mode"
      --version "$version"
      --frames "$frames"
      --timeout-sec "$timeout_sec"
      --out "$attempt_out"
      --save-frames "$save_frames"
      --log-level "$log_level"
    )

    {
      echo "Command:"
      printf '  %q' "${cmd[@]}"
      echo
      echo
    } > "$log_file"

    set +e
    "${cmd[@]}" >> "$log_file" 2>&1
    exit_code=$?
    set -e

    frames_received=$(grep -cE '^frame [0-9]+:' "$log_file" || true)
    if grep -q 'status=completed' "$log_file"; then
      status="completed"
    elif grep -q 'status=timeout' "$log_file"; then
      status="timeout"
    elif grep -q 'status=interrupted' "$log_file"; then
      status="interrupted"
    elif ((frames_received > 0)); then
      status="frames_seen"
    else
      status="no_frames"
    fi

    printf "%s\t%s\t%s\t%s\t%s\n" "$mode" "$version" "$exit_code" "$frames_received" "$status" >> "$summary_file"
    printf "%-6s %-8s %-9s %-15s %s\n" "$mode" "$version" "$exit_code" "$frames_received" "$status"
  done
done

echo
echo "Summary saved to: $ROOT_DIR/$summary_file"
