# Ogg 主动出页（flush）与终止页（EOS）的封装延迟分析

范围：仅 Ogg 封装层（libogg 编码侧），即 `src/framing.c` 中
`ogg_stream_packetin` / `ogg_stream_iovecin` 收包与
`ogg_stream_pageout` / `ogg_stream_flush` 系列出页的逻辑；
不涉及任何编解码器（Vorbis/Opus 等）的调度策略。

---

## 1. 包大小如何被概括：lacing 值与段表

`ogg_stream_iovecin`（framing.c:276）把一个完整包追加进流状态的
`body_data` 缓冲，同时把包长分解为 **lacing 值**序列
（framing.c:290, 319-327）：

- 包长 `bytes` 被拆成 `bytes/255 + 1` 个段：前若干段恒为 255，
  最后一段为 `bytes % 255`（0–254）。
- 末段 `< 255` 即"包在此结束"的标记；因此**零长包**（`bytes=0`）
  也占一个 lacing 值 0，而长度恰为 255 倍数的包需要一个额外的 0 段收尾。
- 每个 lacing 值附带一个 granule 值：延续段继承当前 `os->granulepos`，
  末段记录该包提交时的 `granulepos`（framing.c:321-324）。
- 首段被或上 `0x100` 作为"包起始"内部标记（framing.c:327），
  出页时用它决定 continued 标志（见 §4）。

由此得出页的硬约束：一页段表最多 **255 个 lacing 值**
（`maxvals = min(lacing_fill, 255)`，framing.c:358），
即页体上限 255×255 = 65025 字节。任何超过一页的包必然**跨页**，
这是包大小直接决定页边界的第一条途径。

## 2. 出页决策：pageout 与 flush 共用同一个核心

四个公开入口全部汇聚到 `ogg_stream_flush_i(os, og, force, nfill)`
（framing.c:349）：

| 入口 | force | nfill | 语义 |
|---|---|---|---|
| `ogg_stream_pageout` (framing.c:511) | 0（两种例外置 1） | 4096 | 被动：只出"名义满"页 |
| `ogg_stream_pageout_fill` (framing.c:526) | 0（同上） | 调用者给 | 同上，页大小可调 |
| `ogg_stream_flush` (framing.c:495) | 1 | 4096 | 主动：有数据就出页 |
| `ogg_stream_flush_fill` (framing.c:503) | 1 | 调用者给 | 主动 + 页大小可调 |

`pageout` 的两个例外（framing.c:515-517）：
`e_o_s && lacing_fill`（流已终止且尚有残留数据）或
`lacing_fill && !b_o_s`（初始头页）时强制 `force=1`。

### 2.1 正常满页（pageout 的隐式出页条件）

非首页路径（framing.c:385-399）顺序扫描段表，累计字节数 `acc`，
在两个条件下把 `force` 置 1：

1. **`acc > nfill && packet_just_done >= 4`**：累计字节超过名义页大小
   （默认 4096）**且**最近已连续完成至少 4 个包，就在包边界处切页。
   注释（framing.c:376-383）说明这是启发式：不轻易跨页，
   且小包流要攒够 4 个包才允许超 4K 切页，以摊薄页头开销。
2. **`vals == 255`**：段表满了，无条件切页（framing.c:399）——
   大包的 255 段继续段、或 255 个小包，都会触发。

页边界永远落在**包边界**上（除非单包超页被迫跨页），
切页点取"超过阈值之后的第一个包尾"。

### 2.2 显式 flush

`ogg_stream_flush` 以 `force=1` 进入，只要 `lacing_fill > 0`
就立即构造一页，无论页体多小。注意（framing.c:481-493 的注释）：
一次 flush 只出**一页**，若残留数据超过 255 段需循环调用，
**返回 0 才表示全部排空**。flush 不改变 §2.1 的段数扫描逻辑
（`vals` 仍由 4 包/4096 字节启发式或 255 段上限决定），
它改变的只是"不够条件也允许出页"。

### 2.3 流结束与不完整尾包

- 应用在最后一个包上置 `op->e_o_s=1`，`packetin` 把它记入
  `os->e_o_s`（framing.c:334）。此后 `pageout` 的例外条件生效：
  只要还有未出页的 lacing 数据就强制出页，直到缓冲排空——
  **EOS 流的尾部不需要显式 flush 也能被 pageout 排干**。
- EOS 页标志的设置条件（framing.c:416）：
  `os->e_o_s && os->lacing_fill == vals`，即"本次出页恰好清空全部
  残留段"。若尾包跨页，前面各页不带 EOS，只有含包尾的最后一页带。
- 编码侧不存在"半个包"：packetin 一次性拷贝整包，
  所以"不完整尾包"只会以**跨页包的未完部分**形式存在于缓冲中；
  EOS 强制出页会把这部分以 continued 页形式排出，
  最后一页同时置 continued(0x01) 与 EOS(0x04)。
  （解码侧对应地，`ogg_stream_pagein` 在页号跳变时回滚未完成的
  部分包，framing.c:826-839。）
