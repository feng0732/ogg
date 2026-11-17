# Ogg 页损坏后的逻辑流恢复边界分析

> 分析对象：本仓库 libogg（`src/framing.c`、`include/ogg/ogg.h`）
> 范围：页序号（page sequence number）、continued packet 标志、粒度位置（granule position）
> 与多逻辑流（multi-serialno）状态如何共同决定损坏后跳过哪些数据、在哪里恢复输出。
> 本文不修改任何代码；结论均可由源码与 framing 自测（`_V_SELFTEST`）复现。

---

## 0. 证据与复现方式

| 证据 | 位置 |
|---|---|
| 同步层 CRC 校验、丢同步、重捕获 | `src/framing.c:642-729`（`ogg_sync_pageseek`）、`742-770`（`ogg_sync_pageout`） |
| 逻辑层页序号检查、回卷、`0x400` 空洞标记、continued 跳过 | `src/framing.c:775-902`（`ogg_stream_pagein`） |
| 空洞标记如何变成 `packetout()==-1` | `src/framing.c:944-996`（`_packetout`） |
| framing 自测（含丢页、重捕获、大跨页包重同步） | `src/framing.c:1682-2136`（`main`，`_V_SELFTEST`） |
| 格式规范依据 | `doc/rfc3533.txt` 第 6 节页头字段语义；`doc/framing.html` |

构建并运行仓库自带自测（CMake 的 `BUILD_TESTING` 把 `framing.c` 以 `-D_V_SELFTEST`
编成 `test_framing`，见 `CMakeLists.txt:190-196`）：

```sh
cmake -DBUILD_TESTING=ON -B /tmp/ogg-build .
make -C /tmp/ogg-build
/tmp/ogg-build/test_framing     # 全部用例输出 ok.，退出码 0
```

为得到"可观察的包序列"，另写了一个只读的实验程序（链接本仓库静态库，不改库代码），
通过翻转某个页 **body 字节** 制造 CRC 失配，再走完整的
`ogg_sync_* → ogg_stream_pagein → ogg_stream_packetout` 管线打印每个事件。
其输出与自测的断言逐一对应，下文用 [实验] 标注；自测内断言用 [自测] 标注。

---

## 1. 两层架构：物理同步层与逻辑组包层的职责边界

Ogg 的恢复不是在一个地方完成的，而是两个独立层各管一段：

```
字节流
  │  ogg_sync_buffer / ogg_sync_wrote
  ▼
┌─────────────────────────────────────────────┐
│ 物理同步层 ogg_sync_state（跨所有逻辑流共享） │  找 "OggS"、按段表算页长、CRC 校验
│  ogg_sync_pageseek / ogg_sync_pageout        │  坏页：丢同步，扫描下一个 'O' 重捕获
└─────────────────────────────────────────────┘
  │  返回一个 CRC 合法的 ogg_page（或 -1 表示发生过跳字节）
  ▼
        应用按 serialno 分派给对应逻辑流
  ▼
┌─────────────────────────────────────────────┐
│ 逻辑组包层 ogg_stream_state（每个 serialno 一个）│  查页序号、回卷残缺包、
│  ogg_stream_pagein / ogg_stream_packetout     │  插 0x400 空洞、按段重组包
└─────────────────────────────────────────────┘
  │  完整 ogg_packet（packetout==1）或空洞信号（packetout==-1）
  ▼
编解码器
```

关键含义：**CRC 失败的页根本不会进入逻辑层**。逻辑层看到的永远是校验合法的页，
它要处理的是"页序号不连续"（页丢失/被同步层丢弃）以及"残缺的跨页包"。

### 1.1 物理同步层：CRC 失败 → 丢同步 → 重捕获

`ogg_sync_pageseek`（`framing.c:642`）的判定顺序：

