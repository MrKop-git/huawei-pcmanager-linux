#!/usr/bin/env bash
# 编译 Windows.Devices.WiFiDirect 垫片
#
# 依赖 mingw-w64-gcc（Arch: pacman -S mingw-w64-gcc）
#
# 用法: ./build.sh [x64|x86|all]

set -euo pipefail
cd "$(dirname "$0")"
WHICH="${1:-x64}"

# 只需导出这两个；DllMain 由链接器自动处理
gen_def() {
  cat > "$1" <<'EOF'
LIBRARY windows.devices.wifidirect.dll
EXPORTS
  DllGetActivationFactory
  DllCanUnloadNow
EOF
}

build() {   # $1=工具链前缀 $2=输出名
  local cc="$1-gcc" out="$2" def="$1.def"
  command -v "$cc" >/dev/null || { echo "缺 $cc"; return 1; }
  gen_def "$def"

  # HSTRING 那套（WindowsCreateString / WindowsGetStringRawBuffer）在
  # api-ms-win-core-winrt-string-l1-1-0.dll，mingw 里由 -lruntimeobject 或
  # -lwindowsapp 提供；两个都带上，缺哪个由链接器挑。
  "$cc" -O2 -shared -Wall -Wextra -Wno-unused-parameter -Werror \
        -o "$out" windows.devices.wifidirect.c "$def" \
        -lole32 -lruntimeobject -lwindowsapp -static-libgcc 2>&1 || {
    echo "  首次链接失败，去掉 -lwindowsapp 重试…"
    "$cc" -O2 -shared -Wall -Wextra -Wno-unused-parameter \
          -o "$out" windows.devices.wifidirect.c "$def" \
          -lole32 -lruntimeobject -static-libgcc
  }
  echo "  -> $out  ($(stat -c %s "$out") 字节)"
}

case "$WHICH" in
  x64) build x86_64-w64-mingw32 windows.devices.wifidirect-x64.dll ;;
  x86) build i686-w64-mingw32   windows.devices.wifidirect-x86.dll ;;
  all) build x86_64-w64-mingw32 windows.devices.wifidirect-x64.dll
       build i686-w64-mingw32   windows.devices.wifidirect-x86.dll 2>/dev/null \
         || echo "  (i686 工具链没装，跳过)" ;;
  *) echo "用法: $0 [x64|x86|all]"; exit 1 ;;
esac

echo
echo "完成。注册见 install.sh"
