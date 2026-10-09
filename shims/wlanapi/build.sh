#!/usr/bin/env bash
# 编译 wlanapi 垫片（64 位 + 32 位）
#
# 依赖: mingw-w64-gcc
#   Arch: pacman -S mingw-w64-gcc
#
# 用法:
#   ./build.sh              # 两个位数都编
#   ./build.sh x64          # 只编 64 位

set -euo pipefail
cd "$(dirname "$0")"

WHICH="${1:-all}"

# 导出表：从 PCManager 全树里 objdump 出来的、实际被导入的 Wlan* 面。
# 少一个导出 → 调用方整个模块加载失败，所以这张表要跟实测对齐。
read -r -d '' EXPORTS <<'EOF' || true
WlanCloseHandle
WlanConnect
WlanDeleteProfile
WlanDisconnect
WlanEnumInterfaces
WlanFreeMemory
WlanGetAvailableNetworkList
WlanGetNetworkBssList
WlanGetProfile
WlanGetProfileList
WlanHostedNetworkForceStart
WlanHostedNetworkForceStop
WlanHostedNetworkInitSettings
WlanHostedNetworkQueryProperty
WlanHostedNetworkQuerySecondaryKey
WlanHostedNetworkQueryStatus
WlanHostedNetworkRefreshSecuritySettings
WlanHostedNetworkSetProperty
WlanHostedNetworkSetSecondaryKey
WlanHostedNetworkStopUsing
WlanIhvControl
WlanOpenHandle
WlanQueryInterface
WlanReasonCodeToString
WlanRegisterNotification
WlanScan
WlanSetInterface
WlanSetProfile
WlanSetProfilePosition
WlanSetSecuritySettings
EOF

gen_def() {
  local def="$1"
  { echo "LIBRARY wlanapi.dll"
    echo "EXPORTS"
    printf '%s\n' $EXPORTS | sed 's/^/  /'
  } > "$def"
  echo "生成 $def（$(printf '%s\n' $EXPORTS | wc -l) 个导出）"
}

build() {  # $1=前缀 $2=输出名
  local cc="$1-gcc" out="$2" def="$1.def"
  command -v "$cc" >/dev/null || { echo "缺 $cc"; return 1; }
  gen_def "$def"
  "$cc" -O2 -shared -Wall -Wextra -Wno-unused-parameter \
        -o "$out" wlanapi_shim.c scdw_hook.c "$def" \
        -lole32 -ladvapi32 -static-libgcc
  echo "  -> $out  ($(stat -c %s "$out") 字节)"
}

case "$WHICH" in
  x64)  build x86_64-w64-mingw32  wlanapi-x64.dll ;;
  x86)  build i686-w64-mingw32    wlanapi-x86.dll ;;
  all)  build x86_64-w64-mingw32  wlanapi-x64.dll
        build i686-w64-mingw32    wlanapi-x86.dll 2>/dev/null \
          || echo "  (i686 工具链没装，跳过 32 位)" ;;
  *)    echo "用法: $0 [all|x64|x86]"; exit 1 ;;
esac

echo
echo "完成。部署见 README.md"