1. 捕获模式：`memcmp(page,"OggS",4)` 失败 → `sync_fail`（`framing.c:657`）。
2. 按段表累加 body 长度（`framing.c:664-665`），数据未到齐则返回 0 等待更多数据。
3. 整页缓冲齐后重算 CRC 并与页内 CRC 比较（`framing.c:672-698`）；
   失配则（未定义 `DISABLE_CRC` 时）`goto sync_fail`（`framing.c:694-697`）。
4. `sync_fail`（`framing.c:717-728`）清零已解析的 header/body 计数，
   用 `memchr(page+1,'O',bytes-1)` 从下一个字节起寻找新的候选捕获点，
   返回**负值** `-（跳过的字节数）`。

`ogg_sync_pageout`（`framing.c:742`）把负返回值翻译成面向应用的三态：

- 第一次发现失步：置 `oy->unsynced=1`，返回 **-1**（"recapture / 数据有洞"）；
- 之后内部循环继续找，直到下一个 CRC 合法页才返回 1（`framing.c:750-769`）。

[实验] 所有注入 body 翻转的场景中，损坏页之后都稳定出现一行
`ogg_sync_pageout -> -1 (physical hole/recapture)`，且下一条事件就是后一个完好页，
同步层不会吐出任何损坏页的内容。

> `doc/framing.html` 解释了为什么损坏的长度字段不会导致失控：
> 页最大 65307 字节（255 段 ×255 + 27 头），"按损坏的尺寸信息读入……发现校验和不匹配、
> 丢弃同步然后寻找重捕获"。

### 1.2 `DISABLE_CRC` 的边界（编译期开关）

`framing.c:694` 的 `#ifndef DISABLE_CRC` 表明：禁用 CRC 后，校验失配**不**跳转
`sync_fail`，损坏页被原样当作合法页交付。自测对应地跳过两个强依赖 CRC 的用例
（`framing.c:1796-1808`、`2078-2122`，打印 `Skipping ... due to --disable-crc`）。

[实验] 用 `-DDISABLE_CRC` 构建后，同样翻转 body 字节：同步层无 -1、无跳页，
8 个包全部照常输出（含被翻转的损坏数据）。即**同步层是唯一的 CRC 把关点**；
逻辑层的页序号/continued 机制在 CRC 关闭时仍在，但只对真正"丢页/乱序"生效。

---

## 2. 逻辑层恢复的四个决定因素

### 2.1 页序号 `pageno`：判断是否有页丢失

每个 `ogg_stream_state` 独立维护期望页号 `os->pageno`（初始 0，
`ogg_stream_reset` 后为 -1，见 `framing.c:930`、`899`）。
`ogg_stream_pagein` 第一步做版本/serial 校验（`framing.c:820-821`），随后：

```c
if(pageno!=os->pageno){              /* framing.c:826 */
  /* unroll previous partial packet (if any) */
  for(i=os->lacing_packet;i<os->lacing_fill;i++)
    os->body_fill-=os->lacing_vals[i]&0xff;   /* framing.c:830-831 */
  os->lacing_fill=os->lacing_packet;          /* framing.c:832 */
  if(os->pageno!=-1){
    os->lacing_vals[os->lacing_fill++]=0x400; /* framing.c:835-836 */
    os->lacing_packet++;
  }
}
...
os->pageno=pageno+1;                 /* framing.c:899：以页内实际序号重新对齐 */
```

要点：

- **判断单位是逻辑流自己的页号**，与物理交错无关。RFC 3533 第 6 节：
  "page_sequence_number …… the decoder can identify page loss. This sequence
  number is increasing on each logical bitstream separately."
- 序号不连续触发两件事：(a) 回卷当前尚未完成的跨页包（见 2.2）；
  (b) 在段 FIFO 里插入一个 `0x400` 空洞标记（流的第一页除外，`pageno==-1` 不插）。
- 处理结束后 `os->pageno=pageno+1`，无论跳了多少页都**一次重新对齐**，
  因此后续好页不再重复报错。
