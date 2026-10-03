#!/usr/bin/env python3
"""生成 tc275_car / tc275_sbl 的 clangd 编译数据库（build/compile_commands.json）。

TASKING 工具链只有 Windows 版，Mac/Linux 上没有 SCons + cctc 可跑，clangd 拿不到
编译参数。本脚本与 site_scons/aurix_tasking.py 同源解析 .cproject（Debug 配置的
include / 宏 / 源码排除列表），改用宿主 clang 作为驱动生成 CDB——只求跳转/补全/
静态浏览可用，不用于真正编译（宿主 clang 也不认识 TriCore）。

用法: python3 scripts/gen-tc275-cdb.py [tc275_car] [tc275_sbl]
      不带参数则两个工程都生成。
"""

import fnmatch
import json
import os
import sys
import xml.etree.ElementTree as ET

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# 与 site_scons/aurix_tasking.py 保持一致
PRUNE_ROOT = {
    '.git', '.settings', '.zcode', 'build', 'doc',
    'Debug', 'Release',
    'TriCore Debug (TASKING)', 'TriCore Release (TASKING)',
    'TriCore Debug (GCC)', 'TriCore Release (GCC)',
}
PRUNE_ANY = {'.git', '.zcode', '__pycache__'}

# 宿主 clang 额外参数：
#   -D__HIGHTEC__  —— iLLD 编译器抽象层（Compilers.h/Ifx_Types.h）按编译器宏分
#                     支，TASKING 语法宿主 clang 解析不了，GCC 风格的 __HIGHTEC__
#                     分支最兼容；缺失的 HighTec 专有头文件由 clangd-shim 兜底
#   -D__interrupt/__vector_table 空 —— TASKING 关键字，port.c 等直接写在函数
#                     签名里；iLLD 的 Ifx_Types*.h 里也有同名空宏，需一并关掉
#                     重定义告警
#   -Wno-implicit-function-declaration —— 少数裸内建（__isync 等）无声明，
#                     真实 TASKING 编译器里是内建符号；降级为不诊断
EXTRA_ARGS = ['-x', 'c', '-std=gnu99', '-fsyntax-only',
              '--target=x86_64-apple-macos',
              '-D__HIGHTEC__',
              '-D__interrupt(x)=', '-D__vector_table(x)=', '-Wno-macro-redefined',
              '-Wno-implicit-function-declaration',
              '-I' + os.path.join(REPO, 'scripts', 'clangd-shim', 'tc275')]


def include_rel(value):
    """${workspace_loc:/<proj>/x}、${ProjDirPath}/x、带引号 -> 项目相对路径 x。"""
    p = value.strip().strip('"').replace('\\', '/')
    if p.startswith('${ProjDirPath}/'):
        p = p[len('${ProjDirPath}/'):]
    elif p.startswith('${workspace_loc:/'):
        rest = p[len('${workspace_loc:/'):]
        p = rest.split('/', 1)[1] if '/' in rest else ''
        if p.endswith('}'):
            p = p[:-1]
    return '' if p in ('', '}') else p.rstrip('/')


def load_cproject(project_dir):
    """取 .cproject 第一个配置（两工程均为 Debug）：includes / defines / excludes。"""
    tree = ET.parse(os.path.join(project_dir, '.cproject'))
    for ccfg in tree.iter('cconfiguration'):
        cfg = ccfg.find('.//storageModule[@moduleId="cdtBuildSystem"]/configuration')
        if cfg is None:
            continue
        includes, defines = [], []
        for opt in ccfg.iter('option'):
            sup = opt.get('superClass', '')
            vals = [v.get('value', '') for v in opt.iter('listOptionValue')]
            if sup.endswith('c.compiler.tasking.include'):
                includes += [include_rel(v) for v in vals]
            elif sup.endswith('tasking.preprocessor.definedSymbols'):
                defines += vals
        entry = ccfg.find('.//sourceEntries/entry')
        excludes = []
        if entry is not None:
            excludes = [e for e in entry.get('excluding', '').split('|') if e]
        return {
            'includes': [i for i in includes if i],
            'defines': [d for d in defines if d],
            'excludes': excludes,
        }
    raise SystemExit('%s/.cproject 中没有配置' % project_dir)


def excluded(rel_posix, excludes):
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
                       and not excluded(d if rel == '.' else rel + '/' + d, excludes)]
        for f in filenames:
            if not f.lower().endswith('.c'):
                continue
            fre = f if rel == '.' else rel + '/' + f
            if not excluded(fre, excludes):
                out.append(fre)
    return sorted(out)


def gen(project):
    project_dir = os.path.join(REPO, project)
    if not os.path.isdir(project_dir):
        raise SystemExit('工程不存在: %s' % project_dir)
    cfg = load_cproject(project_dir)
    sources = discover_sources(project_dir, cfg['excludes'])
    if not sources:
        raise SystemExit('%s: 没有发现源文件' % project)

    flags = ['-D__CPU__=%s' % 'tc27xd']
    flags += ['-D' + d for d in cfg['defines']]
    flags += ['-I.'] + ['-I' + i for i in cfg['includes']]

    db = [{
        'directory': project_dir,
        'file': os.path.join(project_dir, src),
        'arguments': ['clang'] + EXTRA_ARGS + flags + [src],
    } for src in sources]

    build = os.path.join(project_dir, 'build')
    os.makedirs(build, exist_ok=True)
    out = os.path.join(build, 'compile_commands.json')
    with open(out, 'w', encoding='utf-8') as f:
        json.dump(db, f, ensure_ascii=False, indent=1)
        f.write('\n')
    print('%s: %d 个源文件 -> %s' % (project, len(db), os.path.relpath(out, REPO)))


if __name__ == '__main__':
    projects = sys.argv[1:] or ['tc275_car', 'tc275_sbl']
    for p in projects:
        gen(p)
