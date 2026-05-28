#!/usr/bin/env bash
# Build Lynx + PrimJS + LynxBase + LynxServiceAPI iOS xcframeworks
# from the upstream CocoaPods source pods.
#
# Lynx ships only as source pods on CocoaPods (no prebuilt iOS
# binaries on its GitHub Releases), so we set up a tiny "carrier"
# Xcode project, `pod install` the source pods, build for iOS device
# + Simulator with `use_frameworks!` (dynamic linkage so the
# resulting .frameworks contain Mach-O dylibs that ld64 won't
# dead-strip Obj-C categories from), and lift them into xcframeworks.
#
# Outputs (under $OUT_DIR, default ../../target/lynx-ios):
#   Lynx.xcframework/
#   PrimJS.xcframework/
#   LynxBase.xcframework/
#   LynxServiceAPI.xcframework/
#   headers/{Lynx,PrimJS,LynxBase,LynxServiceAPI}/    (.h tree)
#
# Ported from whiskerrs/whisker:xtask/src/ios/build_lynx_frameworks.rs
# so this fork's CI can produce the same artifacts without depending
# on the Whisker repo.

set -euo pipefail

LYNX_VERSION="${LYNX_VERSION:-3.7.0}"
PRIMJS_VERSION="${PRIMJS_VERSION:-3.7.0}"
BUILD_DIR="${BUILD_DIR:-$(pwd)/lynx-build}"
OUT_DIR="${OUT_DIR:-$(pwd)/lynx-ios}"

PODS=(Lynx PrimJS LynxBase LynxServiceAPI)

echo "==> LYNX_VERSION=$LYNX_VERSION PRIMJS_VERSION=$PRIMJS_VERSION"
echo "==> BUILD_DIR=$BUILD_DIR OUT_DIR=$OUT_DIR"

# ----- Clean -----------------------------------------------------------------

echo "==> Clean"
rm -rf "$BUILD_DIR" "$OUT_DIR"
mkdir -p "$BUILD_DIR" "$OUT_DIR"

# ----- Carrier project -------------------------------------------------------

echo "==> Generate carrier Xcode project"
mkdir -p "$BUILD_DIR/Sources"
cat > "$BUILD_DIR/Sources/AppDelegate.swift" <<'EOF'
import UIKit
@UIApplicationMain
class AppDelegate: UIResponder, UIApplicationDelegate {
    var window: UIWindow?
}
EOF

cat > "$BUILD_DIR/project.yml" <<'EOF'
name: LynxCarrier
options:
  bundleIdPrefix: rs.whisker.carrier
  deploymentTarget:
    iOS: '13.0'
targets:
  LynxCarrier:
    type: application
    platform: iOS
    sources: [Sources]
    info:
      path: Info.plist
      properties:
        UILaunchScreen: {}
    settings:
      base:
        PRODUCT_BUNDLE_IDENTIFIER: rs.whisker.carrier.LynxCarrier
EOF

(cd "$BUILD_DIR" && xcodegen generate)

# ----- Podfile + pod install -------------------------------------------------

echo "==> Write Podfile + pod install"

# `use_frameworks!` (no `:linkage =>` → defaults to `:dynamic`) gives
# us `.framework` bundles whose binary is a real Mach-O dylib. Static
# linkage would dead-strip Lynx's Obj-C `__objc_catlist` entries when
# WhiskerDriver later links them in — see the rationale in the
# original xtask source.
cat > "$BUILD_DIR/Podfile" <<EOF
platform :ios, '13.0'
use_frameworks!
target 'LynxCarrier' do
  pod 'Lynx', '$LYNX_VERSION'
  pod 'PrimJS', '$PRIMJS_VERSION', :subspecs => ['quickjs', 'napi']
end
EOF

(cd "$BUILD_DIR" && pod install --repo-update)

# ----- Patch upstream podspec bug --------------------------------------------

# Lynx 3.7.0's published xcconfigs hardcode `HEADER_SEARCH_PATHS`
# values pointing at `/Users/runner/work/lynx/lynx/lynx` (the runner
# path that produced the CocoaPods release). For LynxServiceAPI it's
# the *only* search path, so the build fails outright. Rewrite to
# `${PODS_TARGET_SRCROOT}` so headers resolve against the locally
# extracted pod sources.
echo "==> Patch upstream podspec HEADER_SEARCH_PATHS"
find "$BUILD_DIR/Pods" -name '*.xcconfig' -print0 | \
  xargs -0 grep -l '/Users/runner/work/lynx/lynx/lynx' 2>/dev/null | \
  while read -r f; do
    sed -i.bak \
      's|/Users/runner/work/lynx/lynx/lynx|${PODS_TARGET_SRCROOT}|g' \
      "$f"
    rm -f "$f.bak"
    echo "    patched $(basename "$f")"
  done

# ----- Patch upstream modp_b64 symbol drift ----------------------------------

# Lynx 3.7.0's bundled modp_b64 library prefixes every public symbol
# with `lynx_` (modp_b64.h: `#define modp_b64_decode lynx_modp_b64_decode`
# etc.) to avoid collisions when an embedder also links a stock
# modp_b64. The defines are wired up correctly inside `modp_b64.c`
# itself, but `core/runtime/lepus/bindings/renderer_functions.cc:1332`
# was missed and still calls `modp_b64_decode` directly — which post-
# rename is no longer a visible symbol. The pinned upstream pod ships
# this broken file; clang refuses to link. Patch the one call site
# here so the rest of the xcodebuild step has a chance.
echo "==> Patch modp_b64 symbol drift in renderer_functions.cc"
RF="$BUILD_DIR/Pods/Lynx/core/runtime/lepus/bindings/renderer_functions.cc"
if [ -f "$RF" ]; then
  /usr/bin/perl -i -pe 's/(?<![_A-Za-z0-9])modp_b64_decode\b/lynx_modp_b64_decode/g' "$RF"
  echo "    patched $(basename "$RF")"
