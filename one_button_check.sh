#!/usr/bin/env bash
set -euo pipefail

# --- config ---
ASSETS_DIR="assets"
WORK_DIR="work"
PYTHON="${PYTHON:-python3}"

# --- helpers ---
have() { command -v "$1" >/dev/null 2>&1; }
ptime() {  # portable-ish timer wrapper
  if have /usr/bin/time; then
    /usr/bin/time -f "  -> time: %E, mem: %M KB" "$@"
  else
    time "$@"
  fi
}

process_case() {
  local name="$1"  # e.g. small_example / large_example
  local in_tsv="${ASSETS_DIR}/${name}.tsv"
  local bin="${WORK_DIR}/${name}.bin"
  local out_tsv="${WORK_DIR}/${name}.out.tsv"

  if [[ ! -f "$in_tsv" ]]; then
    echo "[skip] ${in_tsv} not found"
    return 0
  fi

  echo "== ${name} =="
  echo "[1/4] serialize: ${in_tsv} -> ${bin}"
  ptime make serialize IN="$in_tsv" OUT="$bin"

  echo "[2/4] deserialize: ${bin} -> ${out_tsv}"
  ptime make deserialize IN="$bin" OUT="$out_tsv"

  echo "[3/4] sizes:"
  bytes=$(du -b "$bin" | awk '{print $1}')
  du -h "$bin" | sed "s|\$| (${bytes} bytes)|" | sed 's/^/  /'
  wc -l "$in_tsv" "$out_tsv" | sed 's/^/  /' | tail -n +1

  echo "[4/4] edge multiset check:"
  if [[ -f "check_edges.py" ]]; then
    $PYTHON check_edges.py "$in_tsv" "$out_tsv" || true
  else
    echo "  (check_edges.py not found — skipping check)"
  fi

  echo
}

main() {
  mkdir -p "$WORK_DIR"

  echo "== build =="
  make build
  echo

  process_case "small_example"
  process_case "large_example"

  echo "Done. Artifacts in: ${WORK_DIR}/"
}

main "$@"
