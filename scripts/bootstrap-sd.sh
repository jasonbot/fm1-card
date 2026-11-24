#!/usr/bin/env bash
# Prepare a microSD card for fm1-card.
#
# Creates the directory layout, writes config.json, and optionally imports
# .fwsc packages. Safe to re-run: existing config/packages are kept unless
# --force is given.
set -euo pipefail

DEFAULT_MOUNT="/run/media/${USER:-user}/FM1CARD"

mount_point=""
ssid="${FM1_WIFI_SSID:-}"
password="${FM1_WIFI_PASSWORD:-}"
force=0
dry_run=0
no_prompt=0
import_dir=""

usage() {
  cat <<'EOF'
usage: bootstrap-sd.sh [MOUNT] [options]

  MOUNT                 SD mount point (default: /run/media/$USER/FM1CARD)
  --ssid SSID           Wi-Fi SSID; default from FM1_WIFI_SSID
  --password PASS       Wi-Fi password; default from FM1_WIFI_PASSWORD
  --import DIR          copy DIR/*.fwsc into fm1-card/firmwares
  --force               overwrite an existing config.json
  --no-prompt           never ask for Wi-Fi credentials
  --dry-run             print actions without writing
  -h, --help            this help
EOF
}

die() {
  printf 'error: %s\n' "$*" >&2
  exit 1
}

while (($#)); do
  case "$1" in
    --ssid) ssid="${2:?--ssid needs a value}"; shift 2 ;;
    --password) password="${2:?--password needs a value}"; shift 2 ;;
    --import) import_dir="${2:?--import needs a directory}"; shift 2 ;;
    --force) force=1; shift ;;
    --no-prompt) no_prompt=1; shift ;;
    --dry-run) dry_run=1; shift ;;
    -h|--help) usage; exit 0 ;;
    --*) die "unknown option $1" ;;
    *) [[ -z "$mount_point" ]] || die "mount point already given"; mount_point="$1"; shift ;;
  esac
done

[[ -n "$mount_point" ]] || mount_point="$DEFAULT_MOUNT"
[[ -d "$mount_point" ]] || die "not a directory: $mount_point"
[[ -w "$mount_point" ]] || die "not writable: $mount_point"
[[ "$(readlink -f "$mount_point")" != "/" ]] || die "refusing to write to /"

source="$(findmnt -n -o SOURCE -T "$mount_point" 2>/dev/null || true)"
fstype="$(findmnt -n -o FSTYPE -T "$mount_point" 2>/dev/null || true)"
[[ "$source" == /dev/* ]] || die "$mount_point is not on a block device (source=${source:-unknown})"
[[ "$fstype" == "vfat" || "$fstype" == "exfat" ]] || die "expected vfat/exfat, found ${fstype:-unknown}"

base="$mount_point/fm1-card"
config="$base/config.json"

if ((!no_prompt)) && [[ -z "$ssid" && -t 0 ]]; then
  printf 'Wi-Fi SSID (leave empty for SD-only): '
  read -r ssid
  if [[ -n "$ssid" ]]; then
    printf 'Wi-Fi password: '
    IFS= read -r -s password
    printf '\n'
  fi
fi

run() {
  if ((dry_run)); then
    printf 'dry-run:'
    printf ' %q' "$@"
    printf '\n'
  else
    "$@"
  fi
}

run mkdir -p "$base/firmwares" "$base/backups" "$base/logs"

if [[ -f "$config" ]] && ((!force)); then
  printf 'keeping existing %s (use --force to replace)\n' "$config"
else
  if ((dry_run)); then
    printf 'dry-run: write %s (ssid=%q password=%s)\n' \
      "$config" "$ssid" "${password:+<set>}"
  else
    tmp="$config.tmp.$$"
    python3 - "$tmp" "$ssid" "$password" <<'PY'
import json, sys
path, ssid, password = sys.argv[1:4]
with open(path, "w", encoding="utf-8") as file:
    json.dump({"wifi": {"ssid": ssid, "password": password}}, file, indent=2)
    file.write("\n")
PY
    mv "$tmp" "$config"
    printf 'wrote %s\n' "$config"
  fi
fi

readme="$base/README.txt"
if [[ ! -f "$readme" ]]; then
  if ((dry_run)); then
    printf 'dry-run: write %s\n' "$readme"
  else
    cat >"$readme" <<'EOF'
fm1-card SD layout

  config.json         Wi-Fi SSID/password; read at boot
  firmwares.json      local firmware manifest (source of truth)
  firmwares/*.fwsc    firmware packages
  backups/*.bin       full-flash backups
  logs/               optional device logs

Put .fwsc files in fm1-card/firmwares or in a top-level firmwares/
directory. The local manifest is loaded at boot and after a REST upload.
Devices -> select a unit -> 2 FLASH picks a local package.
EOF
  fi
fi

if [[ -n "$import_dir" ]]; then
  [[ -d "$import_dir" ]] || die "import directory not found: $import_dir"
  shopt -s nullglob
  packages=("$import_dir"/*.fwsc)
  shopt -u nullglob
  if ((${#packages[@]} == 0)); then
    printf 'no .fwsc files in %s\n' "$import_dir"
  fi
  for package in "${packages[@]}"; do
    target="$base/firmwares/$(basename "$package")"
    if [[ -f "$target" ]] && ((!force)); then
      printf 'keeping %s\n' "$target"
    else
      run cp -f "$package" "$target"
      if ((!dry_run)); then
        printf 'imported %s\n' "$target"
      fi
    fi
  done
fi

# Report packages already on the card; validates the marker layout cheaply.
python3 - "$base/firmwares" <<'PY'
import pathlib, sys
root = pathlib.Path(sys.argv[1])
if not root.is_dir():
    raise SystemExit
for path in sorted(root.glob("*.fwsc")):
    data = path.read_bytes()[:0x30 * 20]
    blocks = 20
    while blocks < 64 and len(data) > blocks * 0x30 + 0x2f and data[blocks * 0x30 + 0x2f] == 0x7d:
        blocks += 1
    product = []
    for i in range(blocks):
        marker = data[i * 0x30 + 0x2f]
        if marker != 0x7d:
            product.append(chr((marker - i - 1) & 0xff))
    label = "".join(product) or "unknown"
    print(f"  {path.name}: {label}, {path.stat().st_size} bytes")
PY

printf '\nready: %s (%s, %s)\n' "$base" "$source" "$fstype"
df -h "$mount_point" | tail -1
