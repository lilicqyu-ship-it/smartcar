# -*- coding: utf-8 -*-
"""AURIX + TASKING TriCore 工具链的 SCons 支持（两仓库各持一份相同副本）。

设计目标：SCons 构建与 AURIX Studio（Eclipse CDT + com.infineon.aurix.buildsystem）
的托管 TASKING 工程同源同步——include 路径、宏、源码排除列表直接解析 .cproject，
编译/链接参数复刻 IDE 生成目录里 makefile 的命令行（TASKING TriCore v6.3r1）。

IDE 生成命令（参照 <build 目录>/subdir.mk 与 makefile）：

    编译  cctc -cs --dep-file=.. --misrac-version=2012 -D__CPU__=tc27xd -I..
              --iso=99 --c++14 --language=+volatile --exceptions --anachronisms
              --fp-model=3 -O0 --tradeoff=4 --compact-max-size=200 -g
              -Wc-w544 -Wc-w557 -Ctc27xd -Y0 -N0 -Z0 -o x.src x.c
    汇编  astc -Og -Os --no-warnings= --error-limit=42 -o x.o x.src
    链接  cctc --lsl-file=<lsl> -Wl-Oc -Wl-OL -Wl-Ot -Wl-Ox -Wl-Oy
              -Wl--map-file=x.map -Wl-mc -Wl-mf -Wl-mi -Wl-mk -Wl-ml -Wl-mm
              -Wl-md -Wl-mr -Wl-mu --no-warnings= -Wl--error-limit=42
              --fp-model=3 -lrt --lsl-core=vtc --exceptions --strict
              --anachronisms --force-c++ -Ctc27xd -ox.elf -Wl-ox.hex:IHEX objs

SCons 采用 cctc 单步编译（-c 直出 .o，省掉中间 .src；生成代码与两步法一致）。
"""

import fnmatch
import glob
import os
import re
import xml.etree.ElementTree as ET

import SCons.Action

# 期望的工具链版本（主.次），匹配 TriCore v6.3* 取最新
TASKING_VERSION = 'v6.3'

DEFAULT_INSTALL_ROOTS = [
    r'C:\Program Files\TASKING',
    r'C:\Program Files (x86)\TASKING',
    '/opt/tasking',
    '/usr/opt/tasking',
]

# 源码发现时按名跳过的项目根级目录：IDE 生成目录 / 版本库内部目录 / 本构建输出。
# 注意只在项目根这一层生效，Configurations/Debug 这类同名子目录不受影响。
PRUNE_ROOT = {
    '.git', '.settings', '.zcode', 'build', 'doc',
    'Debug', 'Release',
    'TriCore Debug (TASKING)', 'TriCore Release (TASKING)',
    'TriCore Debug (GCC)', 'TriCore Release (GCC)',
}
PRUNE_ANY = {'.git', '.zcode'}

# cctc 公共编译参数（除 -O 与 -C<cpu> 外全部固定，与 IDE 托管默认值一致）
BASE_CCFLAGS = [
    '--misrac-version=2012',
    '--iso=99',
    '--c++14',
    '--language=+volatile',
    '--exceptions',
    '--anachronisms',
    '--fp-model=3',
    '--tradeoff=4',
    '--compact-max-size=200',
    '-g',
    '-Wc-w544',
    '-Wc-w557',
    '-Y0',
    '-N0',
    '-Z0',
]


def _write_if_changed(path, content):
    """内容没变就不落盘，避免每次 scons 都改动响应文件触发无谓重建。"""
    try:
        with open(path, 'r') as f:
            if f.read() == content:
                return
    except IOError:
        pass
    d = os.path.dirname(path)
    if d and not os.path.isdir(d):
        os.makedirs(d)
    with open(path, 'w') as f:
        f.write(content)


# --------------------------------------------------------------------------- #
# 版本号（产物命名用）
# --------------------------------------------------------------------------- #

def header_version(header, macro='APP_VERSION_STRING'):
    """读取版本头中的 #define <macro> "x.y.z"，供产物名携带版本号。"""
    with open(header, encoding='utf-8') as f:
        m = re.search(r'^#define\s+%s\s+"([^"]+)"' % macro, f.read(), re.M)
    if not m:
        raise Exception('%s 中未找到 #define %s "x.y.z"' % (header, macro))
    return m.group(1)


