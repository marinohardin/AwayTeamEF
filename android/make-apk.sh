#!/bin/bash
# Build the AwayTeamEF Android APK (arm64-v8a, flat) without Gradle:
#   NDK CMake (android/CMakeLists.txt: SDL3, sdl2-compat, the engine, the game modules)
#   -> javac + d8 (SDL 3's Java glue + EFActivity) -> aapt2 link -> zip -> zipalign -> apksigner
#
#   android/make-apk.sh            -> build-android/AwayTeamEF-<version>-arm64.apk
#
# Install and run (a device or an arm64 emulator with Vulkan):
#   adb install -r build-android/AwayTeamEF-1.5.0-arm64.apk
#   adb shell mkdir -p /sdcard/Android/obb/org.awayteamef.app/baseEF
#   adb push pak0.pk3 pak1.pk3 pak2.pk3 pak3.pk3 /sdcard/Android/obb/org.awayteamef.app/baseEF/
#   adb shell am start -n org.awayteamef.app/.EFActivity -e args "'+set s_volume 0 +map borg1'"
#   adb logcat -s VoyagerSP EFSP SDL AwayTeamEF
# Uninstalling the app deletes that folder (data, configs and saves) too.
#
# Not in this build yet: Bink cinematics (no Android FFmpeg; movies are skipped),
# co-op (EF_COOP), touch controls art, and the Quest/OpenXR parts (see AndroidManifest.xml).
#
# Environment (all optional):
#   ANDROID_HOME        SDK (default ~/Library/Android/sdk)
#   ANDROID_NDK         NDK (default: the newest under $ANDROID_HOME/ndk)
#   BUILD_TOOLS         build-tools version (default: the newest installed)
#   PLATFORM            compile-against platform (default: the newest installed, e.g. android-36)
#   OUT                 build directory (default <repo>/build-android)
#   VERSION             app version (default 1.5.0)
#   EF_ANDROID_KEYSTORE debug signing key (default ~/.android/awayteamef-debug.keystore;
#                       made on first use, never in the repo)
#   JOBS                parallel compile jobs
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/deps/versions.sh"

ANDROID_HOME="${ANDROID_HOME:-$HOME/Library/Android/sdk}"
newest() { ls -1 "$1" 2>/dev/null | sort -V | tail -1; }
ANDROID_NDK="${ANDROID_NDK:-$ANDROID_HOME/ndk/$(newest "$ANDROID_HOME/ndk")}"
BUILD_TOOLS="${BUILD_TOOLS:-$(newest "$ANDROID_HOME/build-tools")}"
PLATFORM="${PLATFORM:-$(newest "$ANDROID_HOME/platforms")}"
BT="$ANDROID_HOME/build-tools/$BUILD_TOOLS"
ANDROID_JAR="$ANDROID_HOME/platforms/$PLATFORM/android.jar"
OUT="${OUT:-$ROOT/build-android}"
VERSION="${VERSION:-1.5.0}"
PACKAGE=org.awayteamef.app
MIN_SDK=29
TARGET_SDK="${PLATFORM#android-}"
TARGET_SDK="${TARGET_SDK%%.*}"
KEYSTORE="${EF_ANDROID_KEYSTORE:-$HOME/.android/awayteamef-debug.keystore}"
JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || nproc)}"
# 1.5.0 -> 10500
IFS=. read -r _ma _mi _pa <<<"$VERSION"
VERSION_CODE=$(( ${_ma:-0} * 10000 + ${_mi:-0} * 100 + ${_pa:-0} ))

if [ -z "${JAVA_HOME:-}" ] && [ -x /usr/libexec/java_home ]; then
	JAVA_HOME="$(/usr/libexec/java_home 2>/dev/null || true)"
fi
JAVAC="${JAVA_HOME:+$JAVA_HOME/bin/}javac"
KEYTOOL="${JAVA_HOME:+$JAVA_HOME/bin/}keytool"

for f in "$ANDROID_NDK/build/cmake/android.toolchain.cmake" "$BT/aapt2" "$BT/d8" "$BT/zipalign" "$BT/apksigner" "$ANDROID_JAR"; do
	[ -e "$f" ] || { echo "make-apk: missing $f" >&2; exit 1; }
done
command -v cmake >/dev/null && command -v ninja >/dev/null || { echo "make-apk: needs cmake and ninja" >&2; exit 1; }

echo "== NDK $(basename "$ANDROID_NDK"), build-tools $BUILD_TOOLS, $PLATFORM, version $VERSION ($VERSION_CODE)"
T0=$(date +%s)

