#!/usr/bin/env bash
# INITIAL DRAFT: clean-room Factorio Android build orchestrator.
# Deliberately no cleanup/deletion. Each attempt gets a unique intermediate tree;
# published APK paths are never overwritten. This script has not been build-tested.
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
cd "$ROOT"

usage() {
    echo "Usage: $0 [/path/to/factorio_linux-arm64_VERSION.tar.xz]" >&2
    echo "   or: $0 [/path/to/factorio-space-age_linux-arm64_VERSION.tar.xz]" >&2
    exit 2
}
mkdir -p "$ROOT/input"
[[ $# -le 1 ]] || usage
if [[ $# == 1 ]]; then
    ARCHIVE=$(realpath -- "$1")
else
    matches=()
    versions=()
    for candidate in "$ROOT"/input/*.tar.xz; do
        [[ -f $candidate ]] || continue
        candidate_base=${candidate##*/}
        if [[ $candidate_base =~ ^factorio(-space-age)?_linux-arm64_([0-9]+\.[0-9]+\.[0-9]+)\.tar\.xz$ ]]; then
            matches+=("$candidate")
            versions+=("${BASH_REMATCH[2]}")
        fi
    done
    ((${#matches[@]} > 0)) || {
        echo "No supported Factorio ARM64 .tar.xz archives found directly in $ROOT/input" >&2
        exit 1
    }
    highest=$(printf '%s\n' "${versions[@]}" | sort -V | tail -n 1)
    highest_matches=()
    for i in "${!matches[@]}"; do
        [[ ${versions[$i]} == "$highest" ]] && highest_matches+=("${matches[$i]}")
    done
    ((${#highest_matches[@]} == 1)) || {
        echo "Multiple Factorio ARM64 archives match highest version $highest; pass the desired archive path explicitly:" >&2
        printf '  %s\n' "${highest_matches[@]}" >&2
        exit 1
    }
    ARCHIVE=$(realpath -- "${highest_matches[0]}")
fi
[[ -f "$ARCHIVE" ]] || { echo "Missing Factorio archive: $ARCHIVE" >&2; exit 1; }
BASE=$(basename -- "$ARCHIVE")
if [[ $BASE =~ ^factorio(-space-age)?_linux-arm64_([0-9]+\.[0-9]+\.[0-9]+)\.tar\.xz$ ]]; then
    GAME_VERSION=${BASH_REMATCH[2]}
else
    echo "Unsupported archive filename (expected Factorio Linux ARM64 versioned .tar.xz)" >&2
    exit 1
fi
APKOUT="$ROOT/build/output/apk"

SDK=${ANDROID_SDK_ROOT:-${ANDROID_HOME:-$HOME/Android/Sdk}}
NDK=${ANDROID_NDK_HOME:-$SDK/ndk/android-ndk-r28c}
BT=${BUILD_TOOLS:-$SDK/build-tools/36.0.0}
ANDROID_JAR=${ANDROID_JAR:-$SDK/platforms/android-37.0/android.jar}
export PATH="$BT:$PATH"
for required in tools/port.py tools/elf.py native/fshim/CMakeLists.txt \
    native/glcompat/CMakeLists.txt patches/sdl/sdl-dynapi-android.patch \
    patches/sdl/sdl-glproc-android.patch android-wrapper/AndroidManifest.xml \
    android-wrapper/config.ini.template; do
    [[ -f "$ROOT/$required" ]] || { echo "Missing project input: $required" >&2; exit 1; }
done
[[ -d "$ROOT/android-wrapper/java" ]] || { echo "Missing Android Java sources: android-wrapper/java" >&2; exit 1; }

for tool in bash python3 tar curl git cmake ninja meson pkg-config flex bison sha256sum \
    clang llvm-config glslangValidator patch aapt2 d8 zipalign apksigner \
    javac jar keytool zip realpath grep sort tail find awk ln cp cat cut dirname basename; do
    command -v "$tool" >/dev/null || { echo "Missing required tool: $tool" >&2; exit 1; }
done
NINJA=$(command -v ninja)
export PATH="$(dirname -- "$NINJA"):$PATH"
python3 - <<'PY'
required_modules = ("capstone", "mako", "yaml", "packaging")
missing = []
for module in required_modules:
    try:
        __import__(module)
    except ImportError:
        missing.append(module)
if missing:
    raise SystemExit("Missing Python modules: " + ", ".join(missing))
PY
[[ -f "$NDK/build/cmake/android.toolchain.cmake" ]] || { echo "Missing Android NDK toolchain: $NDK" >&2; exit 1; }
[[ -f "$ANDROID_JAR" ]] || { echo "Missing Android platform jar: $ANDROID_JAR" >&2; exit 1; }
[[ -x "$BT/aapt2" && -x "$BT/d8" && -x "$BT/zipalign" && -x "$BT/apksigner" ]] || { echo "Incomplete Android build-tools directory: $BT" >&2; exit 1; }

BUILDROOT="$ROOT/build"
INTERMEDIATE="$BUILDROOT/intermediate"
DEPS="$BUILDROOT/dependencies"
SDL_VERSION=3.4.14
SDL_PATCH_KEY=$(cat patches/sdl/sdl-dynapi-android.patch patches/sdl/sdl-glproc-android.patch | sha256sum | cut -c1-12)
SDL_BUILD="$INTERMEDIATE/sdl-$SDL_VERSION-$SDL_PATCH_KEY"
SDL_SOURCE="$DEPS/sdl-$SDL_VERSION/source"
SDL_PATCHED="$DEPS/sdl-$SDL_VERSION/patched-$SDL_PATCH_KEY"
FSHIM_BUILD="$INTERMEDIATE/fshim/$GAME_VERSION"
GLCOMPAT_BUILD="$INTERMEDIATE/glcompat-$SDL_PATCH_KEY"
FACTORIO_BUILD="$INTERMEDIATE/factorio/$GAME_VERSION"
PANFROST_COMMIT=8af067e0a48a977334838c97f4a992bbfa100f69
PANFROST_URL=https://github.com/JimVulkan/mali-panfrost.git
PANFROST_SOURCE="$DEPS/mali-panfrost-$PANFROST_COMMIT"
PANFROST_BUILD="$INTERMEDIATE/panfrost-android-$PANFROST_COMMIT"
# Space-separated APK variants to produce. FACTORIO_VARIANTS=glcompat skips the
# whole Mesa/Panfrost fetch and build (it needs libclc and SPIRV-LLVM-Translator
# on the host for mesa_clc).
VARIANTS=" ${FACTORIO_VARIANTS:-panfrost glcompat} "
want_variant() { [[ $VARIANTS == *" $1 "* ]]; }

mkdir -p "$INTERMEDIATE" "$DEPS" "$BUILDROOT/output/apk"
RUN=$(mktemp -d "$INTERMEDIATE/run.XXXXXXXX")
mkdir -p "$RUN/factorio/bin/arm64" "$RUN/package"
LOG_DIR="$INTERMEDIATE/logs/$GAME_VERSION"
mkdir -p "$LOG_DIR"
echo "== Factorio Android build: $GAME_VERSION =="
echo "== run directory: $RUN =="
echo "== persistent dependencies: $DEPS =="
echo "== persistent native builds: $INTERMEDIATE =="
step() { printf '\n==> %s\n' "$*"; }
run_logged() {
    local label=$1 log=$2 status warnings
    shift 2
    step "$label"
    if "$@" >"$log" 2>&1; then
        warnings=$(grep -ci 'warning:' "$log" || true)
        if [[ $warnings -gt 0 ]]; then
            printf '    completed with %s warning(s); details: %s\n' "$warnings" "$log"
        else
            printf '    completed; details: %s\n' "$log"
        fi
    else
        status=$?
        printf '    FAILED (exit %s); full output follows from %s\n' "$status" "$log" >&2
        cat "$log" >&2
        return "$status"
    fi
}

# Extract only the known executable member. Python verifies it is exactly one
# regular file and streams it to a fixed destination (no archive paths are used
# to construct output paths).
step "Extracting only the Factorio executable from $BASE"
case "$BASE" in
    factorio_linux-arm64_*) EXE_MEMBER="factorio/bin/arm64/factorio" ;;
    factorio-space-age_linux-arm64_*) EXE_MEMBER="factorio/bin/arm64/factorio" ;;
esac
GAME_EXE="$RUN/factorio/bin/arm64/factorio"
python3 - "$ARCHIVE" "$EXE_MEMBER" "$GAME_EXE" <<'PY'
import pathlib
import shutil
import sys
import tarfile

archive, member_name, destination = sys.argv[1:]
destination = pathlib.Path(destination)
with tarfile.open(archive, mode="r:xz") as tf:
    matches = [m for m in tf.getmembers() if m.name == member_name]
    if len(matches) != 1 or not matches[0].isfile():
        raise SystemExit(f"Expected exactly one regular executable member: {member_name}")
    source = tf.extractfile(matches[0])
    if source is None:
        raise SystemExit(f"Cannot read archive member: {member_name}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    with source, destination.open("xb") as output:
        shutil.copyfileobj(source, output)
    destination.chmod(0o755)
PY
step "Translating Factorio $GAME_VERSION into the Android-loadable game library"
mkdir -p "$FACTORIO_BUILD"
python3 tools/port.py --source "$GAME_EXE" \
    --out "$FACTORIO_BUILD/libfactorio_game.so" --report "$FACTORIO_BUILD/port-report.json" \
    --constants "$FACTORIO_BUILD/game_constants.h" --game-version "$GAME_VERSION"

# Use source releases pinned by version/tag or immutable commit. The SDL GitHub
# release tag is 3.4.14; Panfrost is pinned to the tested JimVulkan commit.
SDL_URL="https://github.com/libsdl-org/SDL/archive/refs/tags/release-$SDL_VERSION.tar.gz"
SDL_ARCHIVE="$DEPS/SDL3-$SDL_VERSION.tar.gz"
if [[ ! -f $SDL_ARCHIVE ]]; then
    step "Downloading SDL $SDL_VERSION source"
    curl --fail --location --retry 3 "$SDL_URL" --output "$SDL_ARCHIVE"
else
    step "Using cached SDL $SDL_VERSION source archive"
fi
if [[ ! -f "$SDL_SOURCE/CMakeLists.txt" ]]; then
    if [[ -n $(find "$SDL_SOURCE" -mindepth 1 -maxdepth 1 -print -quit 2>/dev/null || true) ]]; then
        echo "Incomplete cached SDL source tree: $SDL_SOURCE (preserving it; inspect manually)" >&2
        exit 1
    fi
    step "Extracting SDL source into persistent dependency directory $SDL_SOURCE"
    mkdir -p "$SDL_SOURCE"
    tar -xzf "$SDL_ARCHIVE" -C "$SDL_SOURCE" --strip-components=1 --no-same-owner --no-same-permissions
fi
[[ -f "$SDL_SOURCE/CMakeLists.txt" ]] || { echo "SDL archive layout unexpected" >&2; exit 1; }
step "Applying or verifying SDL Android patches: dynapi, then GL procedure lookup"
if [[ ! -f "$SDL_PATCHED/CMakeLists.txt" ]]; then
    [[ ! -e $SDL_PATCHED ]] || { echo "Incomplete patched SDL directory: $SDL_PATCHED" >&2; exit 1; }
    step "Creating persistent patched SDL tree $SDL_PATCHED"
    cp -a "$SDL_SOURCE" "$SDL_PATCHED"
    patch --batch --fuzz=0 -d "$SDL_PATCHED" -p1 --forward < patches/sdl/sdl-dynapi-android.patch
    patch --batch --fuzz=0 -d "$SDL_PATCHED" -p1 --forward < patches/sdl/sdl-glproc-android.patch
else
    step "Reusing persistent patched SDL tree $SDL_PATCHED"
    patch --dry-run --silent --batch --fuzz=0 --reverse -d "$SDL_PATCHED" -p1 < patches/sdl/sdl-dynapi-android.patch || {
        echo "Cached SDL dynapi patch state is inconsistent: $SDL_PATCHED" >&2; exit 1;
    }
    patch --dry-run --silent --batch --fuzz=0 --reverse -d "$SDL_PATCHED" -p1 < patches/sdl/sdl-glproc-android.patch || {
        echo "Cached SDL GL lookup patch state is inconsistent: $SDL_PATCHED" >&2; exit 1;
    }
fi
# Intentionally do not apply the abandoned touch-hold experiment.

if ! want_variant panfrost; then
    step "Skipping Panfrost source (FACTORIO_VARIANTS=${VARIANTS# })"
elif [[ ! -d "$PANFROST_SOURCE/.git" ]]; then
    step "Fetching Panfrost source at pinned commit $PANFROST_COMMIT into persistent dependencies"
    git clone --filter=blob:none --no-checkout "$PANFROST_URL" "$PANFROST_SOURCE"
    git -C "$PANFROST_SOURCE" fetch --depth=1 origin "$PANFROST_COMMIT"
    git -C "$PANFROST_SOURCE" checkout --detach "$PANFROST_COMMIT"
else
    step "Reusing persistent Panfrost source at $PANFROST_SOURCE"
    CURRENT_PANFROST=$(git -C "$PANFROST_SOURCE" rev-parse HEAD)
    [[ $CURRENT_PANFROST == "$PANFROST_COMMIT" ]] || {
        echo "Panfrost cache is at $CURRENT_PANFROST, expected $PANFROST_COMMIT; preserving it." >&2; exit 1;
    }
fi

# Build the project sources into persistent CMake trees. Dependency sources and
# their build outputs are retained under build/ for incremental later runs.
run_logged "Configure fshim (CMake)" "$LOG_DIR/fshim-configure.log" \
    cmake -S native/fshim -B "$FSHIM_BUILD" -G Ninja \
    -DCMAKE_MAKE_PROGRAM="$NINJA" \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 -DCMAKE_BUILD_TYPE=Release \
    -DGAME_INCLUDE_DIR="$FACTORIO_BUILD"
run_logged "Build fshim (Ninja)" "$LOG_DIR/fshim-build.log" \
    "$NINJA" -C "$FSHIM_BUILD" fshim

run_logged "Configure patched SDL $SDL_VERSION (CMake)" "$LOG_DIR/sdl-configure.log" \
    cmake -S "$SDL_PATCHED" -B "$SDL_BUILD" -G Ninja \
    -DCMAKE_MAKE_PROGRAM="$NINJA" \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 -DCMAKE_BUILD_TYPE=Release \
    -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_LIBC=ON
run_logged "Build SDL3 $SDL_VERSION (Ninja/Clang)" "$LOG_DIR/sdl-build.log" \
    "$NINJA" -C "$SDL_BUILD" SDL3-shared

run_logged "Configure GLCompat (CMake)" "$LOG_DIR/glcompat-configure.log" \
    cmake -S native/glcompat -B "$GLCOMPAT_BUILD" -G Ninja \
    -DCMAKE_MAKE_PROGRAM="$NINJA" \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 -DCMAKE_BUILD_TYPE=Release \
    -DSDL_INCLUDE_DIR="$SDL_PATCHED/include" \
    -DGLCOMPAT_DIAG="${GLCOMPAT_DIAG:-OFF}"
run_logged "Build GLCompat (Ninja/Clang)" "$LOG_DIR/glcompat-build.log" \
    "$NINJA" -C "$GLCOMPAT_BUILD"

if want_variant panfrost; then
    # Mesa build.sh handles host tools and cross compilation. Its source and build
    # trees persist, but its script always invokes Ninja. Skip that invocation when
    # the complete output set is present and the source/build configuration matches.
    PANFROST_CACHE="$PANFROST_BUILD/.factorio-clean-cache-key"
    PANFROST_CACHE_KEY=$(printf '%s\n' \
        "$PANFROST_COMMIT" "$NDK" "$(sha256sum "$PANFROST_SOURCE/build.sh" | awk '{print $1}')" \
        "$(sha256sum "$PANFROST_SOURCE/meson.build" | awk '{print $1}')" \
        "$("$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/clang" --version | head -n1)" | sha256sum | awk '{print $1}')
    PANFROST_OUTPUTS=(
        "$PANFROST_SOURCE/build-host/bin/mesa_clc"
        "$PANFROST_SOURCE/build-host/bin/vtn_bindgen2"
        "$PANFROST_SOURCE/build-host/bin/panfrost_compile"
        "$PANFROST_BUILD/src/egl/libEGL_mesa.so"
        "$PANFROST_BUILD/src/mesa/glapi/es2api/libGLESv2_mesa.so"
        "$PANFROST_BUILD/src/gallium/targets/dri/libgallium_dri.so"
        "$PANFROST_BUILD/src/android_stub/libhardware.so"
        "$PANFROST_BUILD/src/android_stub/liblog.so"
        "$PANFROST_BUILD/src/android_stub/libnativewindow.so"
        "$PANFROST_BUILD/src/android_stub/libsync.so"
    )
    PANFROST_OUTPUTS_READY=true
    for output in "${PANFROST_OUTPUTS[@]}"; do
        [[ -s $output ]] || { PANFROST_OUTPUTS_READY=false; break; }
    done
    if [[ $PANFROST_OUTPUTS_READY == true && -f $PANFROST_CACHE && $(<"$PANFROST_CACHE") == "$PANFROST_CACHE_KEY" ]]; then
        step "Reusing cached Panfrost host tools and Android libraries"
        echo "    source: $PANFROST_SOURCE"
        echo "    build: $PANFROST_BUILD"
    else
        run_logged "Build Panfrost host tools and Android libraries" "$LOG_DIR/panfrost-build.log" \
            env PATH="$PANFROST_SOURCE/build-host/bin:$PATH" BUILD_DIR="$PANFROST_BUILD" \
            "$PANFROST_SOURCE/build.sh" "$NDK"
        for output in "${PANFROST_OUTPUTS[@]}"; do
            [[ -s $output ]] || { echo "Panfrost build did not produce required output: $output" >&2; exit 1; }
        done
        printf '%s\n' "$PANFROST_CACHE_KEY" > "$PANFROST_CACHE"
    fi
fi

# Package the base wrapper and backend-specific libraries into this run's APK stage.
step "Preparing Android wrapper APK package"
PKG="$RUN/package"
mkdir -p "$PKG/classes" "$PKG/dex" "$PKG/assets" "$PKG/stage/lib/arm64-v8a"
cp android-wrapper/config.ini.template "$PKG/assets/config.ini.template"
find android-wrapper/java -name '*.java' -print > "$PKG/sources.txt"
run_logged "Compile Android Java wrapper" "$LOG_DIR/javac.log" \
    javac --release 11 -Xlint:-options -cp "$ANDROID_JAR" -d "$PKG/classes" @"$PKG/sources.txt"
run_logged "Assemble Java classes" "$LOG_DIR/jar.log" \
    jar cf "$PKG/classes.jar" -C "$PKG/classes" .
run_logged "Convert Java classes to DEX" "$LOG_DIR/d8.log" \
    d8 --release --min-api 26 --lib "$ANDROID_JAR" --output "$PKG/dex" "$PKG/classes.jar"
run_logged "Link Android wrapper manifest and assets" "$LOG_DIR/aapt2.log" \
    aapt2 link -o "$PKG/base.apk" -I "$ANDROID_JAR" -A "$PKG/assets" --manifest android-wrapper/AndroidManifest.xml \
    --min-sdk-version 26 --target-sdk-version 33 --version-code 1 --version-name "$GAME_VERSION"
KEYSTORE=${FACTORIO_KEYSTORE:-$ROOT/build/intermediate/debug.keystore}
if [[ ! -f $KEYSTORE ]]; then
    mkdir -p "$(dirname -- "$KEYSTORE")"
    run_logged "Generate debug signing key" "$LOG_DIR/keytool.log" \
        keytool -genkeypair -noprompt -keystore "$KEYSTORE" -storepass android \
        -alias androiddebugkey -keypass android -keyalg RSA -keysize 2048 \
        -validity 10000 -dname 'CN=Android Debug,O=Android,C=US'
fi

package_variant() {
    local variant=$1 backend=$2 suffix=$3 stage="$PKG/$1/stage" libs="$PKG/$1/libs" unsigned="$PKG/$1/unsigned.apk" aligned="$PKG/$1/aligned.apk" signed="$PKG/$1/signed.apk" target="$APKOUT/factorio-$GAME_VERSION-$3.apk"
    step "Packaging and signing $variant variant: $(basename -- "$target")"
    mkdir -p "$stage/lib/arm64-v8a" "$libs"
    cp "$SDL_BUILD/libSDL3.so" "$libs/"
    cp "$FSHIM_BUILD/lib/libfshim.so" "$stage/lib/arm64-v8a/"
    cp "$FACTORIO_BUILD/libfactorio_game.so" "$stage/lib/arm64-v8a/"
    cp "$libs/libSDL3.so" "$stage/lib/arm64-v8a/"
    if [[ $backend == glcompat ]]; then
        cp "$GLCOMPAT_BUILD/lib/libGLCompat.so" "$stage/lib/arm64-v8a/"
    else
        PANBUILD="$PANFROST_BUILD"
        PANLIB="$stage/lib/arm64-v8a"
        cp "$PANBUILD/src/egl/libEGL_mesa.so" "$PANLIB/"
        cp "$PANBUILD/src/mesa/glapi/es2api/libGLESv2_mesa.so" "$PANLIB/"
        cp "$PANBUILD/src/gallium/targets/dri/libgallium_dri.so" "$PANLIB/"
        for name in libhardware liblog libnativewindow libsync; do
            cp "$PANBUILD/src/android_stub/$name.so" "$PANLIB/"
        done
    fi
    cp "$PKG/base.apk" "$unsigned"
    cp "$PKG/dex/classes.dex" "$stage/classes.dex"
    (cd "$stage" && zip -q -X -9 "$unsigned" classes.dex)
    mapfile -t native_entries < <(cd "$stage" && find lib/arm64-v8a -maxdepth 1 -type f -printf '%p\n' | sort)
    (cd "$stage" && zip -q -X -0 "$unsigned" "${native_entries[@]}")
    run_logged "Align $variant APK" "$LOG_DIR/zipalign-$variant.log" \
        zipalign -P 16 -f 4 "$unsigned" "$aligned"
    run_logged "Sign $variant APK" "$LOG_DIR/apksigner-$variant.log" \
        apksigner sign --ks "$KEYSTORE" --ks-pass pass:android --key-pass pass:android \
            --ks-key-alias androiddebugkey --out "$signed" "$aligned"
    run_logged "Verify $variant APK signature" "$LOG_DIR/verify-$variant.log" \
        apksigner verify --print-certs "$signed"
    # APKs are reproducible outputs; replace an existing regular APK in place.
    [[ ! -L $target ]] || { echo "Refusing APK output symlink: $target" >&2; return 1; }
    cp -f -- "$signed" "$target"
    echo "Created $target"
}
want_variant panfrost && package_variant panfrost panfrost panfrost-experimental
want_variant glcompat && package_variant glcompat glcompat glcompat-unverisal
printf '\nBuild completed. APKs are in %s\n' "$APKOUT"
