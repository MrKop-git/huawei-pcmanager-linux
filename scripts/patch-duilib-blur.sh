#!/usr/bin/env bash
# 关掉 DuiLib 窗口的毛玻璃，修 Wine 下的"窗口透明度不对"。
#
# 背景
# ----
# 电脑管家的界面是 DuiLib 写的。窗口定义里有 blur="true"，走的是微软**未公开**的
# SetWindowCompositionAttribute + AccentState（ACCENT_ENABLE_ACRYLICBLURBEHIND）。
# Wine 没实现这个 API，于是毛玻璃做不出来，窗口却仍按"背后有模糊层"来画 ——
# 表现就是窗口半透明、把桌面透出来。
#
# 把 blur 关掉，DuiLib 就不走那条路径，窗口恢复正常不透明。
#
# 用法
# ----
#   WINEPREFIX=~/pcm-wine ./patch-duilib-blur.sh [--restore]
#
#   --restore  从 .orig 备份还原

set -euo pipefail

WINEPREFIX="${WINEPREFIX:-$HOME/pcm-wine}"
PCMMGR="$WINEPREFIX/drive_c/Program Files/Huawei/PCManager"
LAYOUT="$PCMMGR/res/layout"

[ -d "$LAYOUT" ] || { echo "找不到 layout 目录: $LAYOUT"; exit 1; }

if [ "${1:-}" = "--restore" ]; then
  n=0
  while IFS= read -r -d '' f; do
    if [ -f "$f.orig" ]; then
      mv "$f.orig" "$f"
      printf '  还原: %s\n' "${f#$LAYOUT/}"
      n=$((n+1))
    fi
  done < <(find "$LAYOUT" -name '*.orig' -print0)
  echo "还原 $n 个文件"
  exit 0
fi

echo "关掉 DuiLib 毛玻璃（blur=true -> false）"
n=0
# ★ 目录名带空格，必须用 -print0 + read -d ''，不能用 for 遍历
while IFS= read -r -d '' f; do
  cp -n "$f" "$f.orig" 2>/dev/null || true
  perl -pi -e 's/blur="true"/blur="false"/g' "$f"
  printf '  改: %s\n' "${f#$LAYOUT/}"
  n=$((n+1))
done < <(grep -rlZ 'blur="true"' "$LAYOUT" 2>/dev/null || true)

echo "共 $n 个文件（备份为同目录 .orig）"

echo
echo "验证（排除 .orig）:"
printf '  仍为 true:  %s\n' "$(grep -rlZ --exclude='*.orig' 'blur="true"'  "$LAYOUT" 2>/dev/null | tr -d -c '\0' | wc -c)"
printf '  已为 false: %s\n' "$(grep -rlZ --exclude='*.orig' 'blur="false"' "$LAYOUT" 2>/dev/null | tr -d -c '\0' | wc -c)"

cat <<'EOF'

注意：这些是**被改过的程序文件**，电脑管家自我修复/重新安装会覆盖掉，需要重跑。

重启电脑管家生效：
  wineserver -k; wine "$WINEPREFIX/drive_c/Program Files/Huawei/PCManager/PCManager.exe"
EOF
