# Ogg 页损坏后的逻辑流恢复边界分析

> 分析对象：本仓库 libogg（`src/framing.c`、`include/ogg/ogg.h`），协议依据 `doc/rfc3533.txt`（RFC 3533），测试依据为 `framing.c` 内 `_V_SELFTEST` 自测（即 `make check` / ctest 的 `test_framing`）。
> 本文只分析与实测，不修改任何代码。

## 1. 结论速览

Ogg 的损坏恢复分两层，各自有独立的"恢复边界"：

| 层次 | 状态对象 | 判定手段 | 损坏时的动作 | 应用可观察信号 |
|---|---|---|---|---|
| 物理同步层 | `ogg_sync_state` | 捕获模式 `OggS` + 段表长度 + **整页 CRC32** | 丢弃整页，逐字节搜索下一个 `OggS` 重新捕获 | `ogg_sync_pageout()` 返回 **-1**（一次重捕获只报一次） |
| 逻辑分包层 | `ogg_stream_state`（每个 serial 一份） | 页序号 `pageno`、continued 标志、段表 255 约定、serial | 回滚未完成包、插入 0x400 空洞标记、跳过孤儿续段 | `ogg_stream_packetout()` 返回 **-1**，随后 `packetno` 跳号 |

四个字段在恢复决策中的角色：

- **页序号（page_sequence_number，header[18..21]）**：逻辑层检测丢页/重捕获的唯一依据。`pageno != os->pageno` 即判定出现空洞（`framing.c:826`）。
- **continued packet 标志（header_type bit 0x01，header[5]）**：决定续页的段是拼接到已有半包，还是作为"孤儿续段"被跳过（`framing.c:843-858`）。
- **粒度位置 granule_position（header[6..13]）**：**不参与任何恢复判定**，只是时间/位置提示；仅挂到本页最后一个完整包上，页内无完整包时为 -1（`framing.c:871, 886-889`）。
- **多逻辑流状态 serial number（header[14..17]）**：决定页归属哪个 `ogg_stream_state`；不匹配直接拒收（`framing.c:820`），各逻辑流的 `pageno`/半包缓冲/空洞计数互相隔离。

关键边界：**同步层保证"交给逻辑层的每一页 CRC 都正确"，但它不理解包；逻辑层理解包，但它信任同步层送来的页，只用页序号发现"页缺失"。** 一个跨多页的大包只要其中一页在同步层被丢弃，整个包都无法恢复，即使其余页完好——这就是独立包损坏与跨页包损坏结果不同的根源。

---

## 2. 两层恢复机制的源码走读

### 2.1 物理同步层：CRC 失败即丢失整页（`ogg_sync_pageseek`）

`ogg_sync_pageseek()`（`src/framing.c:642-729`）的状态机：

1. 缓冲区不足 27 字节固定头 → 返回 0，等待更多数据（`framing.c:654`）。
2. 头四字节不是 `OggS` → 进入 `sync_fail`（`framing.c:657`）。
3. 按 `header[26]`（段表项数）算出整页长度 `headerbytes = page[26]+27`，并累加段表得到 `bodybytes`；数据未到齐 → 返回 0（`framing.c:659-669`）。
4. 整页缓冲齐后，把 CRC 字段清零重算 CRC32，与页内存储的校验和比较（`framing.c:672-698`）。**不一致就 `goto sync_fail`**（编译期未定义 `DISABLE_CRC` 时）。
5. `sync_fail`（`framing.c:717-728`）：用 `memchr(page+1,'O',...)` 寻找下一个可能的捕获点，返回负数 `-(next-page)` 表示跳过的字节数；`ogg_sync_pageout()`（`framing.c:742-770`）在首次跳过时置 `unsynced=1` 并向应用返回 **-1**，之后静默循环寻找，直到下一页 CRC 通过返回 1。