# ---- SDL 3 + sdl2-compat sources: the desktop build-deps tarballs, else downloaded ----
SRC="$OUT/src"
mkdir -p "$SRC"
fetch_unpack() {	# NAME-VERSION TARBALL SHA URL
	local dir="$1" tgz="$2" sha="$3" url="$4"
	[ -f "$SRC/$dir/CMakeLists.txt" ] && return 0
	local have=""
	for c in "$ROOT/build-deps/$tgz" "$SRC/$tgz"; do
		if [ -f "$c" ] && ef_sha256_check "$c" "$sha"; then have="$c"; break; fi
	done
	if [ -z "$have" ]; then
		echo "== downloading $tgz"
		curl -fL --retry 3 -o "$SRC/$tgz" "$url"
		ef_sha256_check "$SRC/$tgz" "$sha" || { echo "make-apk: $tgz checksum mismatch" >&2; exit 1; }
		have="$SRC/$tgz"
	fi
	tar xzf "$have" -C "$SRC"
}
fetch_unpack "SDL3-$SDL3_VERSION" "SDL3-$SDL3_VERSION.tar.gz" "$SDL3_SHA256" \
	"https://github.com/libsdl-org/SDL/releases/download/release-$SDL3_VERSION/SDL3-$SDL3_VERSION.tar.gz"
fetch_unpack "sdl2-compat-$SDL2COMPAT_VERSION" "sdl2-compat-$SDL2COMPAT_VERSION.tar.gz" "$SDL2COMPAT_SHA256" \
	"https://github.com/libsdl-org/sdl2-compat/releases/download/release-$SDL2COMPAT_VERSION/sdl2-compat-$SDL2COMPAT_VERSION.tar.gz"

# ---- native libraries ----
cmake -S "$ROOT/android" -B "$OUT/native" -G Ninja \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
	-DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-$MIN_SDK -DANDROID_STL=c++_shared \
	-DEF_ANDROID_PACKAGE=$PACKAGE -DEF_APP_VERSION="$VERSION" \
	-DEF_SDL3_SOURCE="$SRC/SDL3-$SDL3_VERSION" \
	-DEF_SDL2COMPAT_SOURCE="$SRC/sdl2-compat-$SDL2COMPAT_VERSION" >/dev/null
cmake --build "$OUT/native" -j"$JOBS" --target main efgame efui SDL2 SDL3-shared

STAGE="$OUT/apk"
rm -rf "$STAGE"
mkdir -p "$STAGE/lib/arm64-v8a" "$STAGE/classes" "$STAGE/dex"
STRIP="$ANDROID_NDK/toolchains/llvm/prebuilt/$(ls "$ANDROID_NDK/toolchains/llvm/prebuilt" | head -1)/bin/llvm-strip"
LIBCXX="$(dirname "$STRIP")/../sysroot/usr/lib/aarch64-linux-android/libc++_shared.so"
for so in libmain.so libefgame.so libefui.so sdl3/libSDL3.so sdl2compat/libSDL2.so; do
	f="$(find "$OUT/native" -path "*/$so" -print -quit)"
	[ -n "$f" ] || { echo "make-apk: $so not built" >&2; exit 1; }
	"$STRIP" --strip-unneeded -o "$STAGE/lib/arm64-v8a/$(basename "$so")" "$f"
done
"$STRIP" --strip-unneeded -o "$STAGE/lib/arm64-v8a/libc++_shared.so" "$LIBCXX"

# ---- Java: SDL 3's activity glue + ours -> classes.dex ----
SDLJAVA="$SRC/SDL3-$SDL3_VERSION/android-project/app/src/main/java"
find "$SDLJAVA" "$ROOT/android/java" -name '*.java' > "$OUT/java-sources.txt"
"$JAVAC" -nowarn -Xlint:-options --release 11 -classpath "$ANDROID_JAR" \
	-d "$STAGE/classes" @"$OUT/java-sources.txt"
"$BT/d8" --release --min-api $MIN_SDK --lib "$ANDROID_JAR" --output "$STAGE/dex" \
	$(find "$STAGE/classes" -name '*.class')

# ---- package ----
UNSIGNED="$OUT/unsigned.apk"
ALIGNED="$OUT/aligned.apk"
APK="$OUT/AwayTeamEF-$VERSION-arm64.apk"
rm -f "$UNSIGNED" "$ALIGNED" "$APK"
"$BT/aapt2" link -o "$UNSIGNED" -I "$ANDROID_JAR" \
	--manifest "$ROOT/android/AndroidManifest.xml" \
	--min-sdk-version $MIN_SDK --target-sdk-version "$TARGET_SDK" \
	--version-code $VERSION_CODE --version-name "$VERSION"
( cd "$STAGE/dex" && zip -q -X "$UNSIGNED" classes.dex )
( cd "$STAGE" && zip -q -X -r "$UNSIGNED" lib )
"$BT/zipalign" -P 16 -f 4 "$UNSIGNED" "$ALIGNED"

if [ ! -f "$KEYSTORE" ]; then
	echo "== making a debug key at $KEYSTORE"
	mkdir -p "$(dirname "$KEYSTORE")"
	"$KEYTOOL" -genkeypair -keystore "$KEYSTORE" -storepass android -keypass android \
		-alias androiddebugkey -keyalg RSA -keysize 2048 -validity 10000 \
		-dname "CN=AwayTeamEF Debug,O=Android,C=US" >/dev/null
fi
"$BT/apksigner" sign --ks "$KEYSTORE" --ks-pass pass:android --key-pass pass:android \
	--ks-key-alias androiddebugkey --out "$APK" "$ALIGNED"
rm -f "$UNSIGNED" "$ALIGNED" "$APK.idsig"

echo "== $APK ($(du -h "$APK" | cut -f1), $(( $(date +%s) - T0 )) s)"
