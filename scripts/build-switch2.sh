#!/usr/bin/env bash
set -euo pipefail

export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
export DEVKITA64="${DEVKITA64:-$DEVKITPRO/devkitA64}"
export PATH="$DEVKITPRO/tools/bin:$DEVKITA64/bin:$DEVKITPRO/portlibs/switch2/bin:$DEVKITPRO/portlibs/switch/bin:$PATH"
export PKG_CONFIG_PATH="$DEVKITPRO/portlibs/switch2/lib/pkgconfig:$DEVKITPRO/portlibs/switch/lib/pkgconfig"

cd "$(dirname "$0")/.."
ROOT_DIR="$(pwd)"

if [[ ! -f "$DEVKITPRO/libounce/cmake/Switch2.cmake" ]]; then
  echo "ERROR: Switch2.cmake not found at $DEVKITPRO/libounce/cmake/Switch2.cmake" >&2
  exit 1
fi

apply_patch_if_needed() {
  local patch_file="$1"
  if git -C "$ROOT_DIR/library/borealis" apply --reverse --check "$patch_file" >/dev/null 2>&1; then
    echo "Patch already applied: $(basename "$patch_file")"
  else
    git -C "$ROOT_DIR/library/borealis" apply "$patch_file"
    echo "Applied patch: $(basename "$patch_file")"
  fi
}

echo "==> Applying Borealis patches..."
apply_patch_if_needed "$ROOT_DIR/scripts/patches/borealis-fixes.patch"
apply_patch_if_needed "$ROOT_DIR/scripts/patches/borealis-switch2.patch"

BUILD_DIR="$ROOT_DIR/build/switch2"
FFMPEG_VER="7.1"
FFMPEG_URL="https://ffmpeg.org/releases/ffmpeg-${FFMPEG_VER}.tar.xz"
FFMPEG_SHA256="40973d44970dbc83ef302b0609f2e74982be2d85916dd2ee7472d30678a7abe6"
FFMPEG_PATCH_BASE="https://raw.githubusercontent.com/devkitPro/pacman-packages/5cf3a4da7665a04032565a3e788656cb485dd279/switch/ffmpeg"
FFMPEG_PATCH_FALLBACK="https://raw.githubusercontent.com/devkitPro/pacman-packages/master/switch/ffmpeg"
FFMPEG_PATCH_SHA256="1792380b992e3554a4abcddf0d7b395bfd8c118ac7c6e38c8f2fb0d39753a390"
TLS_PATCH_SHA256="57ea7ec8ed26d13d3172d0fd589b7883d22f0c180d50b7434fcc73fb2b3ab7d7"

FFMPEG_WORK="$BUILD_DIR/ffmpeg-src"
FFMPEG_BUILD="$BUILD_DIR/ffmpeg-build"
FFMPEG_PREFIX="$BUILD_DIR/ffmpeg-install"
JOBS="${GMCA_BUILD_JOBS:-$(nproc)}"

fetch_verified() {
  local url="$1"
  local fallback="$2"
  local dest="$3"
  local expected_sha="$4"
  if [[ -f "$dest" ]] && echo "${expected_sha}  ${dest}" | sha256sum -c - >/dev/null 2>&1; then
    return 0
  fi
  rm -f "$dest"
  if ! curl -fsSL -o "$dest" "$url"; then
    if [[ -n "$fallback" ]]; then
      curl -fsSL -o "$dest" "$fallback"
    else
      return 1
    fi
  fi
  echo "${expected_sha}  ${dest}" | sha256sum -c -
}

