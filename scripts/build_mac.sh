#!/bin/bash
# Build VectorMotionBlur.plugin on macOS using Adobe's own Skeleton Xcode project with our source swapped in.
# Usage: scripts/build_mac.sh /path/to/AfterEffectsSDK
# Needs the full Xcode app (xcodebuild) and the Adobe After Effects SDK (27.x tested against headers).
set -euo pipefail
echo "build_mac.sh v3 (Xcode project build) running from: $(cd "$(dirname "$0")/.." && pwd)"
SDK="$(cd "${1:?usage: $0 /path/to/AfterEffectsSDK}" && pwd)"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/build-mac"
EX="$SDK/Examples"
[ -d "$EX/Template/Skeleton/Mac/Skeleton.xcodeproj" ] || { echo "Not an After Effects SDK folder (missing Examples/Template/Skeleton): $SDK"; exit 1; }
command -v xcodebuild >/dev/null || { echo "xcodebuild not found. Install the full Xcode app from the App Store."; exit 1; }

# Stage a private copy so the SDK folder is never modified. Adobe's project uses ../../../Headers etc.
rm -rf "$OUT"; W="$OUT/work/Examples"; mkdir -p "$W/Template" "$OUT/products"
for d in Headers Util Resources; do ln -s "$EX/$d" "$W/$d"; done
cp -R "$EX/Template/Skeleton" "$W/Template/Skeleton"
S="$W/Template/Skeleton"
cp "$ROOT/ae/MotionBlur.cpp" "$S/Skeleton.cpp"
cp "$ROOT/ae/MotionBlur.r"   "$S/SkeletonPiPL.r"
cp "$ROOT/core/motion_blur.h" "$ROOT/core/motion_blur.cpp" "$S/"
printf '\n#include "motion_blur.cpp"\n' >> "$S/Skeleton.cpp"      # build the core as part of the same unit
sed -i '' 's/<string>Skeleton<\/string>/<string>VectorMotionBlur<\/string>/' "$S/Mac/Skeleton.plugin-Info.plist" 2>/dev/null \
  || sed -i 's/<string>Skeleton<\/string>/<string>VectorMotionBlur<\/string>/' "$S/Mac/Skeleton.plugin-Info.plist"

( cd "$S/Mac" && xcodebuild -project Skeleton.xcodeproj -target Skeleton -configuration Release \
    ARCHS="arm64 x86_64" ONLY_ACTIVE_ARCH=NO PRODUCT_NAME=VectorMotionBlur \
    PRODUCT_BUNDLE_IDENTIFIER=com.pipelife.vectormotionblur SYMROOT="$OUT/products" \
    CODE_SIGN_IDENTITY="-" build )

P="$(find "$OUT/products" -name VectorMotionBlur.plugin -type d | head -1)"
[ -n "$P" ] || { echo "Build finished but no .plugin was produced."; exit 1; }
cp -R "$P" "$OUT/VectorMotionBlur.plugin"
xattr -cr "$OUT/VectorMotionBlur.plugin" || true
codesign --force --deep -s - "$OUT/VectorMotionBlur.plugin"

echo
echo "Built: $OUT/VectorMotionBlur.plugin"
echo "Install: cp -R \"$OUT/VectorMotionBlur.plugin\" \"/Library/Application Support/Adobe/Common/Plug-ins/7.0/MediaCore/\""
echo "Then restart After Effects: Effects > Blur & Sharpen > Vector Motion Blur"
