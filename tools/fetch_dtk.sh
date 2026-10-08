#!/bin/sh
# Downloads decomp-toolkit (dtk), used to (re)generate analysis/symbols.txt.
cd "$(dirname "$0")"
case "$(uname -s)-$(uname -m)" in
  Darwin-arm64) asset=dtk-macos-arm64 ;;
  Darwin-x86_64) asset=dtk-macos-x86_64 ;;
  Linux-x86_64) asset=dtk-linux-x86_64 ;;
  *) echo "unsupported host; see https://github.com/encounter/decomp-toolkit/releases"; exit 1 ;;
esac
curl -sL -o dtk "https://github.com/encounter/decomp-toolkit/releases/latest/download/$asset" && chmod +x dtk
