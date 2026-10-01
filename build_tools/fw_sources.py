"""Firmware build inputs derived from the IDE project files.

SCons and AURIX Development Studio must not hold two copies of the source
filter, include paths or defines: .cproject is the single source of truth, so
everything here reads it. The generated `TriCore */` makefiles are not usable
inputs (gitignored, machine-absolute paths, "do not edit").
"""

import os
import re
import glob
import xml.etree.ElementTree as ET

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CPROJECT = os.path.join(ROOT, '.cproject')

# IDE build-output dirs (gitignored) and non-firmware trees that never compile.
PRUNE_DIRS = ('.git', '.ads', '.codegraph', '.settings', '.github', 'doc', 'build')

# __CPU__ is the iLLD device selector; the bare-name defines are config data.
CONFIG_MACRO_RE = re.compile(r'^[A-Za-z_][A-Za-z0-9_]*(?:=[^\s"|<>]+)?$')

TOOLCHAIN_GLOBS = (
    'C:/infineon/AURIX-Studio-*/tools/Compilers/tricore-gcc11',
    'C:/Program Files/Infineon/AURIX-Studio-*/tools/Compilers/tricore-gcc11',
    'C:/Program Files (x86)/Infineon/AURIX-Studio-*/tools/Compilers/tricore-gcc11',
)


def _text(value):
    """Resolve the CDT workspace macros we actually use to repo-relative paths."""
    value = value.strip().strip('"')
    for prefix in ('${workspace_loc:/${ProjName}', '${ProjName}'):
        if value.startswith(prefix):
            value = '.' + value[len(prefix):]
    value = value.rstrip('}')
    return value.replace('\\', '/').rstrip('/') or '.'


def _option_lists(tool, suffix):
    for opt in tool.iter('option'):
        if (opt.get('superClass') or '').endswith(suffix):
            return [v.get('value') for v in opt.iter('listOptionValue')]
    return []


def read_config(name, cproject=None):
    """Return {'excludes', 'includes', 'defines', 'lsl'} for one IDE configuration."""
    path = cproject or CPROJECT
    if not os.path.isfile(path):
        raise IOError('missing %s (firmware inputs come from the IDE project files)' % path)
    for cfg in ET.parse(path).getroot().iter('cconfiguration'):
        for storage in cfg.findall('storageModule'):
            if storage.get('moduleId') != 'cdtBuildSystem':
                continue
            conf = next(storage.iter('configuration'), None)
            if conf is None or conf.get('name') != name:
                continue
            entry = next(storage.iter('entry'), None)
            if entry is None:
                raise ValueError('%s has no sourceEntries in .cproject' % name)
            excludes = set(x for x in (entry.get('excluding') or '').split('|') if x)
            includes, defines = [], []
            for tool in storage.iter('tool'):
                if not (tool.get('name') or '').endswith('GCC Compiler'):
                    continue
                includes = [_text(v) for v in _option_lists(tool, 'compiler.option.include.paths')]
                defines = [v for v in _option_lists(tool, 'compiler.option.preprocessor.def.symbols') if v]
            return {
                'excludes': excludes,
                'includes': includes,
                'defines': defines,
                'lsl': find_lsl(name),
            }
    raise ValueError('configuration %r not found in .cproject' % name)


def find_lsl(name):
    """The LSL the IDE links with: GNU linker script for GCC, Tasking for TASKING."""
    prefer = 'Lcf_Gnuc_Tricore_Tc.lsl' if 'GCC' in name else 'Lcf_Tasking_Tricore_Tc.lsl'
    path = os.path.join(ROOT, prefer)
    if not os.path.isfile(path):
        raise IOError('missing linker script %s' % path)
    return path


def validate_defines(config):
    problems = []
    if '__CPU__' not in ' '.join(config['defines']):
        problems.append('no __CPU__ define -> iLLD cannot select the TC27D SFR set')
    for define in config['defines']:
        if not CONFIG_MACRO_RE.match(define):
            problems.append('define %r has shell/XML-hostile characters' % define)
    return problems


def walk_sources(excludes):
    """Repo-relative sources participating in the build, minus the IDE exclusion list.

    test/ is excluded unconditionally: host unit tests define main() and cannot
    join a firmware image whatever .cproject says.
    """
    skipped = set(excludes) | {'test'}
    out = []
    for dir_path, dir_names, file_names in os.walk(ROOT):
        rel = os.path.relpath(dir_path, ROOT).replace(os.sep, '/')
        rel = '' if rel == '.' else rel
        dir_names[:] = sorted(d for d in dir_names
                              if d not in PRUNE_DIRS and not d.startswith('TriCore ')
                              and not _excluded('/'.join(p for p in (rel, d) if p), skipped))
        for f in sorted(file_names):
            if not f.endswith(('.c', '.s', '.S')):
                continue
            path = f if rel == '' else '%s/%s' % (rel, f)
            if _excluded(path, skipped):
                continue
            out.append(path)
    return out


def _excluded(path, excludes):
    parts = path.split('/')
    for i in range(len(parts)):
        if '/'.join(parts[:i + 1]) in excludes:
            return True
    return False


def toolchain_dir(override=None):
    """Absolute bin dir of the AURIX GCC toolchain, or None when it cannot be found.

    An explicit override is taken literally: pointing at the wrong dir must fail
    loudly rather than silently building with a different compiler.
    """
    if override:
        candidates = [override]
    else:
        candidates = [os.environ[v] for v in ('AURIX_GCC_HOME', 'TRICORE_GCC_HOME') if os.environ.get(v)]
        candidates += [os.path.dirname(p) for p in _which_all('tricore-elf-gcc')]
        candidates += TOOLCHAIN_GLOBS
    for pattern in candidates:
        for root in sorted(glob.glob(pattern), reverse=True) or [pattern]:
            for cand in (os.path.join(root, 'bin'), root):
                if os.path.isfile(os.path.join(cand, 'tricore-elf-gcc.exe')) \
                        or os.path.isfile(os.path.join(cand, 'tricore-elf-gcc')):
                    return os.path.abspath(cand)
    return None


def _which_all(exe):
    found = []
    for entry in os.environ.get('PATH', '').split(os.pathsep):
        if not entry:
            continue
        for name in (exe + '.exe', exe):
            path = os.path.join(entry, name)
            if os.path.isfile(path):
                found.append(path)
    return found
