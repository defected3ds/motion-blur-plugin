#!/bin/bash
# Build VectorMotionBlur.plugin (universal arm64+x86_64) on macOS.
# Usage: scripts/build_mac.sh /path/to/AfterEffectsSDK
# Needs: Xcode command line tools (clang++, Rez, codesign). The AE SDK comes from Adobe.
set -euo pipefail
SDK="${1:?usage: $0 /path/to/AfterEffectsSDK}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/build-mac"
PLUGIN="$OUT/VectorMotionBlur.plugin"
INC=(-I"$SDK/Examples/Headers" -I"$SDK/Examples/Headers/SP" -I"$SDK/Examples/Resources" -I"$SDK/Examples/Util" -I"$ROOT/core")

[ -d "$SDK/Examples/Headers" ] || { echo "AE SDK headers not found under $SDK/Examples/Headers"; exit 1; }
rm -rf "$OUT"; mkdir -p "$PLUGIN/Contents/MacOS" "$PLUGIN/Contents/Resources"

clang++ -std=c++17 -O3 -fvisibility=hidden -fopenmp-simd -arch arm64 -arch x86_64 \
  -mmacosx-version-min=11.0 -DMACOS -DAE_OS_MAC "${INC[@]}" \
  -bundle -o "$PLUGIN/Contents/MacOS/VectorMotionBlur" \
  "$ROOT/ae/MotionBlur.cpp" "$ROOT/core/motion_blur.cpp" \
  -framework CoreFoundation

# PiPL resource
/usr/bin/Rez -useDF -define __MACH__ -d AE_OS_MAC -d AE_PROC_INTELx64 \
  "${INC[@]}" -i "$SDK/Examples/Resources" -isysroot "$(xcrun --show-sdk-path)" \
  "$ROOT/ae/MotionBlur.r" -o "$PLUGIN/Contents/Resources/VectorMotionBlur.rsrc"

cp "$ROOT/mac/Info.plist" "$PLUGIN/Contents/Info.plist"
printf 'eFKTFXTC' > "$PLUGIN/Contents/PkgInfo"
codesign --force --deep -s - "$PLUGIN"   # ad-hoc signature (required on Apple Silicon)

echo "Built: $PLUGIN"
echo "Install: cp -R \"$PLUGIN\" \"/Library/Application Support/Adobe/Common/Plug-ins/7.0/MediaCore/\""
echo "(then restart After Effects; effect is under Effects > Blur & Sharpen > Vector Motion Blur)"
