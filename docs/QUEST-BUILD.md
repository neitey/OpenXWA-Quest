# Neitey's XW: Alliance VR (based on OpenXWA) for Meta Quest: building

The VR and Quest work lives on the `openxr-frame-loop` branch of this repo
and of the engine repo [`aeron-quest`](https://github.com/neitey/aeron-quest)
(the `aeron` submodule). The upstream projects are
[elyosh/OpenXWA](https://github.com/elyosh/OpenXWA) and elyosh/aeron. Their
`main` branches are kept here as `upstream-main` for merging later updates.

```
git clone --recursive -b openxr-frame-loop https://github.com/neitey/OpenXWA-Quest.git
```

Install and test steps for the headset: `packaging/quest/QUEST-INSTALL.txt`.

## Normal build (Android NDK r27+ and SDK)

Cross-compile the dependencies for arm64-v8a / android-29 into one prefix:
SDL (current main, with `SDL_GPU_OPENXR`), zstd, FFmpeg
(avformat/avcodec/avutil/swresample/swscale) and the Khronos OpenXR-SDK
loader. Then build:

```
cmake -S OpenXWA -B build-android -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DANDROID_STL=c++_shared \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=$PREFIX -DCMAKE_FIND_ROOT_PATH=$PREFIX \
  -DAERON_SHADERCROSS_EXECUTABLE=/path/to/shadercross -DAERON_SHADER_FORMATS=SPIRV \
  -DXWA_BUILD_TOOLS=OFF -DXWA_ENABLE_DEBUG_UI=OFF
cmake --build build-android --target xwa
python3 OpenXWA/packaging/quest/build_apk.py --help
```

## Build without the NDK (`packaging/quest/tools/`)

How the current test builds were made on a server that could only reach
GitHub:

- **Sysroot.** System clang 18 (`--target=aarch64-linux-android29`) with a
  sysroot assembled from AOSP bionic (`aosp-mirror/platform_bionic`):
  `libc/include`, `libc/kernel/uapi` (with `asm-arm64/asm` copied to
  `usr/include/aarch64-linux-android/asm`) and `libc/kernel/android/uapi`.
- **Stub libraries.** `stubgen.py` turns bionic's `libc/libm/libdl *.map.txt`
  into stub `libc.so`, `libm.so` and `libdl.so` (versioned symbols, API 29).
  `crtbegin_so.o` comes from bionic's `arch-common/bionic/crtbegin_so.c`,
  plus a minimal `crtend_so.o`.
- **C++ runtime.** libc++ 18 headers with a hand-written `__config_site`
  (ABI namespace `__ndk1`), plus compiler-rt builtins and a static libunwind
  built from llvm-project `release/18.x`. They are used against the NDK's
  `libc++_shared.so`.
- **Prebuilt libraries.** SDL3 (commit `ce3af25d4`), FFmpeg `n8.0.1`, zstd
  1.5.7 and the OpenXR loader `.so` files come from an earlier APK. Their
  headers come from the matching upstream tags, and Vulkan headers from
  KhronosGroup/Vulkan-Headers.
- **Shaders.** `fake-shadercross` stands in for SDL_shadercross and copies
  previously compiled SPIR-V, so no DXC is needed. Any changed shader still
  has to be compiled for real. The FSR shader generator is built host-native
  with `fsr3_shadergen_reuse.patch`, which skips reflection validation.
- **Configure.** Use `toolchain.cmake` as `CMAKE_TOOLCHAIN_FILE` with the
  options above, and `PKG_CONFIG_LIBDIR` pointing at the prefix.
- **Packaging.** `apk_repack_sign.py` swaps the new `libmain.so` (stripped)
  into the previous APK, aligns it and signs it with APK Signature Scheme v2.
  `sdl_clipboard_dex.py` rewrites the previous APK's `classes.dex` with SDL's
  clipboard handler tolerating a missing clipboard service (Steam Frame),
  using Apktool's baksmali/smali; `build_apk.py` patches the Java source the
  same way.

## Optional X-Wing Revival cockpits

VR Options -> Cockpit Model can show 3D cockpits by
[Raptor007](https://www.raptor007.com/x-wing-revival/) (X-Wing Revival) and
the [X-Wing Alliance Upgrade team](https://xwaupgrade.com/). They are
optional and off by default (X-Wing Alliance's own cockpits). The models are
not in this repository and not covered by its GPL. The packaging converts
them with `packaging/quest/tools/gen_revival_cockpits.py` and the
`aeron_gltf_cook` tool (`-DXWA_BUILD_TOOLS=ON` host build):

```
# once: copy what the conversion reads out of the Revival download
python3 packaging/quest/tools/gen_revival_cockpits.py stage X-Wing_Revival_0_6_1_Alpha.zip revival-data
# at package time: revival-glb/xwing.glb ywing.glb ... falcon.glb
python3 packaging/quest/tools/gen_revival_cockpits.py build build-host/aeron_gltf_cook revival-data revival-glb
```

The GLBs go into the APK as `assets/revival_cockpits/*.glb` (on PC: the
`revival_cockpits` folder next to `OpenXWA.exe`; `XWA_XR_REVIVAL_COCKPITS`
points the game at another folder). Without them every craft keeps XWA's
cockpit. Credits: `THIRD-PARTY-NOTICES.txt` (24).

## Signing key

Test builds are signed with a key that is **not** in this repository; keep it
somewhere safe. Android only installs an update over an existing install when
the key matches. A build with another key needs `adb uninstall` first, and
that deletes the copied game files.
