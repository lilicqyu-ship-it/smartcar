# tc275_sbl —— TASKING TriCore v6.3r1 SCons 构建
#
# 与 AURIX Studio 工程同源：include / 宏 / 源码排除列表解析自 .cproject，
# 编译链接参数复刻 IDE 生成 makefile 的命令行（详见
# site_scons/aurix_tasking.py 头部说明）。
#
# 用法（在本目录执行）：
#   scons                 # Debug（默认；对应 IDE 的 Debug 配置）
#   scons cfg=release     # 对应 IDE 的 Release 源集
#   scons opt=-O2         # 覆盖优化级别（默认 -O0，与 IDE 一致）
#   scons size            # 只跑 elfsize 查看体积
#   scons -c              # 清理 build/
#   scons -j8             # 并行编译
#
# 工具链查找：TASKING_TRICORE_HOME / TASKING_HOME 环境变量
#   -> C:\Program Files\TASKING\TriCore v6.3*（取最新 6.3.x）

import os

import aurix_tasking

CONFIGS = {
    'debug': 'Debug',
    'release': 'Release',
}

variant = ARGUMENTS.get('cfg', 'debug')
if variant not in CONFIGS:
    Abort('cfg=%s 未知，可选: %s' % (variant, ', '.join(CONFIGS)))

# SCons 签名库收进构建目录：默认落在工程根（.sconsign.dblite），根目录必须保持干净
os.makedirs('build', exist_ok=True)
SConsignFile(os.path.join('build', '.sconsign.dblite'))

env = Environment()
aurix_tasking.generate(env)   # 覆盖 CC/LINK/CCCOM 等为 TASKING cctc
elf = aurix_tasking.build(
    env,
    cfg_name=CONFIGS[variant],
    # 产物名带版本（mw/app_version.h 为唯一真源）：tc275_sbl_vX.Y.Z.elf/.hex/.map。
    # IDE（ADS）构建产物名仍为 tc275_sbl.*（.cproject ${ProjName}），不受影响。
    artifact='tc275_sbl_v' + aurix_tasking.header_version('mw/app_version.h'),
    lsl='Lcf_SBL.lsl',                        # 链接脚本（与 IDE 生成命令一致）
    variant='tasking-' + variant,
    opt=ARGUMENTS.get('opt', '-O0'),
)
Default(elf)
