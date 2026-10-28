


# Factorio on Android

Hello, this is my PoC of Factorio running natively on Android. It was mostly done out of boredom, and with a heavy usage of AI. I am disclosing it here, because I think that ALL projects using AI in any aspect should do so.


# Implementation

My first attempt was made in Termux using a Debian container in chroot. Performance was hideous because overhead was quite large, and my outdated, low-end mobile hardware couldn't even run it properly.

My second attempt involved the fact that the original Factorio linux-arm64 build runs glibc, and Android runs bionic(implementation of glibc). I needed a way to translate calls from one implementation to another, and this was mostly done by AI, with manual verification of each iteration.

> tools/port.py contains the slop of a translation code that it produced in the end, and fshim has the implementations of native instructions.

Second part of the integration was the SDL library, which is designed to be pretty portable.
After I figured that out, the hardest part began. Opengl 3.3 -> Gl ES translation. Main differences of those are dumbed down shader programs and hardware specfic features (like texture compressions), and framebuffer differences.
Apparently different mobile GPU manufacturers don't really stick to the same standards of gles, so what worked perfectly on Mali, now displayed garbage data from previous framebuffers and made half the screen flicker on Adreno...
I gave up on using mesa for adreno, so I made the ai write a fix for adreno from scratch... Ending up in a standalone translation layer GLCompat.
Don't get me wrong, the code is garbage, but it actually works.
As for the other thing I used, Panfrost, it worked pretty much out of the box.

# What works

 - Different aspect ratios, rotation
 - Graphics on Adreno and Mali chips(at least on the two tested)
 - Sound
 - Networking
 - Gamepad controls(enable them in the settings first!!!)

# From my tests

## Samsung Galaxy A51

- Exynos 9611, Mali-G72 MP3, about 3.5 GB RAM free idle
- Android 13 (API 33), 2400×1080 display
- The SDL/GLES window test ran at about 51 FPS for 10 seconds.
- Factorio 2.1.23 completed a 120-tick benchmark with the same checksum as the
  stock Linux build: `2195791750`.
- In the tested menu scene, later GLCompat builds reached about 31–33.5 FPS.
  This was GPU-bound and is not a general performance guarantee.

## Redmi Note 7

- Snapdragon 660, Adreno 512, about 3.7 GB RAM free idle
- Android 14 custom AOSP
- The GLCompat streaming fix was tested on-device. The device stayed alive with
  no fatal signal or GL error in the captured run; Loaded a pretty heavy, mid-game save.


 
# Building

Install the general build dependencies from dependencies.md (was not tested outside of a docker test ubuntu image).

Download a Factorio Linux ARM64 archive from [factorio.com/download](https://factorio.com/download) and place it directly in the `input/` directory. Either the base game or Space Age archive. (Space Age requires much more ram. Minimal ram for regular version is 6GB+)
Run:

```sh
./build.sh
```

The APKs are written to build/output/apk/:

  - factorio-{version}-panfrost-experimental.apk
  - factorio-{version}-glcompat-unverisal.apk

## GLCompat diagnostics build

By default GLCompat is built without instrumentation. For profiling, build with:

```sh
GLCOMPAT_DIAG=ON ./build.sh
```

This brings back the per-wrapper profiler (`adb logcat -s glcompat-perf`, a dump every 10 s), GL error
attribution (`glcompat-err`), the stream report (`glcompat-stream`), and timing probes on
`glClientWaitSync`/`glWaitSync`/`glFinish`/`glFlush`/`glReadPixels`/`glBufferSubData`, where CPU↔GPU
synchronisation usually hides.

Runtime knobs live in `/storage/emulated/0/Android/data/org.factorio.android/files/glcompat.ini`
(`key=value`, no reinstall needed): `trace`, `fence`, `early_unmap`, `safe`, `invalidate_range`, `verbose`.


# Installing

Install the APK you need, for Mali:
```sh
adb install build/output/apk/factorio-{version}-panfrost-experimental.apk
```
or for Adreno and others:
```sh
adb install build/output/apk/factorio-{version}-glcompat-unverisal.apk
```

Copy the Factorio *data* directory from the previously downloaded archive to:
/storage/emulated/0/Android/data/org.factorio.android/files/data

# Huge thanks to:
- https://github.com/JimVulkan/mali-panfrost for the actual working driver on Mali
- https://github.com/libsdl-org/SDL for being so damn portable and convenient
- https://gitlab.freedesktop.org/mesa for adreno specific code parts
- https://github.com/hglm/detex for texture decoding references
- https://github.com/github-e51/factorio-native-arm-ipad for giving me an idea of trying to make it work on Android
