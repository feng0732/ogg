# Ogg 主动出页与终止页的封装延迟分析

> 分析对象：本仓库（libogg）的 Ogg **封装（framing/encapsulation）** 实现，核心代码在
> `src/framing.c`，数据结构在 `include/ogg/ogg.h`，格式语义参照 `doc/rfc3533.txt`。
> 范围严格限定在「逻辑比特流 → Ogg 页」的编码侧（`*_packetin/iovecin`、`*_pageout*`、
> `*_flush*`）及其在解码侧可观察到的页标志；不涉及任何具体编解码器（Vorbis/Opus 等）、
> 物理流复用（chaining/multiplex）策略或 `ogg_sync_*` 传输层。
>
> 文中所有实验数据均来自对本仓库源码树外编译出的 `libogg.a` 实际运行的探针程序
> （库源码未改动），每个页头字段都来自真实返回的 `ogg_page`，并对每页重新计算 CRC 与页内
> 校验和比对，全部 `crc_recompute_matches=1`。

---

## 0. 一页速览：四个因素如何共同决定页边界与延迟

| 决定因素 | 在代码中的位置 | 对页边界 / 延迟的作用 |
|---|---|---|
| **包大小 → 段表（lacing）** | `ogg_stream_iovecin` `framing.c:290,319-329` | 每个包被切成若干 255 字节段 + 1 个收尾段；段数 `bytes/255+1`。段是页切分的最小单位 |
| **正常满页（条件出页）** | `ogg_stream_flush_i(force=0)` `framing.c:387-399` | 累积 **超过 nfill 字节且已完成 ≥4 个包**，或段数到达 **255 硬上限**，才在包边界出页 |
| **显式 flush（强制出页）** | `ogg_stream_flush` → `flush_i(force=1)` `framing.c:495-497` | 只要有任何缓冲数据（`maxvals>0`）就出页，可产生欠尺寸页，用于压低延迟 |
| **跨页包与 BOS/EOS 冲突** | `framing.c:366-417` | BOS 首页只放初始包；255 段上限可在包中间切页（continuation）；EOS 只打在「吞下全部残余段」的最后一页 |

封装侧只有一个真正的页构造函数 `ogg_stream_flush_i()`（`framing.c:349`），四个公共 API
只是以不同的 `force` / `nfill` 参数调用它：

```
ogg_stream_pageout()       -> flush_i(force=自动判定, nfill=4096)   framing.c:511
ogg_stream_pageout_fill(f) -> flush_i(force=自动判定, nfill=f)      framing.c:526
ogg_stream_flush()         -> flush_i(force=1,          nfill=4096) framing.c:495
ogg_stream_flush_fill(f)   -> flush_i(force=1,          nfill=f)    framing.c:503
```

因此「页边界」完全由 `flush_i` 内的段计数循环 + 三个标志位决定，延迟就是数据停留在
`body_data` / `lacing_vals` 缓冲中、尚未被某个出页调用取走的时间。

---

## 1. 包大小如何概括成段（lacing）值

### 1.1 段编码规则（`ogg_stream_iovecin`，`framing.c:276-337`）

一个长度为 `bytes` 的包占用的段表项数（`framing.c:290`）：

```c
lacing_vals = bytes/255 + 1;
```

- 前 `lacing_vals-1` 项全部填 **255**（`framing.c:319-322`）；
- 最后一项填 `bytes%255`（`framing.c:323`）；
- 首项再 `|= 0x100`，作为「包起始」内部标记（`framing.c:327`）。

关键推论（RFC 3533 §6 与代码一致）：

1. **255 不是一个完整包**，它表示「本包还有后续段」；只有 `<255` 的 lacing 值才终结一个包。
2. 长度恰为 255 整数倍的包，收尾值是 **0**，即多占一个零长度段。
3. **零长度（nil）包**就是单独一个 `0` 段。
4. `ogg_stream_iovecin` 先把所有 iov 的 `iov_len` 求和（`framing.c:285-289`，
   并用 `long` 做溢出检查——即 HEAD 提交 `06a5e02` 修复的地方），多个 iov 与单个等长包
   产生**完全相同**的段表，对页封装不可区分。
