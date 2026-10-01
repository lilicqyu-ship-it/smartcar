#ifndef MW_APP_VERSION_H
#define MW_APP_VERSION_H

/*
 * 应用固件版本（SemVer）—— 本工程唯一的版本真源。
 *
 * 发布流程：bump 三个数字 -> 提交 -> 打同名 git tag（如 v0.2.2）。
 * 可见性：
 *   - 启动横幅经 ASCLIN0 打印（Cpu0_Main，"APPFW tc275_car vX.Y.Z"）；
 *   - SCons 产物名自动携带版本（build 各配置目录下 tc275_car_vX.Y.Z.elf 与 .hex）；
 *   - 产物内可检索：strings tc275_car_vX.Y.Z.elf | grep APPFW。
 */

#define APP_VERSION_MAJOR 0
#define APP_VERSION_MINOR 2
#define APP_VERSION_PATCH 2
#define APP_VERSION_STRING "0.2.2"

/* 魔术前缀 "APPFW" 使版本串在 elf/hex 里可直接检索 */
extern const char g_app_version[];

const char *app_version_string(void);

/*
 * SBL 版本读取：tc275_sbl 的 Lcf_SBL.lsl 把 .sbl_version 组（magic
 * "SBLFW" 的 const 版本串）定死在 0x80007E00 —— sblfls0 32KB 区尾部，
 * SBL 代码自低向高生长，尾部定址块永不挪位。运行时从该地址直读即可。
 */
#define SBL_VERSION_ADDR 0x80007E00u

/* 返回 SBL 版本串（"SBLFW tc275_sbl vX.Y.Z"）；SBL 未烧/老 SBL（magic
 * 不符）时返回 NULL。 */
const char *sbl_version_string(void);

#endif
