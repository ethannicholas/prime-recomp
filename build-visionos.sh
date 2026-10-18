#!/bin/sh
# Build the Vision Pro app, and with --install, install it on a paired Vision Pro.
#
#   ./build-visionos.sh                 configure (first run) and build build-visionos/
#   ./build-visionos.sh --install       ...then install it on the Vision Pro
#   ./build-visionos.sh --push-image    ...then copy rom/'s disc image into the app
#
# Signing uses the Apple development team in $GCN_APPLE_TEAM (default Y3GFBT2WQ2); Xcode
# makes the provisioning profile. The Vision Pro must be paired with this Mac (Xcode >
# Devices and Simulators) and in developer mode. $GCN_DEVICE names it when more than one
# device is paired.
set -e
cd "$(dirname "$0")"
if [ ! -f gcn-recomp/CMakeLists.txt ]; then
  git submodule update --init
fi
export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
TEAM=${GCN_APPLE_TEAM:-Y3GFBT2WQ2}
BUNDLE=com.ethannicholas.prime
if [ ! -d build-visionos/prime.xcodeproj ]; then
  cmake -S . -B build-visionos -G Xcode -DCMAKE_SYSTEM_NAME=visionOS \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=26.0 -DGCN_APPLE_TEAM="$TEAM"
fi
xcodebuild -project build-visionos/prime.xcodeproj -target prime -configuration RelWithDebInfo \
  -sdk xros -allowProvisioningUpdates -quiet build
APP=build-visionos/RelWithDebInfo-xros/prime.app
echo "built $APP"

device() {
  if [ -n "$GCN_DEVICE" ]; then echo "$GCN_DEVICE"; return; fi
  xcrun devicectl list devices --hide-headers --hide-default-columns --columns Identifier --columns Model \
    | awk '/RealityDevice|Vision/ {print $1; exit}'
}

for arg in "$@"; do
  case "$arg" in
  --install)
    DEV=$(device)
    [ -n "$DEV" ] || { echo "no Vision Pro paired" >&2; exit 1; }
    xcrun devicectl device install app --device "$DEV" "$APP"
    ;;
  --push-image)
    DEV=$(device)
    [ -n "$DEV" ] || { echo "no Vision Pro paired" >&2; exit 1; }
    IMG=$(ls rom/*.iso rom/*.ciso 2>/dev/null | head -1)
    [ -n "$IMG" ] || { echo "no disc image in rom/" >&2; exit 1; }
    EXT=${IMG##*.}
    xcrun devicectl device copy to --device "$DEV" --domain-type appDataContainer \
      --domain-identifier "$BUNDLE" --source "$IMG" --destination "Documents/game.$EXT"
    ;;
  esac
done