5. `0x100/0x200/0x400` 是只存在于 `lacing_vals[]` 内部的标记位（BOS 包起始 / EOS 包尾 /
   解码空洞），写入页头段表时一律 `&0xff` 剥离（`framing.c:458`），线上字节里看不到。

### 1.2 实测：包大小 → 段表（探针场景 A，单包强制 flush）

| 包字节 | 段数 | 段表（首部…尾部） | 页 body_len | `ogg_page_packets()` |
|---:|---:|---|---:|---:|
| 0 | 1 | `0` | 0 | 1 |
| 1 | 1 | `1` | 1 | 1 |
| 17 | 1 | `17` | 17 | 1 |
| 254 | 1 | `254` | 254 | 1 |
| **255** | 2 | `255 0` | 255 | 1 |
| 256 | 2 | `255 1` | 256 | 1 |
| 509 | 2 | `255 254` | 509 | 1 |
| **510** | 3 | `255 255 0` | 510 | 1 |
| 511 | 3 | `255 255 1` | 511 | 1 |
| **65025**（=255×255） | 256 | 第 1 页 255 个 `255`，第 2 页一个 `0` | 65025 / 0 | 0 / 1 |
| 65026 | 256 | 第 1 页 255 个 `255`，第 2 页 `1` | 65025 / 1 | 0 / 1 |

注意 65025 字节这一行：一个包的段数超过单页 255 上限，被切成两页，**第一页没有任何包
结束**，所以 `ogg_page_packets()` 返回 0，第二页的 `0` 段才把包终结（返回 1）。这正是
`framing.c:82-93` 注释描述的「跨整页包」情形。

多 iov 验证（场景 A2）：`iov[2] = 200+100 = 300` 字节 → 段表 `255 45`、body 300，与一个
300 字节普通包无异，`ogg_stream_iovecin` 返回 0。

---

## 2. 正常满页：条件出页的水位线与「4 个包」启发式

### 2.1 决策算法（`flush_i`，`framing.c:374-400`）

非 BOS 页时，段选择循环维护两个计数器：

```c
int packets_done=0, packet_just_done=0;
for(vals=0; vals<maxvals; vals++){
  if(acc>nfill && packet_just_done>=4){ force=1; break; }  // framing.c:388
  acc += os->lacing_vals[vals] & 0x0ff;
  if((os->lacing_vals[vals]&0xff)<255){                    // 一个包在此终结
    granule_pos = os->granule_vals[vals];
    packet_just_done = ++packets_done;
  } else
    packet_just_done=0;
}
if(vals==255) force=1;                                     // framing.c:399 硬上限
```

由此得到正常出页的**三条**触发条件（满足任一）：

1. **水位线 + 包数双条件**：已累积 body 字节 **严格大于** `nfill`（默认 4096），
   **并且**此刻至少完成了 4 个包（`packet_just_done>=4`）。两个条件缺一不可。
2. **255 段硬上限**：循环走到 `vals==255` 时无条件 `force=1`，哪怕最后一个段是 255、
   包还没结束（这是唯一会在包中间切页的常规路径，见 §4）。
3. 调用方本身以 `force=1` 进入（flush 系列，见 §3）。

若循环结束 `force` 仍为 0，`framing.c:402` 直接 `return 0`——**不构造页、`og` 不被修改、
`pageno` 不递增**，数据原样留在缓冲里，这就是封装延迟的来源。

代码注释（`framing.c:376-383`）明确说明这是「比朴素 flush 总是更好」的非强制性启发：
① 不无谓地跨页；② 不足 4 个包时不急于出页，以摊薄 27+ 字节页头开销。

### 2.2 实测：100 字节小包的出页节奏（场景 B，默认 nfill=4096）

每个包后调用一次 `ogg_stream_pageout`：

- 第 1 个包后立即出页——但那是 **BOS 初始页特例**（§4.2），不是水位线触发；
- 之后连续 41 次 `pageout` 返回 0；
- 第 **43** 个包后首次「正常」出页：body=4100（41 个 100B 包）、nsegs=41、flags=0x00、
  granulepos=4200；
