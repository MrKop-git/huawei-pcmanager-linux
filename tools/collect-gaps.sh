#!/usr/bin/env bash
# 跑一遍目标程序，把「Wine 缺什么」一次性收集成清单。
#
# 为什么不用字符串扫二进制
# ------------------------
# 实测：扫 PCManager 目录能得到 698 个"缺失的 WinRT 类名"，但其中绝大多数是
# SDK 元数据带进来的接口/类名，**实际被激活的只有个位数**。噪声比信号大两个量级。
#
# 可靠信号是**运行期**：Wine 每遇到一个它没有的类/函数就吐一行日志。跑一遍就收全。
#
# 用法
#   ./collect-gaps.sh --prefix ~/pcm-wine [--seconds 60] [--out /tmp/gaps]
#
# 产出
#   <out>/all.log              原始日志
#   <out>/gap-winrt.txt        WinRT 缺的类（RoGetActivationFactory 找不到库）
#   <out>/gap-api.txt          Wine 未实现的 API（unimplemented function）
#   <out>/gap-dll.txt          缺的 DLL 模块
#   <out>/gap-stub.txt         被调用但是半成品的存根（需要人工判断是否要紧）

set -euo pipefail

PREFIX_ARG="$HOME/pcm-wine"
SECONDS_RUN=60
OUT=/tmp/gaps

while [ $# -gt 0 ]; do
  case "$1" in
    --prefix)  PREFIX_ARG="$2"; shift 2 ;;
    --seconds) SECONDS_RUN="$2"; shift 2 ;;
    --out)     OUT="$2"; shift 2 ;;
    -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
    *) echo "未知参数: $1"; exit 1 ;;
  esac
done

PREFIX="$(readlink -f "${PREFIX_ARG/#\~/$HOME}")"
export WINEPREFIX="$PREFIX"
export WINEARCH=win64
export DISPLAY="${DISPLAY:-:0}"
# 保留我们自己的垫片，这样收的是"补完之后还缺什么"
export WINEDLLOVERRIDES="${WINEDLLOVERRIDES:-mscoree=;wlanapi=n}"

PCM="$PREFIX/drive_c/Program Files/Huawei/PCManager"
mkdir -p "$OUT"
: > "$OUT/all.log"

# 挑"最会碰 WinRT/网络"的几个跑：连接服务、分布式主服务、主程序
TARGETS=(
  "$PCM/HiConnectivityService.exe"
  "$PCM/HwDistributedMainService.exe"
  "$PCM/PCManager.exe"
)

echo "==> 收集中（每个跑 ${SECONDS_RUN}s）"
for exe in "${TARGETS[@]}"; do
  [ -f "$exe" ] || { echo "  跳过（不存在）: $exe"; continue; }
  echo "--- $(basename "$exe")"
  echo "########## $(basename "$exe") ##########" >> "$OUT/all.log"
  wineserver -k 2>/dev/null || true
  sleep 2
  # 服务 exe 手动跑会因 SCDW 拿不到 SCM 而退出（ERROR_ACCESS_DENIED=5），
  # 但它在退出前已经把 WinRT/RPC 该试的都试了 —— 收缺口够用。
  ( cd /home/wangyi && timeout "$SECONDS_RUN" wine "$exe" \
      >> "$OUT/all.log" 2>&1 ) || true
done
wineserver -k 2>/dev/null || true

echo
echo "==> 汇总"

grep -aoE 'Failed to find library for L"[^"]+"' "$OUT/all.log" 2>/dev/null \
  | sed 's/Failed to find library for L"//; s/"$//' | sort -u > "$OUT/gap-winrt.txt"

grep -aoE 'unimplemented function [A-Za-z0-9_.]+' "$OUT/all.log" 2>/dev/null \
  | sed 's/unimplemented function //' | sort -u > "$OUT/gap-api.txt"

grep -aoE 'Library [A-Za-z0-9_.-]+\.(dll|DLL) .*not found|Failed to load .*' "$OUT/all.log" 2>/dev/null \
  | sort -u > "$OUT/gap-dll.txt"

grep -aoE 'fixme:[a-z]+:[A-Za-z_]+[^ ]* (semi-)?stub' "$OUT/all.log" 2>/dev/null \
  | sort -u | head -60 > "$OUT/gap-stub.txt"

for f in gap-winrt gap-api gap-dll gap-stub; do
  n=$(wc -l < "$OUT/$f.txt" 2>/dev/null || echo 0)
  printf '  %-12s %3d 条   %s\n' "$f" "$n" "$OUT/$f.txt"
done

echo
if [ -s "$OUT/gap-winrt.txt" ]; then
  echo "==> ❌ 缺的 WinRT 类（就是垫片要补的清单）"
  sed 's/^/   /' "$OUT/gap-winrt.txt"
else
  echo "==> ✅ 没收到 WinRT 缺类"
fi
echo
if [ -s "$OUT/gap-api.txt" ]; then
  echo "==> ❌ Wine 未实现的 API"
  sed 's/^/   /' "$OUT/gap-api.txt"
fi
