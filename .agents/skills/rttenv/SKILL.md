---
name: rttenv
description: Operate and diagnose the RT-Thread Env v2 toolchain for the ailink BSP on this Linux/zsh workstation. Use this skill only when the user explicitly asks Codex to use the rttenv skill (including `$rttenv`); never auto-trigger it merely because a task involves RT-Thread Env, Kconfig/menuconfig, package synchronization, SDK or compiler discovery, or SCons builds.
---

# RT-Thread Env

## Environment

- BSP: `/home/huan/workspace/ailink`
- Env: `/home/huan/.env`
- Python venv: `/home/huan/.venvs/rtt-env`
- RT-Thread SDK: `/home/huan/SDK/rt-thread`
- Arm toolchain: `/home/huan/toolchain/arm-eabi-toolchain/bin`

Read versions from the installed tools; do not cache them here.

## Activate

Use `get_rtenv` in interactive zsh. In automation, activate explicitly:

```bash
. /home/huan/.venvs/rtt-env/bin/activate
. /home/huan/.env/env.sh
export RTT_ROOT=/home/huan/SDK/rt-thread
export RTT_EXEC_PATH=/home/huan/toolchain/arm-eabi-toolchain/bin
export PATH=$RTT_EXEC_PATH:$PATH
```

Always activate the venv: Env wrappers call `python3`, and `kconfiglib` is not installed in the system Python.

## Workflow

```bash
cd /home/huan/workspace/ailink
menuconfig              # Edit .config and regenerate rtconfig.h
menuconfig --silent     # Normalize .config non-interactively
menuconfig --generate   # Regenerate only rtconfig.h
menuconfig -s           # Edit Env's own settings; -s means --setting
pkgs --upgrade          # Update the package index
pkgs --update           # Synchronize BSP-selected packages
sdk --help              # Inspect SDK commands
scons -j4               # Build the BSP
```

Treat `menuconfig`, `pkgs --upgrade`, and `pkgs --update` as mutating commands. They may access the network; `menuconfig` also runs `pkgs --update` when `CONFIG_SYS_AUTO_UPDATE_PKGS=y`.

## Diagnose

```bash
command -v python3 menuconfig pkgs sdk scons arm-none-eabi-gcc
python3 -c 'import kconfiglib; print(kconfiglib.__file__)'
pkgs --printenv
git -C /home/huan/SDK/rt-thread describe --tags --always --dirty
arm-none-eabi-gcc --version
```

Use the executable wrappers `menuconfig`, `pkgs`, and `sdk`. Invoke non-executable `env.py` only through the activated `python3`.