# Build clean FFmpeg 7.1 archives for Switch 2 (same Horizon + tls_libnx ABI
# expected by official devkitPro switch-libmpv, compiled against libounce with --disable-nvtegra).
if [[ ! -f "$FFMPEG_PREFIX/lib/libavcodec.a" ]] || [[ ! -f "$FFMPEG_PREFIX/lib/libavutil.a" ]] || [[ ! -f "$FFMPEG_PREFIX/lib/libavfilter.a" ]]; then
  echo "==> Building clean Switch 2 FFmpeg ${FFMPEG_VER} (--disable-nvtegra)..."
  rm -rf "$FFMPEG_WORK" "$FFMPEG_BUILD"
  mkdir -p "$FFMPEG_WORK" "$FFMPEG_BUILD" "$FFMPEG_PREFIX"

  fetch_verified "$FFMPEG_URL" "" "$FFMPEG_WORK/ffmpeg-${FFMPEG_VER}.tar.xz" "$FFMPEG_SHA256"
  fetch_verified "${FFMPEG_PATCH_BASE}/ffmpeg-${FFMPEG_VER}.patch" "${FFMPEG_PATCH_FALLBACK}/ffmpeg-${FFMPEG_VER}.patch" "$FFMPEG_WORK/ffmpeg-${FFMPEG_VER}.patch" "$FFMPEG_PATCH_SHA256"
  fetch_verified "${FFMPEG_PATCH_BASE}/tls.patch" "${FFMPEG_PATCH_FALLBACK}/tls.patch" "$FFMPEG_WORK/tls.patch" "$TLS_PATCH_SHA256"

  tar -xJf "$FFMPEG_WORK/ffmpeg-${FFMPEG_VER}.tar.xz" -C "$FFMPEG_WORK"
  FFMPEG_SRC="$FFMPEG_WORK/ffmpeg-${FFMPEG_VER}"
  patch -d "$FFMPEG_SRC" -Np1 -i "$FFMPEG_WORK/ffmpeg-${FFMPEG_VER}.patch"
  patch -d "$FFMPEG_SRC" -Np1 -i "$FFMPEG_WORK/tls.patch"
  chmod +x "$FFMPEG_SRC/configure"
  find "$FFMPEG_SRC" -name '*.sh' -exec chmod +x {} +

  (
    cd "$FFMPEG_BUILD"
    bash "$FFMPEG_SRC/configure" \
      --prefix="$FFMPEG_PREFIX" \
      --enable-gpl \
      --disable-shared \
      --enable-static \
      --cross-prefix=aarch64-none-elf- \
      --enable-cross-compile \
      --arch=aarch64 \
      --cpu=cortex-a78c \
      --target-os=horizon \
      --enable-pic \
      --extra-cflags="-D__SWITCH__ -D__SWITCH2__ -D_GNU_SOURCE -O2 -mcpu=cortex-a78c+crypto -mtp=soft -fPIC -ftls-model=local-exec -I${DEVKITPRO}/libounce/include -I${DEVKITPRO}/portlibs/switch2/include -I${DEVKITPRO}/portlibs/switch/include" \
      --extra-cxxflags="-D__SWITCH__ -D__SWITCH2__ -D_GNU_SOURCE -O2 -mcpu=cortex-a78c+crypto -mtp=soft -fPIC -ftls-model=local-exec -I${DEVKITPRO}/libounce/include -I${DEVKITPRO}/portlibs/switch2/include -I${DEVKITPRO}/portlibs/switch/include" \
      --extra-ldflags="-fPIE -L${DEVKITPRO}/portlibs/switch2/lib -L${DEVKITPRO}/portlibs/switch/lib -L${DEVKITPRO}/libounce/lib" \
      --disable-runtime-cpudetect \
      --disable-programs \
      --disable-debug \
      --disable-doc \
      --disable-autodetect \
      --enable-asm \
      --enable-neon \
      --disable-avdevice \
      --disable-encoders \
      --disable-muxers \
      --enable-avfilter \
      --enable-swscale \
      --enable-swresample \
      --enable-network \
      --disable-protocols \
      --enable-protocol=file,http,https,ftp,tcp,udp,rtmp,rtmpt,hls,crypto,tls,httpproxy \
      --enable-zlib \
      --enable-bzlib \
      --enable-libass \
      --enable-libfreetype \
      --enable-libfribidi \
      --enable-libdav1d \
      --enable-libnx \
      --disable-nvtegra
    make -j"$JOBS"
    make install
  )
  rm -rf "$FFMPEG_WORK" "$FFMPEG_BUILD"
fi

echo "==> Configuring GMCA for Nintendo Switch 2..."
export PKG_CONFIG_PATH="$FFMPEG_PREFIX/lib/pkgconfig:$PKG_CONFIG_PATH"
CMAKE_EXTRA_ARGS=()
if [[ -n "${GMCA_VERSION_BUILD:-}" ]]; then
  CMAKE_EXTRA_ARGS+=("-DVERSION_BUILD=${GMCA_VERSION_BUILD}")
fi
cmake -B "$BUILD_DIR" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/libounce/cmake/Switch2.cmake" \
  -DCMAKE_BUILD_TYPE=Release \
  -DPLATFORM_SWITCH=ON \
  -DNINTENDO_SWITCH2=ON \
  -DBUILTIN_NSP=OFF \
  -DUSE_LIBUSBHSFS=OFF \
  -DSWITCH2_FFMPEG_DIR="$FFMPEG_PREFIX" \
  "${CMAKE_EXTRA_ARGS[@]}"

echo "==> Building GMCA-switch2.nro..."
cmake --build "$BUILD_DIR" --target GMCA.nro -j"$JOBS"

python3 "$ROOT_DIR/tools/verify_switch2_package.py" \
  "$BUILD_DIR/GMCA-switch2.nro" \
  --link-map "$BUILD_DIR/GMCA-switch2.map"

cp -f "$BUILD_DIR/GMCA-switch2.nro" "$BUILD_DIR/GMCA.nro"
printf '\nBuilt: %s\n' "$BUILD_DIR/GMCA-switch2.nro"