- 随后每 41 个包出一页（第 84 个包后再次出页，body 同样 4100）；
- 流尾剩余 37 个包（3700B）靠最后的强制 flush 成页。

为什么是 4100 而不是 4096？判断是 `acc > nfill` 的**严格大于**，且只在包边界求值：
40 个包时 acc=4000 不满足，41 个包时 acc=4100>4096 且 `packet_just_done=41≥4`，于是出页。
水位线因此是「目标下限」而非精确页大小，实际页在 4096 之上再补齐到下一个包边界。

`ogg_stream_pageout_fill(...,500)`（场景 B2）把水位线降到 500：第 8 个包（acc=600>500、
完成 6 个包）才出第一页（body=600），证明字节条件与包数条件始终是「与」关系——
前 4 个包即使字节够也不会出页。

### 2.3 单包再大也不主动出页（场景 L）

一个 5000 字节（20 个段）的非 BOS 包，字节数已超过 4096，但 `packet_just_done=1<4`：

```
pageout -> 0   (5000>4096 但 packet_just_done<4，继续缓存)
flush   -> 1   nsegs=20 body=5000 gp=555 flags=0x00
```

这说明正常 `pageout` 会延迟容纳「少量大包」的页，应用若要限制 VBR 流的时间延迟，
必须改用 flush（API 文档 `ogg_stream_pageout.html` 也是这样建议的）。

---

## 3. 显式 flush：用欠尺寸页换延迟

### 3.1 语义差异（`framing.c:346-348, 481-505`）

`ogg_stream_flush_i` 的 `force` 参数注释写明：`force==0` 只刷名义尺寸页，`force==1`
只要有数据就刷。`ogg_stream_flush(_fill)` 固定传 `force=1`，因此：

- **可以产生小于水位线的页**（undersized page），把当前所有已缓冲段立刻封成一页；
- 但它**不保证一次刷完**：单个包可能跨多页（§4.1），255 段上限同样生效，所以文档要求
  「在循环中调用直到返回 0」；
- **返回 0 是唯一的「全部刷净」信号**（`framing.c:359` 的 `maxvals==0` 早退）；
- flush 与 pageout 走同一个构造函数，所以 flush 出的页 CRC、页头格式与正常页完全一致，
  区别只在 body 可能欠尺寸，以及它可以绕开 §2 的双条件。

### 3.2 实测：pageout=0 与 flush=1 的对照（场景 C）

BOS 页（30B，flags=0x02）出完后，再送入一个 100B 包：

```
pageout  -> 0   （<4096B、<4 包、非 EOS，没有任何出页理由）
flush    -> 1   flags=0x00 nsegs=1 body=100 gp=10 pageno=1   ← 欠尺寸页
flush 再次 -> 0  （lacing_fill 已归零，缓冲为空）
```

空流上两者都返回 0（场景 J：fresh stream 的 `pageout` 和 `flush` 均为 0），因为
`maxvals=(lacing_fill>255?255:lacing_fill)==0` 在 `framing.c:358-359` 直接早退。

### 3.3 flush 并不破坏包边界（除 255 硬上限外）

即使 `force=1`，段选择循环仍然跑完，`vals` 通常停在某个 `<255` 段（包边界）之后；
强制只是让 `framing.c:402` 的 `if(!force) return 0` 不再拦截。因此 flush 出的欠尺寸页
仍然装着整数个完整包，只有当缓冲本身就含有一个跨 255 段的巨包时，才会因
`if(vals==255) force=1` 在包中间断开（与 flush 与否无关）。

---

## 4. 跨页包与 BOS/EOS 之间的冲突

这是页边界最微妙的部分，三个机制在 `flush_i` 的同一段代码里相互作用。

### 4.1 跨页包：continuation 位与 granulepos = -1

段表单页最多 255 项（页头第 26 字节只有 1 字节，RFC 3533 §6.2），任何需要 >255 段的包
必然跨页。跨页时：

- 被切包的**后续页**，其首段不带 `0x100` 包起始标记，于是
  `if((os->lacing_vals[0]&0x100)==0) header[5]|=0x01;`（`framing.c:412`）置 **continued
  (0x01)**；
