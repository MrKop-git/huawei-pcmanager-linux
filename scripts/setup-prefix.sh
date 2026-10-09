#!/usr/bin/env bash
# 建一个"能跑华为电脑管家安装器"的 Wine 前缀。
#
# 把本项目一路踩出来的坑固化成脚本：
#   1. 干净 win64 前缀 + Windows 10
#   2. 微软原生 VC++ 运行库   —— 否则 msvcp140._Chmod 未实现，安装器崩
#   3. 中文字体              —— 否则界面全是方框
#   4. 微软原生 gdiplus      —— 否则皮肤背景不画
#   5. 原生 IE8 (Trident)    —— 否则许可协议页（CLSID_HTMLDocument）一片空白
#   6. 关掉崩溃对话框        —— 否则每次崩都弹模态框卡死安装
#   7. 钉 DPI 96
#
# 用法:
#   WINEPREFIX=~/pcm-wine ./setup-prefix.sh
#
# 注意:
#   - 需要 winetricks + cabextract
#   - ie8 全部依赖 web.archive.org；若直连不通，请先让它走代理
#   - 前缀里需要放一份 Noto CJK；脚本会自动从系统找

set -euo pipefail

WINEPREFIX="${WINEPREFIX:-$HOME/pcm-wine}"
export WINEPREFIX
export WINEARCH=win64
export WINEDEBUG="${WINEDEBUG:--all}"
export WINEDLLOVERRIDES="mscoree="   # 只跳过 Mono 提示；★不要禁 mshtml★

say() { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }
warn() { printf '\033[1;33m[warn] %s\033[0m\n' "$*"; }

command -v wine       >/dev/null || { echo "缺 wine";        exit 1; }
command -v winetricks >/dev/null || { echo "缺 winetricks";  exit 1; }
command -v cabextract >/dev/null || { echo "缺 cabextract";  exit 1; }

say "建前缀: $WINEPREFIX"
wineserver -k 2>/dev/null || true
sleep 2
if [ ! -d "$WINEPREFIX" ]; then
  wineboot -u
fi

say "设成 Windows 10"
wine reg add 'HKCU\Software\Wine' /v Version /d win10 /f >/dev/null

say "钉 DPI = 96"
wine reg add 'HKCU\Control Panel\Desktop' /v LogPixels /t REG_DWORD /d 96 /f >/dev/null
wine reg add 'HKCU\Software\Wine\X11 Driver' /v Dpi /d 96 /f >/dev/null

say "关掉崩溃对话框（否则崩一次就卡死安装）"
wine reg add 'HKCU\Software\Wine\WineDbg' /v ShowCrashDialog /t REG_DWORD /d 0 /f >/dev/null

say "装微软原生 VC++ 运行库（修 msvcp140._Chmod）"
winetricks -q vcrun2022

say "装微软原生 gdiplus（修皮肤背景）"
winetricks -q gdiplus

say "装中文字体（修方框）"
FONTDIR="$WINEPREFIX/drive_c/windows/Fonts"
mkdir -p "$FONTDIR"
COPIED=0
for src in /usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc \
           /usr/share/fonts/noto-cjk/NotoSerifCJK-Regular.ttc; do
  if [ -f "$src" ]; then cp -n "$src" "$FONTDIR/" && COPIED=$((COPIED+1)); fi
done
if [ "$COPIED" -eq 0 ]; then
  warn "没找到 Noto CJK。请自行把任意中文 TTC/TTF 放进 $FONTDIR"
fi
# 把程序常要的字体名映射到已装的中文
for pair in \
  "Microsoft YaHei=Noto Sans CJK SC" \
  "微软雅黑=Noto Sans CJK SC" \
  "SimSun=Noto Sans CJK SC" \
  "宋体=Noto Sans CJK SC" \
  "SimHei=Noto Sans CJK SC" \
  "黑体=Noto Sans CJK SC" \
  "NSimSun=Noto Sans CJK SC" ; do
  k="${pair%%=*}"; v="${pair##*=}"
  wine reg add 'HKLM\Software\Microsoft\Windows NT\CurrentVersion\FontSubstitutes' \
       /v "$k" /d "$v" /f >/dev/null
done

say "装原生 IE8（修许可协议页 CLSID_HTMLDocument）"
if winetricks -q ie8; then
  :
else
  warn "ie8 安装失败 —— 多半是 web.archive.org 拉不下来。"
  warn "先确认它能走代理：curl -sI https://web.archive.org/ | head -1"
fi

say "完成。检查一下："
for f in syswow64/msvcp140.dll syswow64/mshtml.dll syswow64/gdiplus.dll; do
  p="$WINEPREFIX/drive_c/windows/$f"
  [ -f "$p" ] && printf '  %-28s %10s 字节\n' "$f" "$(stat -c %s "$p")"
done
echo
echo "字体数: $(ls "$FONTDIR" 2>/dev/null | wc -l)"
echo
echo "下一步：用 bootstrap 跑安装器（见 docs/install.md）"
