#include "mw/app_version.h"

/* SBL 版本串，LSL 定死在 0x80007E00（Lcf_SBL.lsl 的 sbl_version 组，32KB
 * SBL 区尾部）：App（tc275_car 的 SBL_VERSION_ADDR 同一地址约定）与调试器
 * 从该地址直读，magic "SBLFW" 校验。Cpu0_Main 的 volatile 读锚点保证
 * 链接期死码消除（-Wl-Oc）不会剔除它。 */
#if defined(__TASKING__)
#pragma protect on
#pragma section farrom "sbl_version"
#endif

const char g_sbl_version[] = "SBLFW tc275_sbl v" APP_VERSION_STRING;

#if defined(__TASKING__)
#pragma section farrom restore
#pragma protect restore
#endif