- 中间页没有任何包终结，循环里 `granule_pos` 从不更新，保持初始化的 **-1**
  （`framing.c:355`）。RFC 3533 §6.2：granule_position=-1 表示「本页没有包结束」；
- 包真正结束的那一页，段表末项 `<255`，granulepos 取该包提交时的 granulepos
  （`framing.c:394`）。

**实测场景 E**：47B BOS 包之后，提交一个 130049 字节、带 EOS、gp=12345 的巨包
（130049 = 255×509 + 254，共 510 段）：

| pageno | flags | cont | bos | eos | granulepos | nsegs | body | 完成包数 |
|---:|:---:|:---:|:---:|:---:|---:|---:|---:|---:|
| 0 | 0x02 | 0 | 1 | 0 | 0 | 1 | 47 | 1（BOS 包） |
| 1 | 0x00 | 0 | 0 | 0 | **-1** | 255 | 65025 | **0**（巨包前半，无包结束） |
| 2 | **0x05** | **1** | 0 | **1** | 12345 | 255 | 65024 | 1（末段 254 终结巨包） |

解码侧 `ogg_stream_packetout` 直到第 3 页才一次性还原出
`{len=130049, e_o_s=512(=内部标记 0x200，逻辑为真), gp=12345}`，与输入逐字节一致。0x05 = continued(0x01) | eos(0x04)：
巨包的收尾页同时是「续包页」和「流终止页」，两个标志并不互斥。

### 4.2 BOS 冲突：初始页只容纳初始头包

`flush_i` 开头的特例（`framing.c:366-373`）：当 `os->b_o_s==0`（还没写过首页）时，
段扫描在遇到**第一个 `<255` 段（第一个包结束）就停下**，并强制 `granule_pos=0`。
随后 `framing.c:414` 置 BOS 位 0x02，`framing.c:417` 把 `b_o_s` 永久置 1。

含义与冲突处理：

1. **无论水位线/包数条件如何，第一个包一定单独触发首页**——这就是场景 B 中第 1 个
   100B 包立刻出页、`ogg_stream_pageout` 注释里「initial header page case」
   （`framing.c:516`）的原因。媒体映射（如 Vorbis 的 identification header）依赖这一点，
   保证流的第一个页自带可识别的头包。
2. 若初始包本身就 >255 段，BOS 特例的扫描在 255 段处被 `maxvals` 截断：
   **场景 K**（70000B 初始包）首页 flags=0x02、nsegs=255、body=65025、完成包数 0，
   残余段留待后续 continued 页。即 BOS 位只打在第 0 页，continuation 规则照常接管，
   BOS 不会「霸占」多个页。
3. **场景 F**（4345B 初始包 + 259B 次包同时缓冲）：首页 nsegs=18、body=4345，
   只含初始包（flags=0x02，gp=0）；已在缓冲里的第二个包被完整留给第 1 页，
   不会搭首页的车。

### 4.3 EOS 冲突：终止位只打在「吞下全部残余」的最后一页

EOS 的判定（`framing.c:416`）：

```c
if(os->e_o_s && os->lacing_fill==vals) os->header[5]|=0x04;
```

- `os->e_o_s` 在提交带 `e_o_s` 的包时就置位（`framing.c:334`），但**页头 EOS 位要等到
  本次出页恰好取走缓冲中最后一个段（`lacing_fill==vals`）才置上**；
- 因此巨包跨页时，前面的页即使知道流将结束也不带 EOS（场景 E 第 1 页 flags=0x00），
  只有收尾页带 0x04；
- `ogg_stream_pageout` 把「`e_o_s` 已置且仍有缓冲」作为两个自动 `force=1` 条件之一
  （`framing.c:515`），所以**欠尺寸的尾页不需要应用手动 flush**，pageout 自己就会出。

**实测场景 D（不完整/欠尺寸尾包）**：BOS 页后只再提交两个 100B 包，第二个带 e_o_s。
此时既不满 4096B 也不足 4 个包，但 `pageout` 直接返回终止页：

