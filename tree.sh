#!/usr/bin/env bash
set -euo pipefail

ROOT="${1:-.}"
MAX_DEPTH="${2:-99}"
IFS=, read -r -a IGNORE <<< "${IGNORE:-.git,target,node_modules,.vscode,dist,.idea}"

contains() {
  for e in "${IGNORE[@]}"; do [[ "$1" == "$e" ]] && return 0; done
  return 1
}

print_tree() {
  local dir="$1" depth="$2"
  local indent="$(printf '│   %.0s' $(seq 1 $depth))"

  for entry in "$dir"/* "$dir"/.[!.]* "$dir"/..?*; do
    [[ ! -e "$entry" ]] && continue
    local name="$(basename "$entry")"
    contains "$name" && continue
    printf '%s├── %s\n' "$indent" "$name"
    if [[ -d "$entry" && $depth -lt $MAX_DEPTH ]]; then
      print_tree "$entry" $((depth+1))
    fi
  done
}

{
  echo '```'
  print_tree "$ROOT" 0
  echo '```'
} > docs/arborescence.md