- 若 `e_o_s` 置位后 `lacing_fill==0`（所有数据已出页），
  `pageout` 不再出页——**不会产生空的纯 EOS 页**；
  EOS 标志总是附着在最后一张有数据的页上。

## 3. BOS 与首页的冲突裁决

`b_o_s==0` 时走"初始头页"分支（framing.c:366-373）：首页**只包含
第一个包**，granulepos 强制为 0，与 4096 字节启发式无关。
若首包本身超过 255 段，`maxvals` 把它截在 255 段，首包跨页，
后续页仍是普通页（BOS 只在第一页，framing.c:414, 417 出页后即置
`b_o_s=1`）。自测用例 `head1_3`（18 段、4345 字节首包，
framing.c:1176-1182）正是这条路径的 framing 证据。

于是三类"冲突"的裁决优先级为：

1. **BOS**：首页只装首包，忽略尺寸启发式；
2. **255 段硬上限**：任何情况下都强制切页（大包跨页）；
3. **4 包 + nfill 字节启发式**：仅普通页、且 `force=0` 时起作用；
   `flush` 或 EOS 把 `force` 置 1 后，该启发式只决定"这一页装到哪里"，
   不再阻止出页。

## 4. 可观察的页面标志（framing 证据）

出页时头部第 5 字节（framing.c:410-416）：

| 位 | 含义 | 设置条件 |
|---|---|---|
| 0x01 continued | 首页段是上一页包的延续 | 首段无 `0x100` 起始标记（framing.c:412） |
| 0x02 BOS | 逻辑流首页 | `b_o_s==0`（framing.c:414） |
| 0x04 EOS | 逻辑流末页 | `e_o_s` 且本次出页清空全部段（framing.c:416） |

granulepos 的页级语义（framing.c:387-397）：取页内**最后一个完成包**
的 granule 值；若页内没有任何包完成（整页都是跨页包的中段），
`granule_pos` 保持初值 **-1**（线上为 8×0xFF）。
自测数据提供了直接证据：

- `head2_4`（framing.c:1200）：flags=0x00、255 段全为 255——
  整页是一个 65500 字节包的中段，无 BOS/continued/EOS
  （它是流内第二页，首页恰好是 nil 包）。
- `head3_6`（framing.c:1371）：flags=0x01、granulepos 全 0xFF——
  跨页包占据整页，continued 置位、granulepos=-1。
- `head3_7`（framing.c:1463）：flags=0x05（continued|EOS）、
  段表仅 `[0]`——64770 字节尾包的收尾段是 0 长段，
  EOS 页体可以为 0 字节但仍携带段表项。
- `head1_0`（framing.c:1138）：flags=0x06（BOS|EOS）——
  单包流，首尾页合一。

解码侧 `ogg_page_continued/bos/eos`（framing.c:37-47）逐位读回这些标志，
`ogg_page_packets`（framing.c:95）以"段值 <255 的个数"统计页内完成包数，
与编码侧的 lacing 构造完全对偶。

## 5. 粒度与返回结果

- **粒度**：出页以**页**为单位，页边界在包边界（单包超页时退化为
  255 段边界）。应用无法要求"出半页"或按字节切页；
  延迟的最小单位是"一次出页调用产生一页"。
- **返回值**：四个出页入口均返回 `1`（交出一页）或 `0`（无页可出，
  含缓冲为空、`pageout` 未达满页条件、流状态异常）。
  `ogg_stream_flush` 返回 0 是"全部数据已入页"的唯一判据。
- **延迟画像**：
  - 纯 `pageout`：小包流延迟取决于何时攒够"4 包且 >4096 字节"
    或 255 段——低码率流可能长时间不出页；
  - 显式 `flush`：立即以欠载页换低延迟，代价是页头开销占比升高；
  - `flush_fill`/`pageout_fill` 的 `nfill` 让应用自己权衡
    页大小与延迟（framing.c:499-501 注释）；
  - EOS：提交末包后 `pageout` 自动转为强制模式，
    尾页（可能欠载、可能带 continued）随下一次调用即出，
    终止延迟为一次 `pageout` 调用。
- 返回的 `ogg_page` 指针指向 `ogg_stream_state` 内部缓冲
  （framing.c:507-509），下一次同状态调用即失效；
  已出页数据经 `body_returned` 惰性回收（framing.c:292-302, 471）。

## 6. 小结

页边界由三个因素共同决定：**包大小**（lacing 分解与 255 段上限，
决定跨页与硬切页）、**flush 策略**（`force` 与 `nfill` 决定
欠载页是否允许、阈值多高）、**BOS/EOS 状态**（首页只装首包、
EOS 强制排空并在最后一页置 0x04）。延迟的本质是"缓冲中未入页的
lacing 数据量"：pageout 用欠载抑制换效率，flush 用页头开销换实时性，
EOS 则一次性放弃效率保证终止页即时可见——三者在
`ogg_stream_flush_i` 的 `force`/`nfill` 两个参数上统一。
