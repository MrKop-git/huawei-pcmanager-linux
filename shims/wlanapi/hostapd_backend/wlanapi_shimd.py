#!/usr/bin/env python3
"""wlanapi-shimd —— wlanapi 垫片的 Linux 侧后端。

职责
----
把垫片（Wine 里的 wlanapi.dll）发来的请求，变成 Linux 原生的网络动作：

    请求(start)  →  起 hostapd 开热点  +  ip addr add 192.168.137.1/24
    请求(stop)   →  停 hostapd        +  移除 IP

设计要点
--------
* **文件通信，不用 socket**：垫片写好 req.json 就行，无需在 Windows 侧做 IPC。
  副作用是 Linux 这半可以**完全独立测试**（不启动 Wine 也能跑），这点很重要 ——
  Wine 调试一轮要几十秒，能独立测就别拖上它。
* **纯标准库**：不引入任何第三方依赖。
* **安全默认**：
    - 只碰你显式指定的网卡（默认 wlan0）
    - 开热点前先查网卡是否支持 AP 模式，不支持就诚实报错
    - 绝不动别的网卡、绝不动已有连接
    - stop 会无条件清理自己起的东西（幂等）

用法
----
    wlanapi_shimd.py --prefix ~/pcm-wine            # 前台跑
    wlanapi_shimd.py --prefix ~/pcm-wine --once     # 处理完当前请求就退出（便于测试）
    wlanapi_shimd.py --dry-run --prefix ~/pcm-wine  # 只打印将要执行的命令，不真跑

systemd user 单元见同目录 wlanapi-shimd.service
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path

# ---------------------------------------------------------------- 常量

# 多屏协同 PC 侧用的地址（与华为在 Windows 上的做法一致）
DEFAULT_AP_IP = "192.168.137.1"
DEFAULT_AP_CIDR = DEFAULT_AP_IP + "/24"
DEFAULT_IFACE = "wlan0"

# 我们自己的运行目录（配置、pid、日志），不污染系统
RUNDIR = Path(os.environ.get("WLANAPI_SHIM_RUNDIR", Path.home() / ".wlanapi-shim"))

SSID_MAX = 32
# WPA2 口令长度限制（8..63 ASCII）
KEY_MIN, KEY_MAX = 8, 63


def log(msg: str) -> None:
    ts = time.strftime("%Y-%m-%d %H:%M:%S")
    line = f"{ts} {msg}"
    print(line, flush=True)
    try:
        RUNDIR.mkdir(parents=True, exist_ok=True)
        with open(RUNDIR / "shimd.log", "a") as f:
            f.write(line + "\n")
    except OSError:
        pass


# ---------------------------------------------------------------- 执行

class Runner:
    """包一层命令执行：--dry-run 时只打印不执行，便于先看清楚要动什么。"""

    def __init__(self, dry_run: bool = False):
        self.dry_run = dry_run

    def run(self, *argv: str, check: bool = False) -> tuple[int, str]:
        cmd = list(argv)
        if self.dry_run:
            log(f"[dry-run] {' '.join(cmd)}")
            return 0, ""
        try:
            p = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
            out = (p.stdout + p.stderr).strip()
            if out:
                log(f"  $ {' '.join(cmd)}  -> rc={p.returncode} {out[:300]}")
            if check and p.returncode != 0:
                raise RuntimeError(f"{cmd[0]} 失败 rc={p.returncode}: {out[:200]}")
            return p.returncode, out
        except FileNotFoundError:
            raise RuntimeError(f"找不到命令: {cmd[0]}")
        except subprocess.TimeoutExpired:
            raise RuntimeError(f"命令超时: {' '.join(cmd)}")


# ---------------------------------------------------------------- 网卡

def iface_exists(iface: str) -> bool:
    return Path(f"/sys/class/net/{iface}").exists()


def iface_ap_capable(iface: str, r: Runner) -> tuple[bool, str]:
    """查网卡是否支持 AP 模式。用 iw，没有就退化成"未知"。"""
    if not shutil.which("iw"):
        return True, "没有 iw，跳过 AP 能力检查（假定支持）"
    rc, out = r.run("iw", "list")
    if rc != 0:
        return True, "iw list 失败，跳过检查"
    # iw list 里每个 phy 段落的 "Supported interface modes" 下列出 * AP
    m = re.search(r"Supported interface modes:(.*?)(?:\n\s*\n|\Z)", out, re.S)
    if not m:
        return True, "iw list 里没找到接口模式段，跳过检查"
    seg = m.group(1)
    return ("AP" in seg), ("网卡支持 AP 模式" if "AP" in seg else "网卡不支持 AP 模式")


def iface_is_busy(iface: str, r: Runner) -> bool:
    """网卡是否已经接在某个网络上（有 IP 或有连接）。"""
    rc, out = r.run("ip", "-brief", "addr", "show", iface)
    if rc != 0:
        return False
    return "UP" in out and "inet " in out


# ---------------------------------------------------------------- 热点

class Hotspot:
    def __init__(self, iface: str, runner: Runner):
        self.iface = iface
        self.r = runner
        self.conf = RUNDIR / "hostapd.conf"
        self.pidfile = RUNDIR / "hostapd.pid"
        self.logfile = RUNDIR / "hostapd.log"

    # ---- 配置

    def write_conf(self, ssid: str, key: str) -> None:
        """写 hostapd 配置。

        注意：这里用的是**我们自己的**SSID/口令，来自上层传来的值。
        华为客户端通常会要求一组自己的 SSID/WPA2 口令，我们照它给的配。
        """
        RUNDIR.mkdir(parents=True, exist_ok=True)
        # 值里不能有换行 —— 否则会注入配置
        ssid = ssid.replace("\n", " ").replace("\r", " ")[:SSID_MAX]
        key = key.replace("\n", "").replace("\r", "")
        body = f"""# 由 wlanapi-shimd 生成 —— 不要手改（会被覆盖）
