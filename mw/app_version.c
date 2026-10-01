#include "mw/app_version.h"

/* 调试器/产物检索用："APPFW tc275_car v0.2.2"；被 Cpu0_Main 的启动横幅引用，
 * 链接期死码消除（-Wl-Oc）不会剔除。 */
const char g_app_version[] = "APPFW tc275_car v" APP_VERSION_STRING;

const char *app_version_string(void)
{
    return g_app_version;
}

const char *sbl_version_string(void)
{
    /* volatile 读：PFlash 固定地址内容是另一个镜像（SBL）编译期常量，
     * 必须运行时读；magic 校验防"未烧 SBL/老 SBL"时把随机 flash 内容当版本串。 */
    const volatile char *p = (const volatile char *)SBL_VERSION_ADDR;
    if (p[0] != 'S' || p[1] != 'B' || p[2] != 'L' || p[3] != 'F' || p[4] != 'W')
        return 0;
    return (const char *)p;
}

void app_ver_evt_build(uint8 *app_out, uint8 *sbl_out)
{
    uint8 i;

    for (i = 0u; i < APP_VER_EVT_LEN; i++)
    {
        app_out[i] = (uint8)g_app_version[i];
    }
    app_out[APP_VER_EVT_LEN - 1u] = 0u;

    {
        const char *sbl = sbl_version_string();

        for (i = 0u; i < APP_VER_EVT_LEN; i++)
        {
            sbl_out[i] = (sbl != 0) ? (uint8)sbl[i] : 0u;
        }
        sbl_out[APP_VER_EVT_LEN - 1u] = 0u;
    }
}