# --------------------------------------------------------------------------- #
# 工具链定位
# --------------------------------------------------------------------------- #

def find_tasking(version=TASKING_VERSION):
    """定位 TASKING TriCore 安装，返回其 ctc/bin 目录；找不到返回 None。

    查找顺序：TASKING_TRICORE_HOME / TASKING_HOME 环境变量（可直接指向
    <TriCore vX.YrZ> 或其上级），然后常见安装根目录下的 TriCore <version>*。
    """
    env_dir = os.environ.get('TASKING_TRICORE_HOME') or os.environ.get('TASKING_HOME')
    roots = ([env_dir] if env_dir else []) + DEFAULT_INSTALL_ROOTS
    for root in roots:
        if not os.path.isdir(root):
            continue
        subs = sorted(glob.glob(os.path.join(root, 'TriCore %s*' % version)),
                      reverse=True)
        for sub in subs + [root]:
            if any(os.path.isfile(os.path.join(sub, 'ctc', 'bin', 'cctc' + ext))
                   for ext in ('.exe', '')):
                return os.path.join(sub, 'ctc', 'bin')
    return None


# --------------------------------------------------------------------------- #
# .cproject 解析
# --------------------------------------------------------------------------- #

def _include_rel(value):
    """${workspace_loc:/<proj>/x}、${ProjDirPath}/x、带引号 -> 项目相对路径 x。"""
    p = value.strip().strip('"').replace('\\', '/')
    if p.startswith('${ProjDirPath}/'):
        p = p[len('${ProjDirPath}/'):]
    elif p.startswith('${workspace_loc:/'):
        rest = p[len('${workspace_loc:/'):]
        p = rest.split('/', 1)[1] if '/' in rest else ''
        if p.endswith('}'):          # 吃掉 ${workspace_loc:...} 的收尾大括号
            p = p[:-1]
    # "${workspace_loc:/${ProjName}/}"（项目根本身）会解析成空串，一并丢弃
    return '' if p in ('', '}') else p.rstrip('/')


def load_cproject(project_dir, cfg_name):
    """从 .cproject 提取指定 TASKING 配置：cpu / includes / defines / excludes。"""
    tree = ET.parse(os.path.join(project_dir, '.cproject'))
    for ccfg in tree.iter('cconfiguration'):
        cfg = ccfg.find('.//storageModule[@moduleId="cdtBuildSystem"]/configuration')
        if cfg is None or cfg.get('name') != cfg_name:
            continue
        out = {'cpu': 'tc27xd', 'includes': [], 'defines': [], 'excludes': []}
        cpu = ccfg.find('.//*[@superClass="com.tasking.ctc.cpu"]')
        if cpu is not None:
            out['cpu'] = cpu.get('value')
        for opt in ccfg.iter('option'):
            sup = opt.get('superClass', '')
            vals = [v.get('value', '') for v in opt.iter('listOptionValue')]
            if sup.endswith('c.compiler.tasking.include'):
                out['includes'] += [_include_rel(v) for v in vals]
            elif sup.endswith('tasking.preprocessor.definedSymbols'):
                out['defines'] += vals
        entry = ccfg.find('.//sourceEntries/entry')
        if entry is not None:
            out['excludes'] = [e for e in entry.get('excluding', '').split('|') if e]
        out['includes'] = [i for i in out['includes'] if i]
        return out
    raise Exception('.cproject 中找不到配置: %s' % cfg_name)


# --------------------------------------------------------------------------- #
# 源码发现（与 Eclipse 构建的源集一致）
# --------------------------------------------------------------------------- #

def _excluded(rel_posix, excludes):
    """CDT sourceEntries 的排除语义：目录前缀整棵排除，支持 fnmatch 通配。"""
    for e in excludes:
        if rel_posix == e or rel_posix.startswith(e + '/') \
                or fnmatch.fnmatch(rel_posix, e):
            return True
    return False


