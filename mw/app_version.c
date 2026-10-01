#include "mw/app_version.h"

/* 调试器/产物检索用："APPFW tc275_car v0.2.2"；被 Cpu0_Main 的启动横幅引用，
 * 链接期死码消除（-Wl-Oc）不会剔除。 */
const char g_app_version[] = "APPFW tc275_car v" APP_VERSION_STRING;

const char *app_version_string(void)
{
    return g_app_version;
}