含义：**损坏页的全部字节（连同它承载的所有完整包与半包片段）都不会进入逻辑层**；恢复点是下一个 CRC 合法页的 `OggS`。RFC 3533 第 6 节明确写道：捕获模式"helps a decoder to find the page boundaries and regain synchronisation after parsing a corrupted stream. Once the capture pattern is found, the decoder verifies page sync and integrity by computing and comparing the checksum."（`doc/rfc3533.txt:513-519`）。

> 注：仓库提供 `--disable-crc`（`framing.c:694-697` 的 `DISABLE_CRC`，见 `CHANGES` 与 `CMakeLists.txt:181-196` 的测试开关），此时校验和失配不再丢页，仅供 fuzzing；正常构建不走这条路径。自测在该构建下会跳过两个 CRC 相关用例（实测输出 `Skipping continuation resync test ...` 与 `Skipping recapture test ...`）。

### 2.2 逻辑分包层：页序号、回滚、空洞标记与孤儿续段

`ogg_stream_pagein()`（`src/framing.c:775-902`）处理一页：

**(a) 身份与版本门控**：serial 不符返回 -1（`framing.c:820`），version>0 返回 -1（`framing.c:821`）。物理层把多流交错的页都交上来，靠这一行分流到各自的 `ogg_stream_state`。

**(b) 页序号检查 —— 回滚 + 空洞标记**（`framing.c:826-839`）：

```c
if(pageno!=os->pageno){
  /* unroll previous partial packet (if any) */
  for(i=os->lacing_packet;i<os->lacing_fill;i++)
    os->body_fill-=os->lacing_vals[i]&0xff;
  os->lacing_fill=os->lacing_packet;
  /* make a note of dropped data in segment table */
  if(os->pageno!=-1){
    os->lacing_vals[os->lacing_fill++]=0x400;
    os->lacing_packet++;
  }
}
```

- `lacing_packet` 指向最后一个**已完整**包之后的位置，`lacing_packet..lacing_fill` 之间是上一页遗留、尚未闭合的半包段。序号不连续时，这段半包从 body 缓冲里按段长"倒账"并截断 FIFO——即**回滚未完成包**，已完整交付过的包不受影响。
- 随后写入特殊段值 **0x400** 作为空洞标记（仅当本流此前已同步过，`pageno!=-1`，所以 BOS 前的乱序不产生假空洞；`ogg_stream_reset()` 把 `pageno` 置 -1，`framing.c:930`）。
- 无论缺多少页，一次序号失配只插**一个** 0x400，空洞在包序列中表现为单个缺口。

**(c) continued 页的孤儿续段跳过**（`framing.c:843-858`）：

```c
if(continued){
  if(os->lacing_fill<1 ||
     (os->lacing_vals[os->lacing_fill-1]&0xff)<255 ||
     os->lacing_vals[os->lacing_fill-1]==0x400){
    bos=0;
    for(;segptr<segments;segptr++){
      int val=header[27+segptr];
      body+=val; bodysize-=val;
      if(val<255){ segptr++; break; }
    }
  }
}
```

续页本应延续一个"最后一段值为 255"的打开包。若当前没有打开包（FIFO 空、最后一段 <255 表示上一包已闭合、或最后一段正是 0x400 空洞标记），说明续页前面的页丢了——页首直到第一个 `<255` 段值为止的内容属于一个再也拼不起来的残缺包，**按段表长度直接跳过、不拷入 body**，并清掉本地 `bos` 防止把残缺首段误标成流首包。这正是 `CHANGES` 中 1.3.3 版"Fix an issue with corrupt continued packet handling"对应的逻辑（显式把 `==0x400` 纳入判定，避免把空洞后的新包错续到残包上）。

**(d) 段表入队与 granulepos 归属**（`framing.c:866-891`）：剩余段写入 `lacing_vals`，首段在 BOS 时或上 0x100 标记；每遇到 `<255` 就推进 `lacing_packet`（一个包闭合）。只有存在闭合包时（`saved!=-1`）才把页头 granulepos 挂到最后一个闭合包的 `granule_vals` 上；纯续页（无闭合包，如自测 `head2_6`，granule 字段为全 0xFF=-1）不更新任何包的粒度位置。