- `0x400` 在 `_packetout` 中被消费：返回 **-1** 通知编解码器"这里有缺口，
  可能要处理包间依赖"，同时 `packetno++`（`framing.c:954-960`）。

注意：**无论丢失的页里装了 1 个还是 8 个包，一次序号失配只插一个 `0x400`，
`packetno` 只额外 +1。** 框架层知道"有洞"，但不知道洞里丢了几个包
（`include/ogg/ogg.h:77-81` 对 `packetno` 的注释明确说这是给编解码器的耦合信号）。

### 2.2 continued packet 标志（0x01）与段表 255：决定回卷与续接跳过

Ogg 用段表（segment table）切分包：段值 `<255` 表示一个包在此结束；
连续的 `255` 表示包还在延续。跨页时新页头置 0x01（continued）。
编码器侧在 `framing.c:412` 根据首段是否带 `0x100`（包起始标记）设置该位。

逻辑层用两个游标跟踪组包状态（`ogg.h:63-66`）：

- `lacing_fill`：段 FIFO 中所有段的边界；
- `lacing_packet`：最后一个**已完成**包之后的边界。
  每遇到 `<255` 的结束段就更新 `lacing_packet=lacing_fill`（`framing.c:883`）。
  因此区间 `[lacing_packet, lacing_fill)` 恰好是"已缓冲但未完成的跨页包"残段。

页号失配时的回卷（`framing.c:830-832`）正是把这部分残段从 body 和段 FIFO 中移除——
**已经完整组好的包（`<lacing_packet`）不受影响，不会回卷。**

接着处理 continued 页（`framing.c:843-858`）：

```c
if(continued){
  if(os->lacing_fill<1 ||
     (os->lacing_vals[os->lacing_fill-1]&0xff)<255 ||
     os->lacing_vals[os->lacing_fill-1]==0x400){
    bos=0;
    for(;segptr<segments;segptr++){     /* 跳过本页开头属于残包的段 */
      int val=header[27+segptr];
      body+=val; bodysize-=val;
      if(val<255){ segptr++; break; }   /* 遇到结束段：残包残余全部丢弃 */
    }
  }
}
```

三种触发"丢弃本页续接残段"的条件：

1. 段 FIFO 为空（没有任何在途包）；
2. FIFO 末段 `<255`（上一个包已经在之前页正常结束，这个 continued 位是"孤儿"）；
3. FIFO 末段是 `0x400`（刚发生过丢页，前情不可信）。

`doc/framing.html` 说明了该设计意图：continued 标志让恢复"只需查看当前页头"，
在"包起源于一个损坏的前序页、前页段表不可信"时实现更快的错误恢复。

若 FIFO 末段确为 255（在途包确实延续到本页且中间没丢页），则不跳过、正常续接。

### 2.3 粒度位置 granule position：恢复后的时间锚点与 -1 语义

- 编码侧：页的 granulepos 取该页上**最后一个完成包**的粒度（`framing.c:393-395`）；
  没有完成包的纯续接页，`granule_pos` 保持初始值 -1（`framing.c:355`）。
- 解码侧：每页段表先全部填 `granule_vals=-1`（`framing.c:871`），
  仅当本页至少完成一个包时，把页头 granulepos 写到最后一个结束段上
  （`framing.c:887-889`）。RFC 3533：granulepos = -1 表示"本页没有包结束"。
- 包输出时取其结束段上记录的值（`framing.c:985`）。

恢复边界上的可观察后果：

1. 被丢页本身携带的 granulepos 永久丢失；`0x400` 空洞不带任何粒度信息，
   编解码器只能从**下一个完成包**重新获得时间锚点。
2. 跨页大包只在其结束页拿到 granulepos，中间纯续接页与残段均为 -1。
3. 空洞之后第一个幸存包若在页中不是最后一个完成包，其 granulepos 仍是 -1，
   直到该页最后一个完成包才出现新的非 -1 锚点（见第 4 节序列表）。