```
flags=0x04 (eos=1, cont=0, bos=0)  granulepos=200  nsegs=2  body=200  segtable=100 100
解码侧还原两个包（首个包 gp=-1，第二个包 `e_o_s=512` 即 0x200 标记、逻辑为真）；之后 `ogg_stream_eos()=1`，再 pageout=0
```

**实测场景 G（255 整数倍包的零身体终止页）**：尾包 64770 = 255×254 字节，恰好以一个
`0` 段收尾。前一页装下 100B 小包 + 254 个满段（nsegs=255，body=64870），最后只剩一个
`0` 段：

```
pageno=2  flags=0x05  nsegs=1  header_len=28  body_len=0  segtable=0
```

这是一个**身体长度为 0 的合法终止页**：它不携带任何新 body 字节，却同时承担
continuation（终结跨页包）、EOS（终结流）和包计数（`ogg_page_packets()=1`）三重职责。
这与 `framing.c:1811` 自测用例「term only page. why not?」是同一现象。

### 4.4 三个标志位的可观察判定汇总（编码侧，`framing.c:410-417`）

| 页头标志 | 置位条件（代码） | 典型出现位置 |
|---|---|---|
| continued 0x01 | 本页首段 `lacing_vals[0]` 不带 `0x100`（不是包起始） | 跨页巨包的第 2…N 页 |
| BOS 0x02 | `os->b_o_s==0`，即整个逻辑流的第一次出页；随后 b_o_s 永久置 1 | 仅 pageno=0 |
| EOS 0x04 | `e_o_s` 已标记 **且** 本页取走全部剩余段 | 仅最后一页 |
| 组合 0x05 | 最后一个包跨页，其收尾页同时是终止页 | 场景 E/G 末页 |
| 组合 0x06 | 理论上仅当流只有一个页且该页同时为首尾（单包流） | BOS+EOS 同页 |

---

## 5. 用 framing 证据说明可观察的页面标志

所有「线上可观察」的东西都在 27 字节固定页头 + 段表里（RFC 3533 §6.1），探针逐页打印并
重算 CRC，全部通过。下面把代码字段与观测手段对应起来：

| 页头偏移 | 字段 | 大小 | 封装侧写入位置 | 观察 API / 证据 |
|---:|---|---:|---|---|
| 0-3 | capture `OggS` | 4 | `framing.c:405` | 探针 `capture='OggS'`；解码侧 `ogg_sync_pageseek` 靠它重同步 |
| 4 | version | 1 | `framing.c:408` 恒为 0 | `ogg_page_version()`，实测全为 0 |
| 5 | header_type flags | 1 | `framing.c:411-416` | `ogg_page_{continued,bos,eos}()` 按 `&0x01/0x02/0x04` 解码（`framing.c:37-47`） |
| 6-13 | granule_position | 8 | `framing.c:420-423` | `ogg_page_granulepos()`；跨页中间页实测为 -1（全 0xFF） |
| 14-17 | serial no | 4 | `framing.c:426-432` | `ogg_stream_init` 的 serialno，实测各场景独立 |
| 18-21 | page sequence | 4 | `framing.c:441-447`，`pageno++` | 实测从 0 连续递增；每次成功出页 +1，返回 0 时不增 |
| 22-25 | CRC32 | 4 | `ogg_page_checksum_set` `framing.c:255-273` | 探针清零重算与存储值逐页比对，全部匹配 |
| 26 | n_segments | 1 | `framing.c:456` | 即 `vals`，范围 1…255；`header_len = vals+27`（`framing.c:462`） |
| 27… | segment table | vals | `framing.c:457-458`（`&0xff`） | 255=续段，<255=包尾；`ogg_page_packets()` 数 <255 的项（`framing.c:95-100`） |

### 5.1 从段表反推页边界与延迟的三条观测法则

1. **数 `<255` 的段表项** = 本页完成的包数（`ogg_page_packets`）。满 255 个 255 的页
   返回 0，外部即可断定这是一个被截断的跨页包，下一页必带 continued。
