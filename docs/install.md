# 安装步骤

> 全部在**你自己的机器上**完成。本项目不分发华为的任何文件。

## 0. 前置

```bash
# Wine + winetricks + 解包工具
pacman -S wine winetricks cabextract 7z

# 参考实现（用于绕过机型检查；本项目不包含它，需自取）
#   https://github.com/Pygmalion03/huawei-pcmanager-14x-compat
```

## 1. 从华为官方拿安装包

```
https://consumer.huawei.com/cn/support/pc-manager/
```

## 2. 建前缀

```bash
export WINEPREFIX=~/pcm-wine
WINEPREFIX=$WINEPREFIX ./scripts/setup-prefix.sh
```

这一步会装：VC++ 运行库、原生 gdiplus、中文字体、原生 IE8。

**若 `ie8` 失败**：它全部依赖 `web.archive.org`。确认该域名能通（必要时走代理）：

```bash
curl -sI https://web.archive.org/ | head -1     # 期望 200
```

## 3. 装电脑管家

用参考项目的 bootstrap 绕过机型检查：

```bash
cd <huawei-pcmanager-14x-compat 解压目录>
wine dist/huawei-pc-manager-bootstrap.exe --install \
     "/路径/PCManager_Setup_<版本>.exe"
```

安装器 GUI 会出现。**点「立即安装」**（许可协议页可能因 Wine 的 mshtml 限制显示不全，
但勾选框已勾选，不影响安装）。

装完后：

```bash
wine dist/huawei-pc-manager-bootstrap.exe --terminate
wine dist/huawei-pc-manager-bootstrap.exe --install-patch
```

## 4. 打二进制补丁（关键）

主程序会因 `advapi32.NotifyServiceStatusChangeA` 未实现而闪退。补掉：

```bash
WINEPREFIX=~/pcm-wine python3 patches/patch-ansi-to-wide.py \
    "$WINEPREFIX/drive_c/Program Files/Huawei/PCManager" \
    "$WINEPREFIX/drive_c/Program Files/Huawei/Hiview" \
    --dry-run          # 先看要改什么
```

确认无误后去掉 `--dry-run` 再跑一次。

## 5. 启动

```bash
export WINEPREFIX=~/pcm-wine
export WINEDLLOVERRIDES="mscoree="
wine "$WINEPREFIX/drive_c/Program Files/Huawei/PCManager/PCManager.exe"
```

## 已知卡点

到这一步会撞上：

```
wine: Call to unimplemented function wlanapi.dll.WlanSetSecuritySettings, aborting
```

**这是当前未解决的部分**，见 [findings.md §3](findings.md)。

## 排查

```bash
# 看它崩在哪个 API 上
WINEDEBUG=+wlanapi,+loaddll wine PCManager.exe 2>&1 | grep -i 'unimplemented\|aborting'

# 看微软运行库是不是原生的
for f in msvcp140 mshtml gdiplus; do
  printf '%-12s %s\n' "$f" "$(stat -c %s "$WINEPREFIX/drive_c/windows/syswow64/$f.dll" 2>/dev/null)"
done

# 华为自己的日志
ls "$WINEPREFIX/drive_c/ProgramData/Comms/PCManager/log/"
ls "$WINEPREFIX/drive_c/ProgramData/Comms/BasicService/dump/"   # 崩溃 dump
```
