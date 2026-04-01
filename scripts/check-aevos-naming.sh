#!/usr/bin/env bash
# 禁止在源码中出现特定第三方桌面产品名；检查 linux 兼容层目录存在。
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

bad=0
matches="$(grep -rE --include='*.c' --include='*.h' \
    'Windows[[:space:]]*7|Win7' src/ 2>/dev/null || true)"
if [[ -n "$matches" ]]; then
  printf '%s\n' "$matches" >&2
  echo "check-aevos-naming: forbidden vendor desktop name in src/" >&2
  bad=1
fi

if [[ ! -d src/linux ]]; then
  echo "check-aevos-naming: missing src/linux" >&2
  bad=1
fi

if [[ ! -f src/container/lc_layer.c ]]; then
  echo "check-aevos-naming: missing lc_layer.c" >&2
  bad=1
fi

exit "$bad"
