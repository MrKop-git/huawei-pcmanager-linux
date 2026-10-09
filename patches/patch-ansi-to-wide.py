#!/usr/bin/env python3
"""把 PE 二进制里对 ANSI 版 Win32 API 的字符串引用改成 W 版。

背景
----
Wine 的 advapi32 只导出 `NotifyServiceStatusChangeW`，没有 `NotifyServiceStatusChangeA`。
而华为电脑管家（在 `libdistribute_service_monitor.dll` 里）动态解析 A 版，
调用即 `wine: unimplemented function ... aborting` 崩溃。

由于 A/W 两个名字**长度完全相同**，可以直接原地替换字节，不需要改任何偏移。
（若名字长度不同则本工具会拒绝执行。）

用法
----
    python3 patch-ansi-to-wide.py <文件或目录> [--name NotifyServiceStatusChange] [--dry-run]

行为
----
- 自动跳过已有的 `.bak`
- 默认先备份成 `<file>.bak`（已存在则不覆盖）
- 打印每个文件替换了几处
"""

from __future__ import annotations

import argparse
import shutil
import sys
from pathlib import Path

# 默认处理的目标：A → W，长度必须相同
DEFAULT_NAME = "NotifyServiceStatusChange"

# 这些后缀的文件不碰
SKIP_SUFFIXES = {".bak", ".png", ".bmp", ".jpg", ".ico", ".dat", ".ini", ".xml", ".json", ".txt"}


def patch_file(path: Path, name: str, dry_run: bool) -> int:
    ansi = (name + "A").encode("ascii")
    wide = (name + "W").encode("ascii")

    if len(ansi) != len(wide):
        raise SystemExit(f"名字长度不同（{ansi!r} vs {wide!r}），不能原地替换")

    data = path.read_bytes()
    count = data.count(ansi)
    if count == 0:
        return 0

    if dry_run:
        print(f"[dry-run] {path}: 将替换 {count} 处")
        return count

    backup = path.with_suffix(path.suffix + ".bak")
    if not backup.exists():
        shutil.copy2(path, backup)

    path.write_bytes(data.replace(ansi, wide))

    # 自检：替换后不应该再有 A 版，且 W 版数量应等于原 A 版数量
    after = path.read_bytes()
    left = after.count(ansi)
    if left:
        raise SystemExit(f"{path}: 替换后仍残留 {left} 处 A 版，请人工检查")

    print(f"{path}: 替换 {count} 处  (备份 {backup.name})")
    return count


def iter_targets(root: Path):
    if root.is_file():
        yield root
        return
    for p in sorted(root.rglob("*")):
        if not p.is_file():
            continue
        if p.suffix.lower() in SKIP_SUFFIXES or p.suffix.lower().endswith(".bak"):
            continue
        yield p


def main() -> int:
    ap = argparse.ArgumentParser(description="把 ANSI 版 API 引用改成 W 版（同长度原地替换）")
    ap.add_argument("targets", nargs="+", help="文件或目录")
    ap.add_argument("--name", default=DEFAULT_NAME,
                    help=f"API 基名，不含 A/W（默认 {DEFAULT_NAME}）")
    ap.add_argument("--dry-run", action="store_true", help="只报告，不写盘")
    args = ap.parse_args()

    total = 0
    touched = 0
    for t in args.targets:
        root = Path(t)
        if not root.exists():
            print(f"跳过（不存在）: {root}", file=sys.stderr)
            continue
        for f in iter_targets(root):
            try:
                n = patch_file(f, args.name, args.dry_run)
            except OSError as e:
                print(f"跳过 {f}: {e}", file=sys.stderr)
                continue
            if n:
                total += n
                touched += 1

    verb = "将替换" if args.dry_run else "已替换"
    print(f"\n{verb} {total} 处，涉及 {touched} 个文件")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
