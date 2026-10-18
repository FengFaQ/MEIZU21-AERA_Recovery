#!/usr/bin/env bash
# Rebuild the pinned source with Rust 1.98.1+ and Android NDK r26d+.
# Dependencies are pinned in Cargo.lock. Run `cargo fetch --locked` in source/
# once when preparing a new toolchain; recovery builds never fetch or use Cargo.
set -euo pipefail
: "${ANDROID_NDK_ROOT:?Set ANDROID_NDK_ROOT to the Android NDK}"
tool_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ndk_bin="$ANDROID_NDK_ROOT/toolchains/llvm/prebuilt/linux-x86_64/bin"
export CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER="$ndk_bin/aarch64-linux-android28-clang"
export CC_aarch64_linux_android="$CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER"
export AR_aarch64_linux_android="$ndk_bin/llvm-ar"
export LZMA_API_STATIC=1
cargo build --manifest-path "$tool_dir/source/Cargo.toml" --release --locked --offline \
    --target-dir "$tool_dir/source/target" --no-default-features --target aarch64-linux-android
mkdir -p "$tool_dir/prebuilt/arm64"
cp "$tool_dir/source/target/aarch64-linux-android/release/otaripper" "$tool_dir/prebuilt/arm64/aera-otaripper"
sha256sum "$tool_dir/prebuilt/arm64/aera-otaripper"