interface={self.iface}
driver=nl80211
ssid={ssid}
hw_mode=g
channel=6
ieee80211n=1
wpa=2
wpa_passphrase={key}
wpa_key_mgmt=WPA-PSK
rsn_pairwise=CCMP
# 多屏协同的接收端要能被发现，别隐藏
ignore_broadcast_ssid=0
"""
        if self.r.dry_run:
            log(f"[dry-run] 写 {self.conf}:\n{body}")
            return
        self.conf.write_text(body)
        os.chmod(self.conf, 0o600)   # 含口令
        log(f"写好配置 {self.conf}（ssid={ssid}）")

    # ---- 起停

    def start(self, ssid: str, key: str) -> None:
        if not shutil.which("hostapd"):
            raise RuntimeError("没装 hostapd（pacman -S hostapd）")

        ok, why = iface_ap_capable(self.iface, self.r)
        if not ok:
            raise RuntimeError(f"{self.iface}: {why}")
        log(f"网卡检查: {why}")

        if iface_is_busy(self.iface, self.r):
            # 不停已有连接 —— 那是用户的网络，动了就是事故
            raise RuntimeError(
                f"{self.iface} 当前已连接到某个网络，拒绝抢占。"
                f"请先断开该网卡，或用 --iface 指定另一块网卡")

        self.write_conf(ssid, key)

        if self.r.dry_run:
            log(f"[dry-run] hostapd -B -P {self.pidfile} {self.conf}")
            log(f"[dry-run] ip addr add {DEFAULT_AP_CIDR} dev {self.iface}")
            return

        r = self.r
        # 提到前台会不会卡住？用 -B（后台）+ pidfile
        rc, out = r.run("hostapd", "-B", "-P", str(self.pidfile),
                        "-f", str(self.logfile), str(self.conf))
        if rc != 0:
            raise RuntimeError(f"hostapd 起不来 rc={rc}: {out[:200]}")

        # 等它真的起来（hostapd -B 会立刻返回）
        for _ in range(20):
            if self.pidfile.exists():
                break
            time.sleep(0.25)

        r.run("ip", "addr", "add", DEFAULT_AP_CIDR, "dev", self.iface)
        r.run("ip", "link", "set", self.iface, "up")
        log(f"热点已起: ssid={ssid} iface={self.iface} ip={DEFAULT_AP_IP}")

    def stop(self) -> None:
        r = self.r
        # 幂等：pidfile 不在也不报错
        if self.pidfile.exists():
            try:
                pid = int(self.pidfile.read_text().strip())
                if r.dry_run:
                    log(f"[dry-run] kill {pid}")
                else:
                    os.kill(pid, signal.SIGTERM)
                    for _ in range(20):
                        try:
                            os.kill(pid, 0)
                        except ProcessLookupError:
                            break
                        time.sleep(0.25)
                    else:
                        os.kill(pid, signal.SIGKILL)
            except (ValueError, ProcessLookupError, PermissionError) as e:
                log(f"停 hostapd 时: {e}")
            finally:
                if not r.dry_run:
                    self.pidfile.unlink(missing_ok=True)

        r.run("ip", "addr", "del", DEFAULT_AP_CIDR, "dev", self.iface)
        log("热点已停")

    def state(self) -> str:
        if not self.pidfile.exists():
            return "unavailable"
        try:
            pid = int(self.pidfile.read_text().strip())
            os.kill(pid, 0)
            return "active"
        except (ValueError, ProcessLookupError, PermissionError):
            return "unavailable"


# ---------------------------------------------------------------- 请求处理

def read_json(path: Path):
    try:
        return json.loads(path.read_text())
    except (OSError, json.JSONDecodeError):
        return None


def write_json(path: Path, obj) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(".tmp")
    tmp.write_text(json.dumps(obj, ensure_ascii=False))
    tmp.replace(path)   # 原子替换，避免垫片读到半个文件


class Handler:
    def __init__(self, req_path: Path, status_path: Path,
                 iface: str, runner: Runner):
        self.req_path = req_path
        self.status_path = status_path
        self.hotspot = Hotspot(iface, runner)
        self.last_seq = None

    def handle(self, req: dict) -> dict:
        seq = req.get("seq")
        action = req.get("action")
        log(f"请求 seq={seq} action={action} req={json.dumps(req, ensure_ascii=False)[:200]}")

        try:
            if action == "start":
                ssid = req.get("ssid") or "HUAWEI-PC"
                key = req.get("key") or ""
                if not (KEY_MIN <= len(key) <= KEY_MAX):
                    raise RuntimeError(f"口令长度不合法（{len(key)}，要求 {KEY_MIN}..{KEY_MAX}）")
                # 已经有在跑的就先清掉，保证幂等
                self.hotspot.stop()
                self.hotspot.start(ssid, key)
                res = {"ok": True, "state": "active", "iface": self.hotspot.iface,
                       "ip": DEFAULT_AP_IP, "ssid": ssid, "detail": "热点已起"}

            elif action == "stop":
                self.hotspot.stop()
                res = {"ok": True, "state": "unavailable", "detail": "热点已停"}

            elif action == "status":
                res = {"ok": True, "state": self.hotspot.state(), "iface": self.hotspot.iface,
                       "ip": DEFAULT_AP_IP, "detail": "查询"}

            elif action == "capability":
                ok, why = iface_ap_capable(self.hotspot.iface, self.hotspot.r)
                res = {"ok": ok, "state": self.hotspot.state(),
                       "detail": why, "iface": self.hotspot.iface}

            else:
                res = {"ok": False, "state": "unavailable", "detail": f"未知 action: {action}"}

        except Exception as e:                       # 任何失败都要落成状态，别静默
            log(f"  !! 失败: {e}")
            res = {"ok": False, "state": "unavailable", "detail": str(e)}

        res["seq"] = seq
        write_json(self.status_path, res)
        log(f"  -> {json.dumps(res, ensure_ascii=False)}")
        return res


# ---------------------------------------------------------------- main

def main() -> int:
    ap = argparse.ArgumentParser(description="wlanapi 垫片的 Linux 侧后端")
    ap.add_argument("--prefix", required=True,
                    help="Wine 前缀（用于定位 drive_c 里垫片的通信目录）")
    ap.add_argument("--iface", default=os.environ.get("WLANAPI_SHIM_IFACE", DEFAULT_IFACE),
                    help=f"开热点的网卡（默认 {DEFAULT_IFACE}）")
    ap.add_argument("--once", action="store_true", help="处理当前请求后退出")
    ap.add_argument("--dry-run", action="store_true", help="只打印要执行的命令")
    ap.add_argument("--init", action="store_true", help="建目录后退出")
    args = ap.parse_args()

    RUNDIR.mkdir(parents=True, exist_ok=True)

    # 通信目录：垫片写 drive_c\wlanapi_shim\req.json
    comm = Path(args.prefix).expanduser() / "drive_c" / "wlanapi_shim"
    comm.mkdir(parents=True, exist_ok=True)
    req_path, status_path = comm / "req.json", comm / "status.json"

    if args.init:
        log(f"通信目录: {comm}")
        log(f"运行目录: {RUNDIR}")
        return 0

    if not iface_exists(args.iface):
        log(f"!! 网卡 {args.iface} 不存在。可用: "
            f"{[p.name for p in Path('/sys/class/net').iterdir()]}")
        # 不退出 —— 让它写一条失败状态回去，比默默死掉有用

    runner = Runner(args.dry_run)
    handler = Handler(req_path, status_path, args.iface, runner)

    log(f"启动 wlanapi-shimd  iface={args.iface}  comm={comm}  dry_run={args.dry_run}")

    # 启动先回一条能力状态，让垫片一开始就有东西可读
    handler.handle({"seq": 0, "action": "capability"})

    if args.once:
        req = read_json(req_path)
        if req:
            handler.handle(req)
        return 0

    # 轮询（简单可靠；文件很小，0.5s 一次的开销可以忽略）
    while True:
        req = read_json(req_path)
        if req and req.get("seq") != handler.last_seq:
            handler.last_seq = req.get("seq")
            handler.handle(req)
        time.sleep(0.5)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        print()
        raise SystemExit(0)
