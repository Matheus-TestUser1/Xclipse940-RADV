# radv-xclipse-patches

Trying to get Mesa's RADV driver running on Samsung's Xclipse 920 (RDNA-based GPU) inside Termux on Android.

## Current Status

- Compiles and runs in Termux
- Vulkan initialization works
- Shaders compile fine
- Command submission reaches the GPU (submit ip=0)
- Basic rendering pipeline is alive

## What's Broken

The main issue is WSI (Window System Integration). 
The driver wants to use proper DRM format modifiers + DRI3 with file descriptors to hand frames to termux-x11, but Android's sandbox blocks it → BadDrawable crashes.

Fallback attempts either freeze the phone or result in "ghost rendering" (GPU works but nothing shows up).

## Goal

Get a working Vulkan driver for the Xclipse 920 in a non-root Termux environment, and eventually take advantage of root for direct /dev/dri access and zero-copy DMA-BUF.

## How to Build

```bash
cd ~/mesa_src
git checkout -- src/vulkan/wsi/wsi_common_x11.c
rm -rf build
meson setup build \
  --prefix=$PREFIX \
  -Dplatforms=x11 \
  -Dvulkan-drivers=amd \
  -Dgallium-drivers=zink \
  -Dbuildtype=release
cd build && ninja install
```

## How to Load and Test the Driver

```bash
termux-x11 :0 -ac &
sleep 2
export DISPLAY=:0
export VK_ICD_FILENAMES=/data/data/com.termux/files/usr/share/vulkan/icd.d/radeon_icd.aarch64.json
export GALLIUM_DRIVER=zink
unset MESA_LOADER_DRIVER_OVERRIDE
```

Then run any Vulkan app you want to test.

## Notes

This is very much a work in progress / research project. Lots of reverse engineering and dirty hacks involved.

**Note from the author:** I'm not the best at this and would really appreciate any help or suggestions. I'll keep trying my best though.

More details in the commit history.