**(e) 出包时上报空洞**（`_packetout`，`framing.c:954-960`）：

```c
if(os->lacing_vals[ptr]&0x400){
  os->lacing_returned++;
  os->packetno++;
  return(-1);
}
```

应用收到一次 **-1** 表示"这里有包丢失"，`packetno` 已自增；后续完整包的 `packetno` 因此整体跳号。`ogg.h:77-81, 97-101` 的注释说明这是刻意的层间耦合：framing 知道空洞在哪，但要让独立的 codec 层也知道。

### 2.3 granulepos 为什么不决定恢复

同步层只看捕获模式、段表长度与 CRC（`framing.c:652-699`），逻辑层只看 serial/version/pageno/continued/段表（`framing.c:781-858`），granulepos 在这两条路径上从未参与条件判断。它仅在 `framing.c:886-889` 作为元数据保存，并在 `_packetout` 中随包返回（`framing.c:985`）。RFC 3533 也把它定义为"a hint for the decoder ... timing and position information"，-1 表示本页无包结束（`doc/rfc3533.txt:555-562`）。因此应用**不能靠 granulepos 跳变自行判定丢包**，权威信号是 `ogg_sync_pageout()==-1` 与 `ogg_stream_packetout()==-1`。

---

## 3. 三种损坏场景与可观察的包序列

以下序列来自两类证据：(1) 仓库自测 `test_framing` 的实际通过结果；(2) 为本文编写、仅链接本仓库库文件的观察程序（位于仓库外 `/tmp/ogg-demo/`，可复现命令见第 5 节）。包编号为 `ogg_packet.packetno`。

### 3.1 无损基线

编码端 5 个包 `{0, 100, 130049, 259, 255}` 字节，实测页布局（与自测 `head1_6..head4_6` 同构）：

| 页 | 标志 | 段数 | 本页闭合包 | granulepos |
|---|---|---|---|---|
| 0 | BOS | 1 | 包0（nil，0 字节） | 0 |
| 1 | — | 255 | 包1（100B）；并放下 130049B 大包的前 65025B | 2000 |
| 2 | CONT | 255 | **0 个**（纯续页，`ogg_page_packets()==0`） | -1（头字段 0xFF…） |
| 3 | CONT+EOS | 5 | 包2（130049B 跨页闭合）、包3、包4 | 5000 |

实测解码包序列：`0, 1, 2, 3, 4` 连续，`packetout` 无 -1，`ogg_sync_pageout` 无 -1。自测中 `test_pack(...,0,0,0)` 全部用例（单页、nil 包、4k 以上首包、单包跨页、255 段上限、超大包、零数据页）均校验通过，且 `inptr==outptr==deptr==depacket`（`framing.c:1663-1674`），证明无损时字节与包双路径完全闭合。

### 3.2 单页损坏（损坏页只承载独立、完整的包）

观察程序场景 `demo2`：20 个 300B 独立包编成 3 页（页 0 含包 0；页 1 含包 1..14；页 2 含包 15..19），翻转页 1 头中的 CRC 字节制造校验和失配。实测：

```
page in: pageno=0 ...            packet packetno=0 bytes=300 granule=0
sync -1 recapture               ← 同步层丢弃页 1，返回一次 -1
page in: pageno=2 granule=2000
  GAP packetout=-1 (packetno now 2)
  packet packetno=2..6 ... EOS  ← 原包 15..19，编号变为 2..6
```

恢复边界与序列变化：

