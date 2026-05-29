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

LYNX_VERSION="${LYNX_VERSION:-3.8.0}"
PRIMJS_VERSION="${PRIMJS_VERSION:-3.8.0}"
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
#
# **Lynx comes from the fork tree directly** via `:path => '$FORK_ROOT'`.
# The Lynx.podspec.json at the fork root mirrors upstream 3.7.0's
# podspec but points all `source_files` / `*_header_files` globs at
# the fork tree and adds the `core/native_renderer_capi/` entries
# (which upstream doesn't ship). This lets every fork addition —
# new virtuals on `element.h`, fork-only call sites in
# `list_element.cc`, the `lynx_list_set_native_item_provider` capi,
# the styling pipeline changes — compile in cleanly without overlay,
# without `LYNX_WHISKER_UPSTREAM_307_COMPAT` gates, and without the
# modp_b64 / HEADER_SEARCH_PATHS drift patches that used to be
# necessary against upstream's broken release pod.
#
# `PrimJS` is still upstream (whiskerrs/whisker#87 tracks dropping
# it altogether once Lynx has a build flag for lepus exclusion).
FORK_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cat > "$BUILD_DIR/Podfile" <<EOF
platform :ios, '13.0'
use_frameworks!
target 'LynxCarrier' do
  # All three of Lynx / LynxBase / LynxServiceAPI come from the
  # fork tree via the local Lynx.podspec.json / LynxBase.podspec.json
  # / LynxServiceAPI.podspec.json checked in at the fork root.
  # Otherwise CocoaPods would resolve LynxBase / LynxServiceAPI from
  # Trunk (upstream 3.7.0 zip) and we'd be right back in the overlay
  # business.
  pod 'Lynx',          :path => '$FORK_ROOT'
  pod 'LynxBase',      :path => '$FORK_ROOT'
  pod 'LynxServiceAPI', :path => '$FORK_ROOT'
  pod 'PrimJS', '$PRIMJS_VERSION', :subspecs => ['quickjs', 'napi']
end
EOF

(cd "$BUILD_DIR" && pod install --repo-update)

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
