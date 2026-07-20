import os
import sys

def logi(msg):
    print(f'[info] {msg}')

def fail(msg, hint=None):
    print(f'[error] {msg}', file=sys.stderr)
    if hint:
        print(f'[hint] {hint}', file=sys.stderr)
    sys.exit(1)

# Environment contract: GNU Arm GCC on Linux; every RTT_* env var is resolved and
# validated here. building.py reloads this module, so keep everything idempotent.
if not sys.platform.startswith('linux'):
    fail('this BSP supports Linux hosts only')
if os.getenv('RTT_CC', 'gcc') != 'gcc':
    fail(f'unsupported RTT_CC={os.getenv("RTT_CC")}, this BSP builds with gcc only', 'unset RTT_CC or set RTT_CC=gcc')

# RT-Thread kernel root: prefer a BSP-local rt-thread/ checkout, else $RTT_ROOT.
RTT_ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'rt-thread')
if not os.path.isdir(RTT_ROOT):
    RTT_ROOT = os.getenv('RTT_ROOT')
if not RTT_ROOT:
    fail('RTT_ROOT is not set and no BSP-local rt-thread/ exists', 'set RTT_ROOT to the RT-Thread source root')
RTT_ROOT = os.path.abspath(os.path.expanduser(RTT_ROOT))
if not os.path.isfile(os.path.join(RTT_ROOT, 'tools/building.py')):
    fail(f'{RTT_ROOT} is not an RT-Thread source tree', 'set RTT_ROOT to the RT-Thread source root')

# Firmware base name for elf/bin/map, overridable by the caller (e.g. run.sh).
PROJECT_NAME = os.getenv('PROJECT_NAME', 'rtthread')

ARCH             = 'arm'
CPU              = 'cortex-m4'
CROSS_TOOL       = 'gcc'
PLATFORM         = 'gcc'
BSP_LIBRARY_TYPE = None

PREFIX     = 'arm-none-eabi-'
CC         = PREFIX + 'gcc'
CXX        = PREFIX + 'g++'
AS         = PREFIX + 'gcc'
AR         = PREFIX + 'ar'
LINK       = PREFIX + 'gcc'
SIZE       = PREFIX + 'size'
OBJDUMP    = PREFIX + 'objdump'
OBJCPY     = PREFIX + 'objcopy'
TARGET_EXT = 'elf'

_exec = os.getenv('RTT_EXEC_PATH')
EXEC_PATH = os.path.abspath(os.path.expanduser(_exec)) if _exec else ''
if not (EXEC_PATH and os.access(os.path.join(EXEC_PATH, CC), os.X_OK)):
    fail(f'{CC} not found in RTT_EXEC_PATH', 'set RTT_EXEC_PATH to the GNU Arm toolchain bin directory')

OUTPUT_DIR = 'build'
LDSCRIPT   = 'board/linker_scripts/link.lds'

DEVICE = ' '.join([
    '-mthumb',
    '-mcpu=cortex-m4',
    '-mfloat-abi=hard',
    '-mfpu=fpv4-sp-d16',
])

CFLAGS = DEVICE + ' ' + ' '.join([
    '-fmessage-length=0 -fsigned-char',
    '-ffunction-sections -fdata-sections',
    '-MMD -MP',
    '-w',
])
AFLAGS = DEVICE + ' -Wa,-mimplicit-it=thumb'
LFLAGS = DEVICE + ' ' + ' '.join([
    f'-T{LDSCRIPT}',
    f'-Wl,-Map={OUTPUT_DIR}/{PROJECT_NAME}.map',
    '-Wl,--gc-sections',
    '-Wl,-cref',
    '-Wl,-u,Reset_Handler',
    '--specs=nano.specs --specs=nosys.specs',
])

BUILD_MODE = os.getenv('BUILD_MODE', 'Release')
if BUILD_MODE == 'Debug':
    CFLAGS += ' -O0 -gdwarf-2 -g'
    AFLAGS += ' -gdwarf-2'
else:
    CFLAGS += ' -Os -DNDEBUG'

CXXFLAGS = CFLAGS
CPATH    = ''
LPATH    = ''

POST_ACTION = (
    f'{OBJCPY} -O binary $TARGET {OUTPUT_DIR}/{PROJECT_NAME}.bin\n'
    f'python3 tools/build/fwinfo.py patch --elf $TARGET {OUTPUT_DIR}/{PROJECT_NAME}.bin\n'
    f'python3 tools/build/report_firmware_info.py --mode={BUILD_MODE} --ldscript={LDSCRIPT} $TARGET\n'
)
