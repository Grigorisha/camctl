#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ROOT_DIR=$(cd -- "$SCRIPT_DIR/.." && pwd)

device=""
width=640
height=512
mode=0
version=1
frames=100
timeout_sec=10
save_frames=5
log_level=15
preview=1
preview_fps=25
preview_format=auto
preview_length_units=auto
preview_palette=gray
preview_normalize=1
child_pid=""
run_dir=""

usage() {
  cat <<USAGE
Usage: $0 [options]

Options:
  --device /dev/videoX
  --mode N
  --version N
  --width N
  --height N
  --frames N
  --timeout-sec N
  --save-frames N
  --log-level N
  --preview
  --no-preview
  --preview-fps N
  --preview-format auto|gray8|uyvy-luma|yuyv-luma|yuyv422|uyvy422|gray16le|gray16be
  --preview-length-units auto|bytes|shorts
  --preview-palette gray|blue-red
  --preview-normalize
  --no-preview-normalize
USAGE
}

stop_child() {
  local signal_name="${1:-INT}"

  if [[ -n "${child_pid:-}" ]] && kill -0 "$child_pid" 2>/dev/null; then
    if [[ -n "${run_dir:-}" ]]; then
      echo "Signal received, forwarding $signal_name to probe pid $child_pid..." | tee -a "$run_dir/run.log" >&2
    else
      echo "Signal received, forwarding $signal_name to probe pid $child_pid..." >&2
    fi

    kill "-$signal_name" "$child_pid" 2>/dev/null || true

    for _ in {1..30}; do
      if ! kill -0 "$child_pid" 2>/dev/null; then
        return 0
      fi
      sleep 0.1
    done

    echo "Probe did not stop after $signal_name, sending TERM..." >&2
    kill -TERM "$child_pid" 2>/dev/null || true

    for _ in {1..20}; do
      if ! kill -0 "$child_pid" 2>/dev/null; then
        return 0
      fi
      sleep 0.1
    done

    echo "Probe still did not stop, sending KILL..." >&2
    kill -KILL "$child_pid" 2>/dev/null || true
  fi
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
    --mode)
      mode="${2:?missing value for --mode}"
      shift 2
      ;;
    --version)
      version="${2:?missing value for --version}"
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
    --preview)
      preview=1
      shift
      ;;
    --no-preview)
      preview=0
      shift
      ;;
    --preview-fps)
      preview_fps="${2:?missing value for --preview-fps}"
      shift 2
      ;;
    --preview-format)
      preview_format="${2:?missing value for --preview-format}"
      shift 2
      ;;
    --preview-length-units)
      preview_length_units="${2:?missing value for --preview-length-units}"
      shift 2
      ;;
    --preview-palette)
      preview_palette="${2:?missing value for --preview-palette}"
      shift 2
      ;;
    --preview-normalize)
      preview_normalize=1
      shift
      ;;
    --no-preview-normalize)
      preview_normalize=0
      shift
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

run_dir="outputs/run_$(date +%Y%m%d_%H%M%S)"
mkdir -p "$run_dir"

export LD_LIBRARY_PATH="$ROOT_DIR/sdk/lib:${LD_LIBRARY_PATH:-}"

cmd=(
  "$ROOT_DIR/bin/plug617_stream_probe"
  --device "$device"
  --width "$width"
  --height "$height"
  --mode "$mode"
  --version "$version"
  --frames "$frames"
  --timeout-sec "$timeout_sec"
  --out "$run_dir"
  --save-frames "$save_frames"
  --log-level "$log_level"
  --preview-fps "$preview_fps"
  --preview-format "$preview_format"
  --preview-length-units "$preview_length_units"
  --preview-palette "$preview_palette"
)

if ((preview)); then
  cmd+=(--preview)
else
  cmd+=(--no-preview)
fi

if ((preview_normalize)); then
  cmd+=(--preview-normalize)
else
  cmd+=(--no-preview-normalize)
fi

{
  echo "Run directory: $ROOT_DIR/$run_dir"
  echo "Command:"
  printf '  %q' "${cmd[@]}"
  echo
  echo
} | tee "$run_dir/run.log"

set +e
trap 'stop_child INT' INT
trap 'stop_child TERM' TERM
"${cmd[@]}" > >(tee -a "$run_dir/run.log") 2>&1 &
child_pid=$!
wait "$child_pid"
exit_code=$?
trap - INT TERM
set -e

echo "exit_code=$exit_code" | tee -a "$run_dir/run.log"
exit "$exit_code"
