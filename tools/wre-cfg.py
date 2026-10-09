#!/usr/bin/env python3
"""把 wre disasm 的输出变成控制流图（CFG）。

为什么需要
----------
WRE 不做函数内 CFG —— 它自己的文档里明确写着：

    | 全量 CFG | ⬜ 不做（callgraph 是"一次进场收边"，不是完整 CFG） |

（`wre callgraph` 是**函数之间**的调用边，不是函数内部的基本块与分支。）

但 **原料是齐的**：`wre disasm --json` 给出逐条指令的 addr/size/bytes/text。
本工具把 jmp/jcc 解析成边、按分支目标切成基本块，再渲染成 Graphviz DOT。

用法
----
    # 直接给 wre 的 JSON
    wre disasm <file> --start 0x140009750 --count 200 --json > f.json
    ./wre-cfg.py f.json -o f.dot

    # 或者让它自己调 wre
    ./wre-cfg.py --file <pe> --start 0x140009750 --count 200 -o f.dot

    dot -Tsvg f.dot -o f.svg      # 渲染（需 graphviz）
    dot -Tpng f.dot -o f.png

口径
----
* 分支识别按助记符（jmp / j<cc> / ret / call）——**不建解码表**，够用且不装聪明。
* `call` 不算分支（它返回后继续往下走），只作为块内注释标出来。
* 间接跳转（`jmp rax` 之类）画成指向 "?" 的边，并标 indirect —— 不凭空造地址。
  （WRE 自己的文档记过一个同族教训：宽松解析把 `JMP EDX` 造出一条到 `0xed` 的假边。）
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from dataclasses import dataclass, field

# ---------------------------------------------------------------- 指令模型

COND_JMP = re.compile(r'^j(?!mp$)([a-z]{2,})$')      # jz/je/jne/jg/ja/... 但不含 jmp
JMP      = re.compile(r'^jmp$')
RET      = re.compile(r'^retn?$|^ret\s')
CALL     = re.compile(r'^call')


@dataclass
class Insn:
    addr: int
    size: int
    text: str

    @property
    def mnem(self) -> str:
        return self.text.split(None, 1)[0].lower() if self.text else ""

    @property
    def operand(self) -> str:
        parts = self.text.split(None, 1)
        return parts[1].strip() if len(parts) > 1 else ""


def parse_addr(op: str) -> int | None:
    """只接受整串就是 0x… 的（WRE 文档里的教训：宽松取前导十六进制会造假边）。"""
    op = op.strip()
    m = re.fullmatch(r'0x([0-9a-fA-F]+)', op)
    return int(m.group(1), 16) if m else None


def load_insns_from_wre(path: str, start: str, count: int) -> list[Insn]:
    out = subprocess.run(
        ["wre", "disasm", path, "--start", start, "--count", str(count), "--json"],
        capture_output=True, text=True, check=True).stdout
    # wre 的 stdout 前面可能有 [metrics] 之类的 stderr 行混进来；取第一个 '{'
    out = out[out.index("{"):]
    data = json.loads(out)
    items = data.get("instructions") or data.get("insns") or data
    res = []
    for it in items:
        try:
            res.append(Insn(int(it["addr"], 16), int(it.get("size", 0)), it.get("text", "")))
        except (KeyError, TypeError, ValueError):
            continue
    return res


def load_insns_from_json(path: str) -> list[Insn]:
    data = json.load(open(path))
    items = data.get("instructions") or data.get("insns") or data
    res = []
    for it in items:
        try:
            res.append(Insn(int(it["addr"], 16), int(it.get("size", 0)), it.get("text", "")))
        except (KeyError, TypeError, ValueError):
            continue
    return res


# ---------------------------------------------------------------- 切基本块

@dataclass
class Block:
    addr: int
    insns: list[Insn] = field(default_factory=list)
    succ: list[tuple[int | None, str]] = field(default_factory=list)   # (目标, 边标签)

    @property
    def end(self) -> int:
        last = self.insns[-1]
        return last.addr + last.size


def build_blocks(insns: list[Insn]) -> list[Block]:
    if not insns:
        return []
    by_addr = {i.addr: i for i in insns}

    # 1) 定 leader
    leaders = {insns[0].addr}
    for i in insns:
        m = i.mnem
        if JMP.match(m) or COND_JMP.match(m) or RET.match(m):
            t = parse_addr(i.operand)
            if t is not None and t in by_addr:
                leaders.add(t)                       # 跳转目标
            nxt = i.addr + i.size
            if nxt in by_addr:
                leaders.add(nxt)                     # 跳转/返回之后

    # 2) 按 leader 切块
    blocks: list[Block] = []
    cur: Block | None = None
    for i in insns:
        if cur is None or i.addr in leaders:
            cur = Block(addr=i.addr)
            blocks.append(cur)
        cur.insns.append(i)

    # 3) 连边
    starts = {b.addr for b in blocks}
    for b in blocks:
        last = b.insns[-1]
        m, op = last.mnem, last.operand
        t = parse_addr(op)
        if JMP.match(m):
            b.succ.append((t if (t in starts) else None, "jmp" if t in starts else "jmp ?"))
        elif COND_JMP.match(m):
            if t in starts:
                b.succ.append((t, "T"))
            else:
                b.succ.append((None, "T ?"))
            nxt = b.end
            if nxt in starts:
                b.succ.append((nxt, "F"))
        elif RET.match(m):
            pass                                     # 没有后继
        else:
            nxt = b.end
            if nxt in starts:
                b.succ.append((nxt, ""))
    return blocks


# ---------------------------------------------------------------- 出 DOT

def to_dot(blocks: list[Block], title: str) -> str:
    lines = [f'digraph cfg {{',
             f'  label="{title}"; labelloc=t; fontname="monospace";',
             f'  node [shape=box, fontname="monospace", fontsize=10];',
             f'  edge [fontname="monospace", fontsize=9];']
    for b in blocks:
        rows = [f"0x{b.addr:x}"]
        for i in b.insns:
            mark = "  ← call" if CALL.match(i.mnem) else ""
            rows.append(f"{i.addr:08x}  {i.text}{mark}")
        lbl = "\\l".join(r.replace('"', '\\"') for r in rows) + "\\l"
        attrs = ""
        # 块里含 call 的给个颜色，方便一眼看到"哪里在调东西"
        if any(CALL.match(i.mnem) for i in b.insns):
            attrs = ', style=filled, fillcolor="#eef4ff"'
        lines.append(f'  n{b.addr:x} [label="{lbl}"{attrs}];')
    for b in blocks:
        for tgt, lab in b.succ:
            if tgt is None:
                # 间接 / 目标在本次范围外 —— 明确画成悬空节点，别造假地址
                gid = f'unk_{b.addr:x}_{abs(hash(lab))%9999}'
                lines.append(f'  {gid} [shape=box, style=dashed, label="{lab}\\n(目标不在范围内)"];')
                lines.append(f'  n{b.addr:x} -> {gid} [style=dashed, label="{lab}"];')
            else:
                color = ' color="#c00"' if lab == "T" else ''
                lines.append(f'  n{b.addr:x} -> n{tgt:x} [label="{lab}"{color}];')
    lines.append('}')
    return "\n".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser(description="wre disasm -> CFG (DOT)")
    ap.add_argument("json", nargs="?", help="wre disasm --json 的输出文件")
    ap.add_argument("--file", help="PE 文件（给了就自己调 wre）")
    ap.add_argument("--start", default=None, help="起始 VA（自己调 wre 时用）")
    ap.add_argument("--count", type=int, default=200)
    ap.add_argument("-o", "--out", default="-", help="DOT 输出（缺省 stdout）")
    args = ap.parse_args()

    if args.file:
        if not args.start:
            print("给了 --file 就要给 --start", file=sys.stderr); return 2
        insns = load_insns_from_wre(args.file, args.start, args.count)
        title = f"{args.file.rsplit('/',1)[-1]} @ {args.start}"
    elif args.json:
        insns = load_insns_from_json(args.json)
        title = args.json
    else:
        ap.print_help(); return 2

    if not insns:
        print("没解析出指令 —— 看一下 wre 的 JSON 结构对不对", file=sys.stderr); return 1

    blocks = build_blocks(insns)
    dot = to_dot(blocks, title)
    if args.out == "-":
        print(dot)
    else:
        open(args.out, "w").write(dot)
        print(f"{len(insns)} 条指令 -> {len(blocks)} 个基本块 -> {args.out}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
