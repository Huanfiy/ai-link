import os
import sys

# rtconfig owns the environment contract; importing it validates host, toolchain and RTT_ROOT.
import rtconfig

rtconfig.logi(f'{rtconfig.BUILD_MODE} build: {rtconfig.OUTPUT_DIR}/{rtconfig.PROJECT_NAME}.{rtconfig.TARGET_EXT}')

# ---- RT-Thread build framework ----
BSP_ROOT = os.getcwd()
RTT_ROOT = rtconfig.RTT_ROOT

sys.path.insert(0, os.path.join(RTT_ROOT, 'tools'))
from building import *

# ---- GNU build environment ----
DefaultEnvironment(tools=[])
env = Environment(tools=['gcc', 'g++', 'gnulink', 'ar', 'gas'],
                  AS   = rtconfig.AS,   ASFLAGS   = rtconfig.AFLAGS,
                  CC   = rtconfig.CC,   CFLAGS    = rtconfig.CFLAGS,
                  CXX  = rtconfig.CXX,  CXXFLAGS  = rtconfig.CXXFLAGS,
                  AR   = rtconfig.AR,   ARFLAGS   = '-rc',
                  LINK = rtconfig.LINK, LINKFLAGS = rtconfig.LFLAGS)
env.PrependENVPath('PATH', rtconfig.EXEC_PATH)
Export('env', 'RTT_ROOT', 'rtconfig')

# ---- BSP sources & build ----
def check_bsp_packages():
    # runs from DoBuilding, not at read time: `scons --menuconfig` stays usable before `pkgs --update`
    for package in ('CMSIS-Core-latest', 'stm32f4_cmsis_driver-latest', 'stm32f4_hal_driver-latest'):
        if not os.path.isdir(os.path.join(BSP_ROOT, 'packages', package)):
            rtconfig.fail(f'missing BSP package {package}', 'run `pkgs --update` in the BSP root (RT-Thread Env)')

RegisterPreBuildingAction(check_bsp_packages)

objects = PrepareBuilding(env, RTT_ROOT, has_libcpu=False)

TARGET = os.path.join(rtconfig.OUTPUT_DIR, f'{rtconfig.PROJECT_NAME}.{rtconfig.TARGET_EXT}')
DoBuilding(TARGET, objects)