### 2.4 多逻辑流状态：serialno 分派与故障隔离

- 物理层 `ogg_sync_state` 只有一个，被所有逻辑流共享；
  每个逻辑流持有独立的 `ogg_stream_state`（独立 `pageno`、段 FIFO、`packetno`）。
- 页头 4 字节 serialno（`framing.c:62-67`）由应用用来选择目标流；
  `ogg_stream_pagein` 对 serial 不匹配直接 `return -1`（`framing.c:820`），
  该页不改变任何状态。
- 因此一个逻辑流的损坏：
  - 在物理层只表现为一段被跳过的字节与一次 -1，交错存放的其他流页面
    只要自身 CRC 合法就照常被捕获；
  - 在逻辑层只污染对应 serial 的 `pageno`/段 FIFO，其他流的序号连续性、
    包序列完全不受影响。

[实验] 双流交错（serial `0xAAAAAAAA` / `0xBBBBBBBB`），注入损坏 A 流第 2 页：

```
sync page: serial=0xaaaaaaaa pageno=0  -> A packet 0 (gran 0)
sync page: serial=0xbbbbbbbb pageno=0  -> B packet 0 (gran 0)
sync: -1 physical hole/recapture              <- A 的坏页被同步层丢弃
sync page: serial=0xbbbbbbbb pageno=1  -> B packets 1,2,3 (gran …,3000)
sync page: serial=0xaaaaaaaa pageno=2  -> A gap marker; packetno now 2
                                         A packets 2,3 (gran …,7000)
```

B 流三个包无损连续输出，无 gap；A 流独立产生一次空洞并从包号 2 续接。
故障半径严格限制在单个 serial 内。

---

## 3. 三种场景的可观察包序列

下列序列均取自实验程序对真实库调用的打印；包号为 `ogg_packet.packetno`
（即解码器视角的序号），`GAP` 表示 `ogg_stream_packetout()` 返回 -1。

### 3.1 场景一：无损（lossless）

8 个独立小包（100/2000/3000/2500/1500/800/300/120 字节）编成 3 页：

```
page[0] BOS           gran=0     -> packet 0  (100B, gran=0)
page[1]               gran=5000  -> packets 1,2,3 (gran=-1,-1,-1), packet 4 (gran=5000)
page[2] EOS           gran=8000  -> packets 5,6 (gran=-1), packet 7 (gran=8000, EOS)
```

[实验] 无损管线：无 sync -1、无 GAP，`packetno = 0,1,2,3,4,5,6,7` 严格连续，
与输入一一对应。自测的全部 8 个 `test_pack` 用例（`framing.c:1691-1817`）
都断言无损往返，并额外校验 peek/out 一致、b_o_s/eos 标志、body 内容与总字节数
（`framing.c:1655-1678`）。

### 3.2 场景二：单页损坏（页内全部是独立、页内完成的包）

翻转 page[1]（含包 1–4）body 一字节：

```
sync page pageno=0 BOS  -> packet 0 (100B, gran=0)
ogg_sync_pageout -> -1 (physical hole/recapture)     <- page[1] 整页被丢弃
sync page pageno=2 EOS  -> GAP; decoder packetno now 2
                          packet 2 (800B, gran=-1)
                          packet 3 (300B, gran=-1)
                          packet 4 (120B, gran=8000, EOS)
```

结果分析：

- 同步层丢弃整页（页是 CRC 校验的最小单位，无法只救页内部分包）。
- page[2] 页号 2 ≠ 期望 1 → 插入一个 `0x400`；此时无在途残包，回卷为空操作。
- page[2] 不是 continued 页，段表全部正常接收。
- **可观察序列：`0, GAP, 2, 3, 4`**。包 1–4（4 个）在数据层面全部丢失，
  但框架只报告一次缺口、`packetno` 只跳 1（0→2）。幸存包的相对编号被重映射，
  这是与跨页损坏最直观的区别之一。