2. **granulepos=-1（8 字节全 0xFF）** 外部即可断定本页无包结束，其时间位置要等后续页，
   解码端不能在本页释放任何完整包——这是延迟在「线上格式」层面的直接体现。
3. **flags=0x04/0x05 是流的终止凭证**；读到 EOS 页后该 serialno 的逻辑流不再有页。
   BOS（0x02）在每个逻辑流中恰好出现一次且位于 pageno=0。

### 5.2 页尺寸的硬边界（由段表 1 字节计数推出）

- 单页段数上限 255 → 单页 body 上限 255×255 = **65025 字节**，页头上限 255+27=**282 字节**
  （结构体里 `header[282]` 的由来，`ogg.h:68`），整页上限 65307 字节；
- 实验中所有满段页均为 `nsegs=255 header_len=282 body_len=65025`（场景 A/E/K），
  场景 I 里 300 个 10B 包（BOS 后）被精确切成 255 段页（gp=255）+ 45 段尾页（gp=300），
  与硬上限完全吻合。

---

## 6. 粒度与返回结果（API 契约）

### 6.1 输入侧：提交粒度是「整个包」

| API | 成功返回 | 失败返回 | 粒度 / 备注 |
|---|---|---|---|
| `ogg_stream_packetin` (`framing.c:339`) | 0 | -1 | 一个 `ogg_packet`；转调 iovecin(count=1)，携带 `e_o_s`、`granulepos` |
| `ogg_stream_iovecin` (`framing.c:276`) | 0 | -1 | 散布在多个 buffer 的**一个**逻辑包；长度按 `long` 累加并做溢出检查（HEAD 修复点） |

两者都把数据**拷贝**进 `os->body_data`（`framing.c:313-316`），返回后调用方可释放原缓冲；
每次提交 `packetno++`（`framing.c:332`），但**不产生任何页**，也不做页边界决策。
失败（分配失败/溢出）会 `ogg_stream_clear` 清空状态，之后 `ogg_stream_check` 返回 -1。

### 6.2 输出侧：取页粒度是「每次一个完整页」，须循环调用

| API | 返回 1（非零） | 返回 0 | 关键延迟行为 |
|---|---|---|---|
| `ogg_stream_pageout` | 一页已写入 `og` | 数据不足/出错，`og` 不动 | 仅在满页、BOS 首页、EOS 尾页三种情形出页 |
| `ogg_stream_pageout_fill` | 同上 | 同上 | 水位线可调，双条件（>nfill 且 ≥4 包）不变 |
| `ogg_stream_flush` | 强制出一页（可欠尺寸） | **缓冲已空**，或流已失效 | 绕开水位线；单次不保证刷净巨包 |
| `ogg_stream_flush_fill` | 强制出约 nfill 尺寸的页 | 同上 | 自定义页大小的强制刷出 |

契约要点（代码 + 官方 HTML 文档互证）：

1. **返回值是 int 而不是页数**：1=本次返回了一页，0=当前没有可出的页。一个巨包需要
   多次调用才能出完（场景 E 一个 flush 循环产出 2 页、场景 G 同），所以标准用法是
   `while(ogg_stream_pageout(...))` 循环排空。
2. **返回 0 时 `og` 不被修改、`pageno` 不递增**（早退路径 `framing.c:357-359,402`）；
   流被 clear 后所有出页 API 也返回 0（`ogg_stream_check` 失败）。
3. **返回的指针归 libogg 所有且易失**：`og.header` 指向 `os->header[282]`、
   `og.body` 指向 `os->body_data+body_returned`（`framing.c:461-463`），下一次
   packetin/pageout/flush 即失效，应用须立即写出或拷贝（pageout 文档明确声明）。
4. **body 的延迟回收**：出页并不立刻 memcpy/释放 body，只移动 `body_returned` 游标
   （`framing.c:471`）；直到下一次 `iovecin` 才 `memmove` 压实缓冲（`framing.c:292-302`），
   因此上一页的 body 指针在下次 packeting 之前保持有效。
5. **EOS 查询与出页解耦**：`ogg_stream_eos()`（`framing.c:537`）只是返回 `os->e_o_s`
   标记，最后一个带 e_o_s 的包一提交就为真，即使终止页还没被取走。
