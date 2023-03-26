## Modules path
```
/lib/modules/5.16.14-1-MANJARO/kernel/drivers/gpu/drm/amd/amdgpu/amdgpu.ko.xz
/lib/modules/5.16.14-1-MANJARO/kernel/drivers/platform/x86/nvidia-wmi-ec-backlight.ko.xz
```

https://wiki.archlinux.org/title/Kernel/Traditional_compilation

## amdgpu patch
Return immediately from `amdgpu_dm_register_backlight_device`
from drivers/gpu/drm/amd/display/amdgpu_dm/amdgpu_dm.c


## Config
```
zcat /proc/config.gz > .config
CONFIG_LOCALVERSION="-1-MANJARO"
CONFIG_IKHEADERS=n
```

## Attempts
Tue Mar 22 10:47:52 PM EET 2022

error on boot:
```
BPF: ENUM r_set_level_irq
invalid name
```

# Credits

Credits go to Daniel Dadap for proposing the fix.

See https://patchwork.kernel.org/project/platform-driver-x86/list/?submitter=58091&state=%2A&archive=both