1. 同步层：页 1 整页丢弃（含其上 14 个独立包），重捕获到页 2，`ogg_sync_pageout` 返回一次 -1。
2. 逻辑层：收到 pageno=2 而期望 1 → 序号失配；因为没有打开的半包，回循环是空操作，写入一个 0x400。
3. 页 2 不是 continued 页，段全部正常入队；`packetout` 先吐出一次 -1（空洞），`packetno` 由 1 跳到 2，随后交付现存的 5 个包，编号 **2,3,4,5,6**。
4. **净损失 = 损坏页上的 14 个包；损坏前后页的包零损伤，无孤儿段需要跳过。** 空洞在序列中只占一个编号缺口。

仓库自测的对应证据是 "Testing loss of pages"（`framing.c:1862-1909`）：在 5 页布局中跳过页 2 再送页 3、4。实测断言的包序列为完整包 `0,1,2,3,4` → `packetout()==-1` → 完整包 `9,10`（原编号，见 `checkpacket(&test,76,9,...)` 等，`framing.c:1890-1907`）。页 2 上闭合的包 5..8 随页一起损失，页 3 上在缺失点之后才闭合的包 5..8 残段被序号检查回滚，页 4 打开的新包不受影响。

### 3.3 跨页包损坏（损坏的是续页，或整页丢失）

观察程序场景 `demo mode 2`：翻转 3.1 布局中**页 2（纯 CONT 续页）**的 CRC。实测：

```
page 0 → packet 0
page 1 → packet 1
sync -1 recapture               ← 页 2 在同步层被丢
page 3 (CONT) accepted:
  GAP: packetout=-1, next packetno jumps to 3
  packet packetno=3 bytes=259 ...
  packet packetno=4 bytes=255 ... EOS
```

恢复边界与序列变化：

1. 页 1 已把 130049B 大包的前 65025B 作为打开的半包留在 `lacing_packet..lacing_fill`。
2. 页 2 被同步层丢弃；页 3（CONT）到达时 `pageno(3)!=os->pageno(2)` → **回滚**：页 1 留下的整段半包按段长从 body 扣除、FIFO 截断到 `lacing_packet`，并插入 0x400。
3. 页 3 虽带 continued 标志，但 FIFO 末尾已是 0x400，命中孤儿续段条件（`framing.c:844-846`）：页 3 开头属于大包尾巴的段（直到第一个 <255 段值）被**跳过不拷贝**；其后同页的包 3（259B）、包 4（255B）作为新包正常入队。
4. `packetout` 序列：`0, 1` → **-1（空洞，packetno 跳到 3）** → `3, 4`。
5. **净损失 = 整个 130049B 包（包 2），即使页 1、页 3 的字节都完好。** 这就是与 3.2 的本质区别：独立包损坏的损失被页边界限制，跨页包损坏的损失按**包边界**扩大——一个段的缺失通过"回滚 + 孤儿跳过"吃掉三段数据。

对照场景 `demo mode 3`（不损坏 CRC，直接从字节流中抽掉页 2，模拟传输丢页）：同步层完全无感知（**0 次 recapture**），页 3 以合法 CRC 到达，逻辑层仅凭 pageno 跳变（1→3）触发完全相同的回滚、0x400 与孤儿跳过，输出序列同为 `0,1 → -1 → 3,4`。说明**逻辑流恢复边界由页序号驱动，与损坏在物理层还是传输层发生无关**。

仓库自测的对应证据：