6. **重置语义**：`ogg_stream_reset`（`framing.c:916`）把 `pageno=-1`、`b_o_s=0`、
   `e_o_s=0`、各游标清零但保留分配的存储；`flush_i` 对 pageno=-1 特判回 0
   （`framing.c:436-440`），使重置后首页序列号重新从 0 开始。

### 6.3 延迟来源的完整清单（封装层视角）

1. **水位线批处理延迟**（§2）：默认要攒到 >4096B 且 ≥4 个包，小包场景下最主要的延迟；
2. **包边界对齐延迟**：出页判断只在包终结处求值，页大小永远向包边界上取整；
3. **段上限分页延迟**：>255 段的包被拆成多页，中间页 granulepos=-1，解码端必须等到
   收尾页才能交付整个包（应用层看到的是包粒度延迟，而非页粒度）；
4. **尾包滞留延迟**：非 EOS 的欠尺寸尾包只能靠显式 flush 出页（§3），否则无限期滞留；
   而带 EOS 的尾包由 pageout 自动强制出页（§4.3），无需 flush；
5. **BOS 提前、EOS 押后**：首包被 BOS 特例提前成页（降低初始延迟），EOS 位被推迟到
   最后一个段真正出页时才写（保证终止语义精确）。

---

## 附录 A：关键代码索引

| 行为 | 位置 |
|---|---|
| 包 → 段数 `bytes/255+1`、收尾 `bytes%255`、`0x100` 包起始标记 | `framing.c:290,319-329` |
| iov 长度累加与溢出检查（HEAD `06a5e02` 修复） | `framing.c:285-289` |
| 唯一页构造函数 `ogg_stream_flush_i` | `framing.c:349-479` |
| BOS 初始页特例（首包独占、gp=0） | `framing.c:366-373,414,417` |
| 水位线 + 4 包启发式 | `framing.c:385-398` |
| 255 段硬上限强制出页 | `framing.c:399` |
| continued / BOS / EOS 三个标志写入 | `framing.c:411-417` |
| EOS 仅在 `lacing_fill==vals` 时置位 | `framing.c:416` |
| 段表写入（剥离内部标记位）、header_len=vals+27 | `framing.c:456-462` |
| pageout 的两个自动 force 条件（EOS 尾页 / BOS 首页） | `framing.c:511-520,526-535` |
| flush 固定 force=1 | `framing.c:495-505` |
| 空缓冲早退（返回 0 的根因） | `framing.c:358-359` |
| 页标志观察 API（`&0x01/0x02/0x04`） | `framing.c:37-47` |
| 完成包数 = 段表中 <255 的项数 | `framing.c:95-100` |
| CRC-32（多项式 0x04c11db7，初值 0） | `framing.c:237-273` |

## 附录 B：实验场景与报告结论对照

| 场景 | 输入 | 验证的结论 |
|---|---|---|
| A / A2 | 0…65026B 单包、多 iov 包 | §1 段编码、255 整数倍补 0、nil 包 |
| B / B2 | 100B 包流，nfill=4096/500 | §2 严格大于水位线、≥4 包双条件、4100B 出页 |
| C | BOS 后单个 100B 包 | §3 pageout=0 vs flush=1、二次 flush=0 |
| D | 两个 100B 包，末包 EOS | §4.3 欠尺寸 EOS 尾页由 pageout 自动出 |
| E | 130049B EOS 巨包 | §4.1 continued、gp=-1、0x05 组合标志 |
| F | 4345B 初始包 + 次包 | §4.2 BOS 页只含初始包 |
| G | 64770=255×254 尾包 | §4.3 零身体终止页（segtable 仅 `0`） |
| H / I | 255 / 300 个 10B 包 | §5.2 255 段硬上限与 BOS 隔离 |
| J | 空流 | §6.2 无数据时 pageout/flush 均返回 0 |
| K | 70000B 初始包 | §4.2 BOS 页自身被 255 段上限截断 |
| L | 单个 5000B 非 BOS 包 | §2.3 单包超水位线仍不主动出页 |