def discover_sources(project_dir, excludes):
    """递归收集参与编译的 .c 文件（项目相对 posix 路径），按排除列表过滤。"""
    out = []
    for dirpath, dirnames, filenames in os.walk(project_dir):
        rel = os.path.relpath(dirpath, project_dir).replace(os.sep, '/')
        if rel == '.':
            dirnames[:] = [d for d in dirnames if d not in PRUNE_ROOT]
        dirnames[:] = [d for d in dirnames
                       if d not in PRUNE_ANY
                       and not _excluded(d if rel == '.' else rel + '/' + d, excludes)]
        for f in filenames:
            if not f.lower().endswith('.c'):
                continue
            fre = f if rel == '.' else rel + '/' + f
            if not _excluded(fre, excludes):
                out.append(fre)
    return sorted(out)


# --------------------------------------------------------------------------- #
# SCons Tool：把 cctc 注册为 CC/LINK
# --------------------------------------------------------------------------- #

def generate(env):
    bindir = find_tasking()
    if bindir is None:
        raise Exception(
            '找不到 TASKING TriCore %s 工具链；请安装或设置环境变量 '
            'TASKING_TRICORE_HOME 指向安装目录（如 '
            r'C:\Program Files\TASKING\TriCore v6.3r1）' % TASKING_VERSION)
    env['ENV'] = dict(os.environ)
    env['ENV']['PATH'] = bindir + os.pathsep + env['ENV'].get('PATH', '')
    env['TASKING_BIN'] = bindir
    # 路径含空格（C:\Program Files\...），必须用单元素列表让 SCons 整体加引号
    cctc = [os.path.join(bindir, 'cctc')]
    env['CC'] = cctc
    env['LINK'] = cctc
    env['SIZE'] = [os.path.join(bindir, 'elfsize')]
    env['OBJSUFFIX'] = '.o'
    env['PROGSUFFIX'] = '.elf'
    env['CCCOM'] = ('$CC -c $CFLAGS $CCFLAGS '
                    '$_CPPDEFFLAGS $_CPPINCFLAGS -o $TARGET $SOURCE')
    env['CCCOMSTR'] = 'cc  $SOURCE'
    # 对象列表走 -f 响应文件（见 build()），命令行只留编译选项，避开 Windows 长度限制
    env['LINKCOM'] = '$LINK $LINKFLAGS -o $TARGET'
    env['LINKCOMSTR'] = 'ld  $TARGET'


def exists(env):
    return find_tasking() is not None


# --------------------------------------------------------------------------- #
# 组装入口：SConstruct 调用
# --------------------------------------------------------------------------- #

