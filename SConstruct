"""Command-line GCC firmware build: run `python -m SCons` in the repo root.

    python -m SCons -j8                          # TriCore Debug (GCC) mirror -> build/gcc/debug/myCar
    python -m SCons -j8 mode=release             # TriCore Release (GCC) mirror
    python -m SCons --clean                      # delete the built targets
    python -m SCons mode=release toolchain=C:/path/to/tricore-gcc11

Which sources compile, which include paths they see and which macros they get
come from .cproject, so the IDE and this build cannot drift apart - see
build_tools/fw_sources.py. The TASKING configurations are not buildable here
(its license only runs inside AURIX Development Studio, CLI exits F104).

Outputs myCar (ELF), myCar.hex and myCar.siz under build/gcc/<mode>/.
Flashing stays in the IDE: the .launch files point at the TASKING image.
"""

import os
import sys

from SCons.Errors import UserError

sys.path.insert(0, os.path.join(Dir('.').abspath, 'build_tools'))
import fw_sources  # noqa: E402

MODES = {
    'debug': ('TriCore Debug (GCC)', ['-Og', '-g3', '-gdwarf-3']),
    'release': ('TriCore Release (GCC)', ['-O2']),
}

mode = ARGUMENTS.get('mode', 'debug')
if mode not in MODES:
    raise UserError('mode must be one of %s, got %r' % ('/'.join(sorted(MODES)), mode))
ide_config, mode_flags = MODES[mode]

try:
    config = fw_sources.read_config(ide_config)
except (IOError, ValueError) as exc:
    raise UserError(str(exc))
problems = fw_sources.validate_defines(config)
if not config['includes']:
    problems.append('%s has no include paths in .cproject' % ide_config)
if problems:
    raise UserError('%s is not buildable from .cproject:\n  - %s'
                    % (ide_config, '\n  - '.join(problems)))

toolchain = fw_sources.toolchain_dir(ARGUMENTS.get('toolchain') or None)
if not toolchain:
    raise UserError('tricore-elf-gcc not found. Pass toolchain=<tricore-gcc11 dir>, '
                    'put its bin on PATH, or set AURIX_GCC_HOME.')

sources = fw_sources.walk_sources(config['excludes'])
build_dir = os.path.join('build', 'gcc', mode)

env = Environment(
    # 'cc' + 'link' give the Object/Program builders and $SMARTLINK (which folds
    # an over-long command line into a temp script) without probing a host compiler.
    tools=['cc', 'link'],
    PROGSUFFIX='', OBJSUFFIX='.o',
    CC=os.path.join(toolchain, 'tricore-elf-gcc'),
    CPPPATH=['.'] + [p for p in config['includes'] if p != '.'],
    CPPDEFINES=sorted(config['defines']),
    CCFLAGS=['-std=c99', '-mtc161', '-Wall', '-fmessage-length=0', '-fno-common',
             '-fstrict-volatile-bitfields', '-fdata-sections', '-ffunction-sections'] + mode_flags,
    LINKFLAGS=['-mtc161', '-nocrt0', '-Wl,--gc-sections', '-T' + config['lsl']],
)
env.AppendENVPath('PATH', toolchain)

objects = [env.Object(os.path.join(build_dir, os.path.splitext(source)[0] + '.o'), source)
           for source in sources]
elf = env.Program(os.path.join(build_dir, 'myCar'), objects)
env.Depends(elf, env.File(config['lsl']))
hex_file = env.Command(os.path.join(build_dir, 'myCar.hex'), elf,
                       'tricore-elf-objcopy -O ihex $SOURCE $TARGET')
size_report = env.Command(os.path.join(build_dir, 'myCar.siz'), elf,
                         'tricore-elf-size --format=berkeley $SOURCE > $TARGET')

Default([elf, hex_file, size_report])
print('scons: %s, %d sources, %s' % (ide_config, len(sources), toolchain))