- granulepos：缺口后前两个幸存包为 -1，最后一个包携带页级锚点 8000；
  坏页上的锚点 5000 丢失。

### 3.3 场景三：跨页包损坏（一个包横跨多页）

构造 130049 字节大包（与自测 "very large packets" 同构，
`framing.c:1787-1794`），布局：

```
page[0] BOS          segs=1   gran=0    packets=1  (100B 独立包)
page[1] 非 CONT      segs=255 gran=-1   packets=0  (大包前 65025B，纯在途)
page[2] CONT         segs=255 gran=2000 packets=1  (大包在此页结束，共 130049B)
page[3] EOS          segs=4   gran=4000 packets=2  (259B、255B 两个后续包)
```

#### 3.3a 损坏"大包结束页" page[2]（经典重同步边界）

```
sync page pageno=0 BOS -> packet 0 (100B)
sync page pageno=1     -> （纯续接页，无包输出）
ogg_sync_pageout -> -1 (physical hole/recapture)    <- page[2] 被丢弃
sync page pageno=3 EOS -> GAP; decoder packetno now 2
                         packet 2 (259B, gran=-1)
                         packet 3 (255B, gran=4000, EOS)
```

page[3] 到达时 `pageno=3 ≠ 期望 2`：

1. 回卷：page[1] 缓冲的大包残段（末段为 255，落在 `[lacing_packet,lacing_fill)`）
   被整体移除——**130049B 的大包连同一个页号失配信号一起作废，不产生任何输出**；
2. 插入 `0x400`；
3. page[3] 头无 continued 位，段表按两个完整新包接收。

**可观察序列：`0, GAP, 2, 3`**。这正是自测
"continuation resync in very large packets"（`framing.c:1796-1805`，
注释称其针对 libogg 1.1.1 的 "resync in large continuation bug"）覆盖的路径：
该用例用 `test_pack(...,100,2,3)` 制造 100 字节偏移、跳过 2 页、3 个包前缀，
断言解码端从 page[2] 重新对齐且后续包数据完全一致。

#### 3.3b 损坏"大包起始页" page[1]（孤儿 continued 页的续接跳过）

```
sync page pageno=0 BOS -> packet 0 (100B)
ogg_sync_pageout -> -1 (physical hole/recapture)    <- page[1] 被丢弃
sync page pageno=2 CONT -> GAP; decoder packetno now 2   （本页续接残段被跳过，无包输出）
sync page pageno=3 EOS  -> packet 2 (259B, gran=-1)
                          packet 3 (255B, gran=4000, EOS)
```

page[2] 带 continued 位到达，此时段 FIFO 末段是刚插入的 `0x400`，
命中 `framing.c:846` 第三条件：开头属于大包的段被逐段跳过直到第一个 `<255`
结束段（`framing.c:848-856`），残包残余被丢弃；同页其余完整包正常保留。
最终输出序列与 3.3a 相同（`0, GAP, 2, 3`），但机制不同：
3.3a 靠**回卷**删除已缓冲残段，3.3b 靠 **continued 跳过逻辑**拒绝孤儿续接。

#### 3.3c 损坏纯中间续接页（三段以上超长包）

若大包横跨 ≥3 页且损坏的是中间"零完成包"页：同步层丢页后，
下一个 CONT 页到达时同样因页号失配先回卷在途残段、再按 `0x400` 条件跳过孤儿续段，
恢复点仍是**洞之后第一个完整包边界**。任何只收到部分段的包都不会被半个输出——
`_packetout` 只在遇到 `<255` 结束段时才交付完整包（`framing.c:973-978`），
残段永远停留在 FIFO 中等待下一页或被下一次失配回卷。

---

## 4. 独立包损坏 vs 跨页包损坏：结果对比

