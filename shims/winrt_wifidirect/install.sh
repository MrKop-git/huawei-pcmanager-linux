#!/usr/bin/env bash
# 把 Windows.Devices.WiFiDirect 垫片注册进 Wine 前缀。
#
#   ./install.sh --prefix ~/pcm-wine
#   ./install.sh --prefix ~/pcm-wine --uninstall
#
# 原理：Wine 用注册表解析 WinRT 类 —— combase 读到
#
#   HKLM\Software\Microsoft\WindowsRuntime\ActivatableClassId\<类名>
#       DllPath = <dll 路径>
#
# 就会加载那个 DLL 并调它的 DllGetActivationFactory。
# 所以我们把自己的 DLL 放进前缀、写一条注册表即可，**不用改 Wine**。

set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"

PREFIX_ARG=""
UNINSTALL=0
while [ $# -gt 0 ]; do
  case "$1" in
    --prefix)    PREFIX_ARG="$2"; shift 2 ;;
    --uninstall) UNINSTALL=1; shift ;;
    -h|--help)   sed -n '2,12p' "$0"; exit 0 ;;
    *) echo "未知参数: $1"; exit 1 ;;
  esac
done

# 本垫片实现的所有 WinRT 类。加类就往这里加一行 —— 用
#   tools/collect-gaps.sh
# 收运行期缺口（combase 会吐 "Failed to find library for L\"...\""）
CLASSES=(
  # —— 真正实现了行为的 ——
  'Windows.Devices.WiFiDirect.WiFiDirectAdvertisementPublisher'
  'Windows.Devices.WiFiDirect.WiFiDirectConnectionListener'
  # —— 只给骨架（上层会拿到 E_NOINTERFACE 并走自己的错误分支）——
  'Windows.Devices.Bluetooth.Advertisement.BluetoothLEAdvertisementPublisher'
  'Windows.Devices.Bluetooth.Advertisement.BluetoothLEManufacturerData'
  'Windows.Devices.Bluetooth.GenericAttributeProfile.GattServiceProvider'
  'Windows.Devices.Bluetooth.GenericAttributeProfile.GattSession'
  'Windows.Devices.Bluetooth.GenericAttributeProfile.GattLocalCharacteristic'
  'Windows.Devices.Bluetooth.GenericAttributeProfile.GattLocalCharacteristicParameters'
  'Windows.Devices.Bluetooth.GenericAttributeProfile.GattServiceProviderAdvertisingParameters'
  'Windows.Devices.WiFiDirect.WiFiDirectDevice'
  'Windows.Networking.Connectivity.NetworkInformation'
  'Windows.Networking.NetworkOperators.NetworkOperatorTetheringManager'
  'Windows.Security.Credentials.PasswordCredential'
  'Windows.Security.Credentials.PasswordVault'
  'Windows.Storage.Streams.DataReader'
  'Windows.Storage.Streams.DataWriter'
)
DLL_NAME='windows.devices.wifidirect.dll'

class_key() { echo "HKLM\\Software\\Microsoft\\WindowsRuntime\\ActivatableClassId\\$1"; }

[ -n "$PREFIX_ARG" ] || { echo "必须给 --prefix"; exit 1; }
PREFIX="$(readlink -f "${PREFIX_ARG/#\~/$HOME}")"
export WINEPREFIX="$PREFIX"
[ -d "$PREFIX/drive_c" ] || { echo "不是有效的 Wine 前缀: $PREFIX"; exit 1; }

if [ "$UNINSTALL" = 1 ]; then
  for c in "${CLASSES[@]}"; do
    wine reg delete "$(class_key "$c")" /f >/dev/null 2>&1 || true
  done
  rm -f "$PREFIX/drive_c/windows/system32/$DLL_NAME"
  echo "已注销 ${#CLASSES[@]} 个类并删除 DLL"
  exit 0
fi

SRC="$HERE/windows.devices.wifidirect-x64.dll"
[ -f "$SRC" ] || { echo "先跑 ./build.sh"; exit 1; }

echo "==> 部署 DLL"
cp -f "$SRC" "$PREFIX/drive_c/windows/system32/$DLL_NAME"
echo "  -> C:\\windows\\system32\\$DLL_NAME"

echo "==> 注册 ${#CLASSES[@]} 个 WinRT 类"
for c in "${CLASSES[@]}"; do
  wine reg add "$(class_key "$c")" /v DllPath /t REG_SZ \
       /d "C:\\windows\\system32\\$DLL_NAME" /f >/dev/null 2>&1
  printf '  %s\n' "$c"
done

echo "==> 核对"
for c in "${CLASSES[@]}"; do
  if wine reg query "$(class_key "$c")" >/dev/null 2>&1; then
    printf '  ✅ %s\n' "$c"
  else
    printf '  ❌ %s\n' "$c"
  fi
done

cat <<EOF

完成。验证：
  1. 日志默认写 C:\\wifidirect_shim.log（可用环境变量 WIFIDIRECT_SHIM_LOG 改）
  2. 跑一次用到它的程序，看日志里有没有 "WiFiDirect 垫片加载"
     wine "\$WINEPREFIX/drive_c/Program Files/Huawei/PCManager/HiConnectivityService.exe"

卸载：./install.sh --prefix $PREFIX --uninstall
EOF
