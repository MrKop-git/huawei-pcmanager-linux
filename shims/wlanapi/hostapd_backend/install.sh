#!/usr/bin/env bash
# 安装 wlanapi 垫片的 Linux 侧后端。
#
#   ./install.sh --prefix ~/pcm-wine [--iface wlan0]
#
# 会做：
#   1. 装 wlanapi-shimd 到 ~/.local/bin
#   2. 按你的 prefix/iface 生成并装 systemd user 单元
#   3. 起服务，并跑一次自检
#
# 卸载：./install.sh --uninstall

set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"

PREFIX_ARG=""
IFACE="wlan0"
UNINSTALL=0

while [ $# -gt 0 ]; do
  case "$1" in
    --prefix)    PREFIX_ARG="$2"; shift 2 ;;
    --iface)     IFACE="$2"; shift 2 ;;
    --uninstall) UNINSTALL=1; shift ;;
    -h|--help)   sed -n '2,12p' "$0"; exit 0 ;;
    *) echo "未知参数: $1"; exit 1 ;;
  esac
done

UNIT_DIR="$HOME/.config/systemd/user"
UNIT="wlanapi-shimd.service"
BIN_DIR="$HOME/.local/bin"

if [ "$UNINSTALL" = 1 ]; then
  systemctl --user disable --now "$UNIT" 2>/dev/null || true
  rm -f "$UNIT_DIR/$UNIT" "$BIN_DIR/wlanapi-shimd"
  systemctl --user daemon-reload
  echo "已卸载（~/.wlanapi-shim 的运行数据保留，要清就 rm -rf）"
  exit 0
fi

[ -n "$PREFIX_ARG" ] || { echo "必须给 --prefix（例如 --prefix ~/pcm-wine）"; exit 1; }
PREFIX="$(readlink -f "${PREFIX_ARG/#\~/$HOME}")"
[ -d "$PREFIX/drive_c" ] || { echo "不是有效的 Wine 前缀: $PREFIX"; exit 1; }

echo "==> 依赖检查"
missing=0
for c in hostapd iw ip; do
  if command -v "$c" >/dev/null; then printf '  %-10s ✅ %s\n' "$c" "$(command -v "$c")"
  else printf '  %-10s ❌ 缺失: pacman -S %s\n' "$c" "$c"; missing=1; fi
done
[ "$missing" = 0 ] || echo "  （缺的装完再跑本脚本）"

echo "==> 安装 wlanapi-shimd"
mkdir -p "$BIN_DIR" "$UNIT_DIR"
install -m755 "$HERE/wlanapi_shimd.py" "$BIN_DIR/wlanapi-shimd"
echo "  -> $BIN_DIR/wlanapi-shimd"

echo "==> 生成 systemd user 单元（prefix=$PREFIX iface=$IFACE）"
sed -e "s|%h/pcm-wine|$PREFIX|g" \
    -e "s|--iface wlan0|--iface $IFACE|" \
    "$HERE/$UNIT" > "$UNIT_DIR/$UNIT"
# 把默认的 iface 参数插进去（模板里没有，这里补）
if ! grep -q -- '--iface' "$UNIT_DIR/$UNIT"; then
  sed -i "s|^ExecStart=\(.*\)$|ExecStart=\1 --iface $IFACE|" "$UNIT_DIR/$UNIT"
fi
# ReadWritePaths 里的前缀也要跟着换
sed -i "s|^ReadWritePaths=%h/pcm-wine/drive_c/wlanapi_shim$|ReadWritePaths=$PREFIX/drive_c/wlanapi_shim|" "$UNIT_DIR/$UNIT"
grep -E '^ExecStart=|^ReadWritePaths=' "$UNIT_DIR/$UNIT" | sed 's/^/  /'

echo "==> 起服务"
systemctl --user daemon-reload
systemctl --user enable --now "$UNIT"
sleep 2
systemctl --user --no-pager status "$UNIT" | head -12 || true

echo
echo "==> 自检"
"$BIN_DIR/wlanapi-shimd" --prefix "$PREFIX" --iface "$IFACE" --init
echo "--- 通信目录 ---"
ls -la "$PREFIX/drive_c/wlanapi_shim/" 2>/dev/null || true
echo "--- 状态 ---"
cat "$PREFIX/drive_c/wlanapi_shim/status.json" 2>/dev/null && echo || echo "(还没有状态)"
echo "--- 日志尾 ---"
tail -6 "$HOME/.wlanapi-shim/shimd.log" 2>/dev/null || echo "(还没日志)"

cat <<EOF

完成。
  看状态: cat $PREFIX/drive_c/wlanapi_shim/status.json
  看日志: tail -f ~/.wlanapi-shim/shimd.log
  干跑  : wlanapi-shimd --prefix $PREFIX --iface $IFACE --once --dry-run
EOF
