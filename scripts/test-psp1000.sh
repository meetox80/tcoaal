#!/usr/bin/env bash
set -u
root="$(cd "$(dirname "$0")/.." && pwd)"
eboot="$(realpath "${1:-$root/out/PSP/GAME/TCOAAL/EBOOT.PBP}")"
cases=(intro:300 graves:400 maps:1800)

command -v PPSSPPSDL >/dev/null || { echo "PPSSPPSDL not found"; exit 2; }
[ -f "$eboot" ] || { echo "no EBOOT at $eboot"; exit 2; }

work="$(mktemp -d "${TMPDIR:-/tmp}/tcoaal-psp1000.XXXXXX")"
cleanup() { [ "${KEEP:-0}" = 1 ] && echo "logs kept in $work" || rm -rf "$work"; }
trap cleanup EXIT

run_case() {
  local name="$1" secs="$2"
  local game="$work/$name/ppsspp/PSP/GAME/TCOAAL" system="$work/$name/ppsspp/PSP/SYSTEM"
  mkdir -p "$game/saves" "$system"
  ln -s "$eboot" "$game/EBOOT.PBP"
  cp "$root/engine/tests/psp1000/$name.txt" "$game/input.txt"
  printf -- '--input input.txt --perf\n' >"$game/args.txt"
  printf '[General]\nFirstRun = False\nCheckForNewVersion = False\n[SystemParam]\nPSPModel = 0\n[Sound]\nEnable = False\n' >"$system/ppsspp.ini"
  SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy XDG_CONFIG_HOME="$work/$name" \
    timeout "$secs" PPSSPPSDL --pause-menu-exit "$game/EBOOT.PBP" >"$work/$name/ppsspp.log" 2>&1
}

pids=()
for c in "${cases[@]}"; do
  run_case "${c%%:*}" "${c##*:}" &
  pids+=($!)
done
wait "${pids[@]}"

failed=0
for c in "${cases[@]}"; do
  name="${c%%:*}"
  log="$work/$name/ppsspp/PSP/GAME/TCOAAL/log.txt"
  problems=()
  peak=0
  slowest=0
  if [ -f "$log" ]; then
    grep -q '^exit after' "$log" || problems+=("did not reach the end of the script")
    peak="$(grep -o 'heap used [0-9]*' "$log" | awk '$3 > m { m = $3 } END { print m + 0 }')"
    slowest="$(grep -o 'perf [0-9.]* fps' "$log" | awk 'NR == 1 || $2 < m { m = $2 } END { print m + 0 }')"
    skipped="$(grep -c 'skipped (' "$log")"
    missing="$(grep -c '^missing image' "$log")"
    [ "$skipped" = 0 ] || problems+=("$skipped image loads ran out of memory and were retried")
    [ "$missing" = 0 ] || problems+=("$missing images never loaded")
  else
    problems+=("no log written")
  fi
  if [ ${#problems[@]} = 0 ]; then
    printf '%-7s ok, peak heap %s KB, slowest %s fps\n' "$name" "$peak" "$slowest"
  else
    failed=1
    printf '%-7s failed, peak heap %s KB\n' "$name" "$peak"
    for p in "${problems[@]}"; do printf '        %s\n' "$p"; done
    [ -f "$log" ] && grep 'skipped (\|^missing image' "$log" | sort | uniq -c | head -5 | sed 's/^/        /'
  fi
done
exit $failed