- "Testing loss of pages (rollback required)"（`framing.c:1911-1966`）：页 0..2 正常 `pagein`，**跳过页 3 的 `pagein`**（页仍过 sync），再送页 4。页 3 上闭合的包 9..16 中，已在页 2/3 缓冲的残段在页 4 序号失配时被整体回滚；页 4 的 EOS 包 17（300B）经孤儿续段处理后完好交付。实测断言序列：`0..8` 完整 → 包 5、6、7、8 仍在（它们在页 2 已闭合）→ `packetout()==-1` → 包 17（`framing.c:1941-1964`）。与 mode 3 的差别只是丢页位置落在一个已有半包的中间，因而可以直接观察到"回滚"而不仅是空洞标记。
- "testing continuation resync in very large packets"（`framing.c:1796-1808`，调用 `test_pack(pl, head1_6.., byteskip=100, pageskip=2, packetskip=3)`）：从物理流最前面截断 100 字节，使页 0、页 1 的 CRC 先后失败并触发重捕获，直到页 2 才重新同步；页 2 是 130049B 大包的纯续页，到达逻辑层时没有打开包，孤儿续段逻辑把整页段跳过，页 3 闭合后的首个可交付包正是原包 3（`packetskip=3` 对应 `depacket` 预置偏移，`framing.c:1521-1522`）。这是 libogg 1.1.1 "resync in large continuation bug" 的回归测试（注释见 `framing.c:1798-1799`）。
- "Testing recapture"（`framing.c:2078-2119`）：页 + 垃圾 + 页，验证 CRC 失败后 `ogg_sync_pageout` 先返回 0/负值再在下一合法页恢复，且该用例在 `DISABLE_CRC` 下编译时被跳过。

### 3.4 两种损坏的结果对比

| 维度 | 独立包页损坏（3.2） | 跨页包损坏（3.3） |
|---|---|---|
| 同步层动作 | 丢 1 页，1 次 `sync -1` | 丢 1 页（或传输层丢页时 0 次） |
| 序号失配时回滚 | 无打开半包，回循环为空 | 回滚此前页中已缓冲的全部残段 |
| 孤儿续段跳过 | 通常不触发（下一页面貌正常） | 必然触发，续页首部残段被丢弃 |
| 实际丢失的数据 | 仅损坏页上闭合的包 | **整个跨页包**，含其余完好页上的片段 |
| 空洞标记 0x400 | 1 个 | 1 个（无论跨多少页，都合并为一个缺口） |
| 恢复后首个包 | 下一完好页上的第一个新包 | 残包被跳过后，同页/次页上第一个闭合的新包 |
| 实测包序列 | demo2：`0 → -1 → 2..6`；mode 1（BOS 页损坏）：`0 → -1 → 1,2,3,4` | `0,1 → -1 → 3,4`（mode 2/3） |
| granulepos | 幸存包的粒度位置原样保留 | 残包无 granule 输出；幸存包粒度位置原样保留 |

---

## 4. 多逻辑流（multiplex）状态如何隔离

RFC 3533：物理流由多个逻辑流按**页级交错**复用，每个逻辑流以独立 serial 标识，解复用把页"redirecting them into the appropriate logical decoding entity"（`doc/rfc3533.txt:163-183`）。libogg 的落实方式：

- 应用为每个 serial 维护独立的 `ogg_stream_state`，其 `pageno`、`lacing_*`、`body_*`、`packetno`、`e_o_s/b_o_s` 全部各自独立（结构定义见 `ogg.h:52-84`）。
- `ogg_stream_pagein()` 对 serial 不符的页直接返回 -1、不改任何状态（`framing.c:820`）。
- 同步层 `ogg_sync_state` 是物理流唯一的，CRC 重捕获对所有流一视同仁；但一次损坏只影响其 serial 对应的那个逻辑流——其他流的页 CRC 仍然合法、序号仍然连续。

实测（`demo mode 4`，流 A=0x11111111 与流 B=0x22222222 交错，损坏 A 的第 2 页）：

- 解码 A：页 0 交付包 0 后收到一次 `sync -1`，A 的 EOS 页随损坏一起丢失，A 流终止在空洞处。
- 解码 B：同一物理缓冲里 B 的两页均被正常接受（A、B 互相的页在对方 `pagein` 处打印 `rejected ... serial mismatch`），包序列 `0,1,2,3` 完整、granule 4000 与 EOS 正常。
- 即 **A 流的损坏不改变 B 流的 `pageno` 期望，也不向 B 插入 0x400**；隔离边界就是 serial 门控（`framing.c:820`）加上每流独立状态。

---