fi

# ----- Overlay fork-modified Lynx core sources -------------------------------

# Upstream Lynx 3.7.0 from CocoaPods is what `pod install` just laid
# down under `Pods/Lynx/`. The whisker fork carries a small set of
# additive changes inside `core/renderer/dom/fiber/` that need to ship
# in the iOS xcframework — most notably the `ListNativeItemProvider`
# mechanism (whiskerrs/lynx#9) gating the `<list>` native-driver path
# Whisker drives. Copy each modified file in to override the
# corresponding pod source before xcodebuild compiles it.
#
# This overlay approach is the minimum invasion that keeps the
# upstream CocoaPods source as the base and only swaps the files the
# fork actually touches. Doing the full `cocoapods_publish_helper.py`
# `--prepare-source` / `--publish_local` flow would buy us a clean
# `pod 'Lynx', '3.7.0-whisker.X'` resolution, but requires
# `geniospkg` (Bytedance-internal) and a GN tree set up under
# `tools_shared/` — neither of which is available on github-hosted
# macos-14 runners.
echo "==> Overlay fork-modified Lynx sources"
FORK_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
for rel in \
    core/renderer/dom/fiber/list_element.h \
    core/renderer/dom/fiber/list_element.cc; do
  src="$FORK_ROOT/$rel"
  dst="$BUILD_DIR/Pods/Lynx/$rel"
  if [ ! -f "$src" ]; then
    echo "::error::fork source missing: $src"
    exit 1
  fi
  if [ ! -f "$dst" ]; then
    echo "::error::upstream pod source missing: $dst (did the layout change?)"
    exit 1
  fi
  cp -v "$src" "$dst"
done

# ----- xcodebuild for device + simulator -------------------------------------

XCODE_COMMON=(
  -workspace LynxCarrier.xcworkspace
  -scheme LynxCarrier
  -configuration Release
  SKIP_INSTALL=NO
  ONLY_ACTIVE_ARCH=NO
  CODE_SIGNING_ALLOWED=NO
  CODE_SIGNING_REQUIRED=NO
  CODE_SIGN_IDENTITY=
)

echo "==> xcodebuild iOS device"
(cd "$BUILD_DIR" && \
  xcodebuild build "${XCODE_COMMON[@]}" \
    -destination 'generic/platform=iOS' \
    -derivedDataPath build/device)

echo "==> xcodebuild iOS Simulator"
(cd "$BUILD_DIR" && \
  xcodebuild build "${XCODE_COMMON[@]}" \
    -destination 'generic/platform=iOS Simulator' \
    -derivedDataPath build/sim)

DEV_PRODUCTS="$BUILD_DIR/build/device/Build/Products/Release-iphoneos"
SIM_PRODUCTS="$BUILD_DIR/build/sim/Build/Products/Release-iphonesimulator"

# ----- Create xcframeworks ---------------------------------------------------

echo "==> Create xcframeworks"
for fw in "${PODS[@]}"; do
  DEV_FW="$DEV_PRODUCTS/$fw/$fw.framework"
  SIM_FW="$SIM_PRODUCTS/$fw/$fw.framework"
  if [ ! -d "$DEV_FW" ] || [ ! -d "$SIM_FW" ]; then
    echo "::warning::missing framework for $fw — dev=$DEV_FW sim=$SIM_FW"
    continue
  fi
  XCF="$OUT_DIR/$fw.xcframework"
  rm -rf "$XCF"
  xcodebuild -create-xcframework \
    -framework "$DEV_FW" \
    -framework "$SIM_FW" \
    -output "$XCF"
  echo "    ✅ $fw.xcframework"
done

# ----- Stage C++ headers -----------------------------------------------------

echo "==> Stage Lynx C++ headers"
mkdir -p "$OUT_DIR/headers"
for pod in "${PODS[@]}"; do
  POD_SRC="$BUILD_DIR/Pods/$pod"
  if [ ! -d "$POD_SRC" ]; then
    echo "::warning::Pods/$pod missing — skipping header stage"
    continue
  fi
  POD_DST="$OUT_DIR/headers/$pod"
  mkdir -p "$POD_DST"
  # Find every `.h` under the pod source and mirror the directory
  # structure into the staged tree. cc-rs's bridge build expects
  # `<staged>/<Pod>/<rel-path>/<header>.h`.
  (cd "$POD_SRC" && find . -name '*.h' -print0) | \
    tar -cf - --null --files-from=- -C "$POD_SRC" | \
    (cd "$POD_DST" && tar -xf -)
  COUNT=$(find "$POD_DST" -name '*.h' | wc -l | tr -d ' ')
  echo "    $pod: $COUNT header(s)"
done

# ----- Done ------------------------------------------------------------------

echo ""
echo "==> Final outputs:"
ls -1 "$OUT_DIR"
echo ""
echo "    xcframeworks → $OUT_DIR/*.xcframework"
echo "    headers      → $OUT_DIR/headers/<pod>/"
