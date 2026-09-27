<!-- CODEGRAPH_START -->
## CodeGraph

In repositories indexed by CodeGraph (a `.codegraph/` directory exists at the repo root), reach for it BEFORE grep/find or reading files when you need to understand or locate code:

- **MCP tool** (when available): `codegraph_explore` answers most code questions in one call — the relevant symbols' verbatim source plus the call paths between them, including dynamic-dispatch hops grep can't follow. Name a file or symbol in the query to read its current line-numbered source. If it's listed but deferred, load it by name via tool search.
- **Shell** (always works): `codegraph explore "<symbol names or question>"` prints the same output.

If there is no `.codegraph/` directory, skip CodeGraph entirely — indexing is the user's decision.
<!-- CODEGRAPH_END -->

## AI 作业前必读

本仓库为 **Infineon TC275 三核 + ESP32-C6** 的智能小车 TC275 侧固件。在改任何代码前，先读 **[doc/30-tc275/33-ai-codebase-guide.md](doc/30-tc275/33-ai-codebase-guide.md)**（AI 协作指南）：代码地图、跨核数据流与单位域、三条高频改动路径的正确改法、安全机制现状、验证命令、9 条红线速查、硬件事实速查。

几条最关键的红线（详见指南）：
- 三核分工固定：**CPU0=FreeRTOS，CPU1/CPU2=裸机（禁用任何 FreeRTOS API）**；核间只走 `mw/xcore` 共享内存。
- 单位域别串：`±100`(协议) / `±1000`(电机·伺服·编码器 pct×10) / `mm/s`(遥测)。
- 改 SF 帧/遥测层后**必须**跑 `test/host/` 主机单测；固件只能在 AURIX Development Studio(TASKING) 里构建，**主机/CI 无法编译固件**。
- 改 `link.c`/`spi_hal_pins.c`：板间 SPI **从未两板通电联调（G1 未过）**，改动需台架验证。
- 改行为**同批改文档**（真源见 `doc/00-index.md §5`）。
