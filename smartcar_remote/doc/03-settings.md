# 03 用户设置（scr_settings）

| 项 | 内容 |
|---|---|
| 代码位置 | `main/scr_settings.c/.h` |
| 上游需求 | spec §15（死区可配置）、§31-33（Control 设置）、§35（Radio 设置）、§39（固定 Peer 起步） |
| 状态 | 🟩 **代码完成** — 见文末完成状态表 |

## 1. 职责

单结构体 NVS 设置：Wi-Fi 凭据（连 C6 softAP）、配对 token、驾驶模式、摇杆
死区。加载于 boot，写回于变更，互斥保护读写一致性。

## 2. 数据与 NVS 布局

```c
typedef struct {
    char    ssid[33];       /* NVS "ssid"；缺省 CONFIG_SCR_AP_SSID        */
    char    pass[65];       /* NVS "pass"；缺省 CONFIG_SCR_AP_PASS        */
    char    token[65];      /* NVS "tok"；64 hex，空 = 未配对             */
    uint8_t mode;           /* NVS "mode"；scr_mode_t（ECO/NORMAL/SPORT） */
    uint8_t deadzone_pct;   /* NVS "dz"；径向死区 %（0..40 合法域）       */
} scr_settings_t;           /* namespace "scr" */
```

- 默认值：SSID/密码来自 Kconfig（台架 `SD-DEV000` / `sddev123456`，与
  esp32c6_car `CONFIG_C6_FACTORY_DEV_OVERRIDE` 口径一致）；模式 NORMAL；死区 8 %
  （spec §15：默认值由实驾测试最终确定）。
- `update()` 整体写回（5 键 + commit），失败仅告警不回滚内存值。

## 3. 接口（全量）

```c
void scr_settings_init(void);
void scr_settings_get(scr_settings_t *out);
void scr_settings_update(const scr_settings_t *in);
void scr_settings_set_token(const char *token);           /* 配对成功后    */
void scr_settings_set_control(uint8_t mode, uint8_t dz);  /* Control 设置页 */
```

## 4. 使用关系

| 写入方 | 场景 |
|---|---|
| scr_link.pair_do | 配对成功 → set_token → 重连 WS |
| UI Control 设置页 | 模式三选 / 死区滑条（RELEASED 时落盘） |
| UI Radio 设置页 | SSID/密码 APPLY → update + `scr_link_apply_wifi()` |
| scr_ctrl | 30 Hz 读 mode（限幅）与 UI 读 deadzone（摇杆按压起始时采样） |

## 5. 资源

1 个互斥；NVS 读写走系统 NVS 任务。

## 6. 完成状态表

| # | 功能 | 状态 | 证据 / 缺口 |
|---|---|---|---|
| T-1 | 五键读写 + 缺省回退 | ✅ | `scr_settings_init` |
| T-2 | token / 控制项便捷写 | ✅ | `set_token` / `set_control` |
| T-3 | NVS 断电持久化 | 🟩 | 逻辑完整，待真机重启验证 |
| T-4 | 显示设置（亮度/主题） | ⚪ | SUB3 背光硬线常亮、无亮度控制（BSP 确认），Display 页如实标注（spec §57 不伪造）；主题 V1.0 仅 DARK（spec §52） |
| T-5 | 高级 Radio 参数（信道/协议） | ⚪ | spec §35：一阶段隐藏，防用户改失联 |