def build(env, *, cfg_name, artifact, lsl, variant, opt='-O0'):
    """按 .cproject 的 cfg_name 配置构建，产物落在 build/<variant>/ 下。

    返回 elf 节点；.hex/.map 由链接器一并产出（-Wl-o..:IHEX / --map-file）。
    """
    cfg = load_cproject('.', cfg_name)
    sources = discover_sources('.', cfg['excludes'])
    if not sources:
        raise Exception('未发现任何源文件（配置 %s）' % cfg_name)

    outdir = os.path.join('build', variant)

    # cctc 内部两步法（compile -> ccXXXXa.src -> assemble）的临时响应文件
    # （cc<pid>x，fopen "w+"）优先写 TMPDIR；未设时落在 cwd = 工程根，且进程
    # 被中断（Ctrl+C、-j8 全量重编被杀）时不回收——工程根就攒下 ccXXXXXX
    # 垃圾（v6.3r1 实测：kill 三次漏三个）。显式指进 build/<variant>/tmp，
    # 泄漏物随 scons -c 一起清；目录必须先建好，缺目录 cctc F101 直接拒编。
    tmpdir = os.path.abspath(os.path.join(outdir, 'tmp'))
    os.makedirs(tmpdir, exist_ok=True)
    env['ENV']['TMPDIR'] = tmpdir
    env.Replace(
        # 项目根本身也在 -I 列表里（Eclipse 传绝对路径；#include "mw/proto/x.h" 依赖它）
        CPPPATH=['.'] + cfg['includes'],
        CPPDEFINES=cfg['defines'],
        CFLAGS=[opt],
        CCFLAGS=BASE_CCFLAGS + ['-C' + cfg['cpu']],
        LINKFLAGS=[
            '--lsl-file=' + lsl,
            '-Wl-Oc', '-Wl-OL', '-Wl-Ot', '-Wl-Ox', '-Wl-Oy',
            '-Wl--map-file=%s' % os.path.join(outdir, artifact + '.map'),
            '-Wl-mc', '-Wl-mf', '-Wl-mi', '-Wl-mk',
            '-Wl-ml', '-Wl-mm', '-Wl-md', '-Wl-mr', '-Wl-mu',
            '--no-warnings=',
            '-Wl--error-limit=42',
            '--fp-model=3',
            '-lrt',
            '--lsl-core=vtc',
            '--exceptions',
            '--strict',
            '--anachronisms',
            '--force-c++',
            '-C' + cfg['cpu'],
            '-Wl-o%s:IHEX' % os.path.join(outdir, artifact + '.hex'),
        ],
    )

    # mw/crypto/ 是 esp32c6_car components/c6_ota 的逐字拷贝（doc 24：两侧共用真源，
    # 不许漂移），而它生成的 c6_consts.h 把 SHA-512 与 ed25519 两组常量塞在同一个头
    # 文件里；ed25519v.c 只用后者，ctc 就报 W537 unused variable（gcc/clang 默认不报，
    # 所以上游一直没暴露）。这些未用数组是 per-object 的 .rodata 段，链接期即被丢弃
    # （.map 里只挂在 sha512.o 下），不进镜像。所以既不改被拷贝的源文件，也不全局关
    # 这个告警号——只对该目录关，本工程自有代码的死变量检测照旧生效。
    objs = []
    for src in sources:
        rel = src.replace(os.sep, '/')
        build_env = env
        extra = []
        if rel.startswith('mw/crypto/'):
            extra.append('-Wc-w537')
        # Libraries/ST/vl53l5cx/ 是 ST Ultra Lite Driver 的逐字拷贝（doc 36 第 2 节），
        # 其中 vl53l5cx_api.h:366 的 union Block_header 用匿名 struct 做联合体成员——
        # 那是 C11 写法，本工程编译标准锁在 --iso=99，ctc 因此报 W586 unnamed
        # struct/union field。两条正路都不走：改上游文件=制造漂移；把全工程升到 C11
        # =为一个外部头文件的声明动整个镜像的语言级别。该声明只被 api.c 使用，但
        # bsp/tof.c 只要 include 它就会同样报号，所以关号范围是"含这个头的编译单元"，
        # 本工程其余代码的匿名成员检测照旧生效。
        if rel.startswith('Libraries/ST/') or rel == 'bsp/tof.c':
            extra.append('-Wc-w586')
        if extra:
            build_env = env.Clone()
            build_env.Append(CCFLAGS=extra)
        objs.append(build_env.Object(
            os.path.join(outdir, 'obj', src[:-2] + '.o'), src))

    # Windows 命令行长度受限（IDE 同样用 argfile）：对象列表写入 TASKING 响应文件
    rsp = os.path.join(outdir, artifact + '.rsp')
    _write_if_changed(rsp,
                      '\n'.join(str(o[0]).replace(os.sep, '/') for o in objs) + '\n')
    env.Append(LINKFLAGS=['-f', rsp])

    elf = env.Program(os.path.join(outdir, artifact + '.elf'), objs)
    env.SideEffect(os.path.join(outdir, artifact + '.hex'), elf)
    env.SideEffect(os.path.join(outdir, artifact + '.map'), elf)
    env.Clean(elf, [os.path.join(outdir, artifact + ext)
                    for ext in ('.hex', '.map', '.rsp')] +
              [os.path.join(outdir, 'obj'), tmpdir])

    # .cproject / 构建脚本一变就全量重编（内容签名判断，不改不会触发）
    recipe = ['.cproject', 'SConstruct', __file__]
    env.Depends(elf, recipe)
    for o in objs:
        env.Depends(o, recipe)

    env.AddPostAction(elf, SCons.Action.Action('$SIZE $TARGET',
                                               'size $TARGET'))
    elf_path = os.path.join(outdir, artifact + '.elf')
    size_alias = env.Alias('size', elf, SCons.Action.Action(
        '$SIZE %s' % elf_path, 'size %s' % elf_path))
    env.AlwaysBuild(size_alias)

    print('== aurix_tasking: %s | %s | %d sources | %s | %s' % (
        artifact, cfg_name, len(sources), opt, cfg['cpu']))
    return elf