| 维度 | 单页损坏（独立包，场景二） | 跨页包损坏（场景三） |
|---|---|---|
| 同步层行为 | CRC 失配，整页丢弃，一次 -1 重捕获 | 相同：整页丢弃、一次 -1 |
| 逻辑层触发条件 | 仅页号失配；无在途包，回卷为空操作 | 页号失配 **+** 回卷在途残段（3.3a）或 continued 孤儿跳过（3.3b） |
| 被牺牲的数据 | 坏页上**所有**页内完成包（实验中 4 个） | **整个**跨页大包（实验中 130049B，分布在 2 页），坏页上若还有完整包则保留 |
| 空洞标记数量 | 1 个 `0x400`，与丢包数无关 | 1 个 `0x400` |
| `packetno` 变化 | 仅 +1（0→2），框架不报告实际丢了几个包 | 同样仅 +1；大包本身从不占用输出序号 |
| 恢复点 | 下一完好页的第一个包边界 | 洞之后第一个**完整**包边界；残包零输出，不会出现"半包" |
| granulepos | 坏页锚点丢失，幸存页末包重建锚点 | 大包结束页锚点丢失，由后续完整包重建；在途页本就为 -1 |
| 可观察包序列 | `0, GAP, 2, 3, 4` | `0, GAP, 2, 3`（3.3a/3.3b 同形，机制不同） |

自测中两个 "loss of pages" 用例（`framing.c:1862-1966`）用同一份 5 页编码、
同一个被跳页（pageno=3），但向逻辑层提交页的前缀长度不同，从而展示
"洞落在包边界"与"洞落在包中间（回卷 + 续接）"两条路径。
页布局（段表尾段值决定页停在何处）：

```
page[0] BOS  segs=1  尾段 0    （1 个 nil 包完成，body=0）
page[1]      segs=19 尾段 254 （4 个包完成，body=4179）
page[2]      segs=23 尾段 17   （4 个包完成，body=5013）
page[3]      segs=20 尾段 235  （8 个包完成，body=4256；235<255，页停在包边界）
page[4] EOS  segs=2  段 255,45（300B 包；255 起始说明它是跨 page[3]→page[4] 的续包）
```

- **用例 1 "no rollback"**（`framing.c:1862-1909`）：控制流为
  pageout/pagein 两次（page 0、1 入逻辑层），第三次 pageout 只同步**不 pagein**
  （跳过 page 2），再 pageout/pagein 提交 page 3（其页号 3 与期望 2 失配）。
  跳洞时段 FIFO 停在包边界，没有在途残段，回卷为空操作。
  [自测] 断言输出包 `0,1,2,3,4` 后 `packetout()==-1`
  （`framing.c:1900-1903`，不返回 -1 即测试失败），随后是洞后幸存的
  输入包 9（76B）、10（34B）（`framing.c:1904-1907`）。
  `checkpacket` 在 `no>lastno+1` 时把期望序号额外 +1（`framing.c:1039-1040`），
  与 `0x400` 消费时的 `packetno++`（`framing.c:958`）严格对应——
  这正是"框架只报一个洞、包号只跳 1"的可观察证据。
- **用例 2 "rollback with continuation"**（`framing.c:1911-1966`）：
  page 0、1、2 均 pagein，第四次 pageout 只同步不 pagein（跳过 page 3），
  再提交 page 4。此时 page 4 页号 4 ≠ 期望 3，先执行回卷
  （`framing.c:830-832`）再插 `0x400`；page 4 带跨页包的 255 起始段，
  其开头残段按 `framing.c:843-857` 的孤儿续接规则处理。
  [自测] 断言输出包 `0…8`，`packetout()==-1`（`framing.c:1959-1962`），
  然后输入包 17（300B，gran=18000，`framing.c:1963-1964`）。
  实验复现的段 FIFO 末尾为 `… 0x011, 0x400, 0x0ff, 0x22d`，
  可直接看到空洞标记夹在旧包结束段（0x011=17）与新包段（255+45=300B）之间。

