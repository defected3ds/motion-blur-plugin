#!/bin/bash
# Double-click (or run) to build the `mbvideo` tool on macOS. Needs: Xcode command line tools
# (xcode-select --install) and ffmpeg (brew install ffmpeg).
set -euo pipefail
cd "$(dirname "$0")"
FLAGS=(-std=c++17 -O3 -arch arm64 -arch x86_64 -Icore)
# Optional multi-threading if Homebrew libomp is installed (brew install libomp).
if [ -d "$(brew --prefix libomp 2>/dev/null || echo /nonexistent)" ]; then
  P="$(brew --prefix libomp)"
  FLAGS+=(-Xpreprocessor -fopenmp -I"$P/include" -L"$P/lib" -lomp)
  echo "Using OpenMP from $P"
fi
clang++ "${FLAGS[@]}" cli/mbvideo.cpp core/motion_blur.cpp -o mbvideo
echo
echo "Built ./mbvideo   Try:  ./mbvideo input.mp4 output.mp4"