## 5. 证据清单与复现方式

### 5.1 仓库内证据（未修改源码）

- 自测源码：`src/framing.c:1013-2138`（`_V_SELFTEST`），构建入口 `src/Makefile.am:10-18`、`CMakeLists.txt:181-196`。
- 实测运行（本文环境中）：
  - `cmake -S . -B build -DBUILD_TESTING=ON && cmake --build build` 后运行 `build/test_framing`，16 个子用例全部以 `ok.` 结束，进程退出码 0；`test_bitwise` 退出码 0。
  - 关键输出行：`testing continuation resync in very large packets... 0, 1, 2, (2), 3, (3), granule:4103 ok.`（编码侧页 0、1 被跳过，解码侧从页 2 重新计入）、`Testing loss of pages... ok.`、`Testing loss of pages (rollback required)... ok.`、`Testing recapture... ok.`。
  - `-DDISABLE_CRC` 变体编译运行后输出 `Skipping continuation resync test ...` 与 `Skipping recapture test ...`，佐证两条恢复路径均以 CRC 为前提。
- 协议证据：`doc/rfc3533.txt:479-562`（页头字段语义）、`:513-519`（捕获模式与校验和重同步）、`:163-183`（多逻辑流复用与 serial 分流）。
- 历史证据：`CHANGES` 1.3.3 条 "Fix an issue with corrupt continued packet handling"，对应 `framing.c:843-858` 的孤儿续段判定。

### 5.2 独立观察程序（仓库外，未触碰仓库）

为避免污染仓库，观察程序放在 `/tmp/ogg-demo/`（`demo.c`、`demo2.c`、`demo3.c`），仅 `#include <ogg/ogg.h>` 并链接本仓库构建出的 `libogg.a`，复现命令：

```sh
cmake -S <repo> -B /tmp/ogg-build -DBUILD_TESTING=ON && cmake --build /tmp/ogg-build
gcc -I/tmp/ogg-build/include -I<repo>/include /tmp/ogg-demo/demo.c  /tmp/ogg-build/libogg.a -o /tmp/ogg-demo/demo
gcc -I/tmp/ogg-build/include -I<repo>/include /tmp/ogg-demo/demo2.c /tmp/ogg-build/libogg.a -o /tmp/ogg-demo/demo2
gcc -I/tmp/ogg-build/include -I<repo>/include /tmp/ogg-demo/demo3.c /tmp/ogg-build/libogg.a -o /tmp/ogg-demo/demo3
/tmp/ogg-demo/demo 0   # 无损；1=单页损坏；2=跨页续页损坏；3=整页丢失(CRC合法)；4=双流隔离
/tmp/ogg-demo/demo2    # 纯独立包页损坏的干净对照
/tmp/ogg-demo/demo3    # 导出自测 5 页布局的标志/段表/granule
```

本文第 3、4 节引用的所有包序列、`sync -1`/`GAP` 计数与 serial 拒收记录均为上述程序在本机的真实输出。

## 6. 小结

1. **跳过还是恢复，由两层各自决定**：CRC 不过 → 同步层跳过整页并在下次 `OggS` 重捕获；CRC 过但页序号不连续 → 逻辑层回滚半包、插 0x400、按 continued 标志跳过孤儿续段后继续出包。
2. **页序号是逻辑层空洞的唯一触发器**，continued 标志决定残段如何切除，granulepos 仅是附加在完整包上的时间提示，不参与任何恢复分支。
3. **恢复边界以包为单位扩大**：独立包损坏的损失被限制在损坏页内；跨页包只要丢一页，整个包通过"回滚 + 孤儿跳过"被完整丢弃，但同页后续新包与后续页不受影响。
4. **空洞在包序列中始终是一个编号缺口、一次 `packetout()==-1`**，无论物理上丢了多少页或多少字节；多逻辑流下该缺口被 serial 隔离在受影响的那一个 `ogg_stream_state` 内。