两个用例都断言：**丢页后第一次 packetout 必须返回 -1，否则测试失败退出**
（`framing.c:1900-1903`、`1959-1962`），证明空洞信号是恢复边界上的强制可观察事件。

---

## 5. 端到端决策流程（综合四因素）

```
字节到达 ogg_sync
  │
  ├─ 找不到 "OggS" / 段表超长未到齐 ? ── 是 ──> 返回 0，等待更多数据
  │
  ├─ CRC 失配（且未 DISABLE_CRC）? ────── 是 ──> 丢同步，memchr 找下一个 'O'
  │                                             首次返回 -1（物理洞）；坏页整体消失
  │
  └─ CRC 合法页 ogg_page
        │  应用按 serialno 选 ogg_stream_state；不匹配 -> pagein 返回 -1，无状态变化
        ▼
  ogg_stream_pagein
        │
        ├─ pageno == os->pageno ? ── 否 ──> 回卷 [lacing_packet,lacing_fill) 在途残段
        │                                   插 0x400（首页除外），之后用 pageno+1 重对齐
        │
        ├─ 页带 continued 且 (FIFO 空 / 末段<255 / 末段==0x400) ?
        │        是 ──> 跳过页首段直到第一个 <255：孤儿残包残余丢弃
        │        否 ──> 正常续接在途包
        │
        ├─ 拷贝 body、写段表（<255 推进 lacing_packet）、末结束段记 granulepos
        ▼
  ogg_stream_packetout
        ├─ 段首是 0x400 -> 返回 -1（逻辑洞信号），packetno++
        ├─ 只有残段、无 <255 结束 -> 返回 0（包未完成，等待下一页）
        └─ 凑齐结束段 -> 返回完整 ogg_packet（绝无半包），packetno++
```

---

## 6. 结论

1. **恢复边界由两层共同确定**：同步层以 CRC 为唯一把关，损坏页整页丢弃并在
   下一个 "OggS" 重捕获（`framing.c:688-728`）；逻辑层只面对"页号不连续"，
   用 `pageno` 失配检测、段 FIFO 回卷、`0x400` 空洞和 continued 跳过完成组包层恢复。
2. **页序号是逐逻辑流独立的丢页检测器**（RFC 3533 第 6 节），一次失配只报一个洞、
   只让 `packetno` 加 1，与实际丢失包数无关；恢复后立即按页内序号重新对齐。
3. **continued + 段表 255 把损坏的影响精确限制在包边界**：起源于坏页的跨页包
   被整体作废（回卷或孤儿续接跳过），框架保证只输出完整包，永不输出半包；
   坏页上已经完整结束的包不受回卷影响。
4. **granulepos 提供恢复后的时间重建锚点**：纯续接页与在途残段为 -1，
   被丢页的锚点丢失，编解码器从洞后第一个完成包重新获取时间位置。
5. **多逻辑流互不干扰**：物理洞是全局字节事件，但 `0x400`、序号重对齐、
   包号跳跃都发生在各自 serial 的 `ogg_stream_state` 内；serial 不匹配的页
   在 `pagein` 入口即被拒绝。
6. **无损 / 单页 / 跨页三种情形的包序列可观察差异**（本文实验与自测一致）：
   无损为连续 `0..N`；单页独立包损坏为 `0, GAP, 2…`（多个包塌缩成一个洞信号）；
   跨页包损坏同为 `0, GAP, 2…` 的序列形状，但跨页大包从不产生半截输出，
   且分别由"回卷"与"continued 孤儿跳过"两条不同代码路径达成。
7. 编译 `DISABLE_CRC` 会关闭第 1 层把关，损坏页被原样交付（自测相应跳过重捕获
   用例）；此时第 2 层机制仍在，但只能检测真正的丢页/乱序，无法识别页内比特翻转。
