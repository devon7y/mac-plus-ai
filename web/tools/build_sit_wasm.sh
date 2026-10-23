#!/bin/zsh
# Build the StuffIt decoder for the page (web/sit_wasm -> web/build/sit_wasm/sit.wasm). Uses a Rust
# toolchain kept inside web/build/rust (rustup, minimal profile, wasm32-unknown-unknown); installs it
# on first use. Cargo's caches stay in web/build too.
set -e
cd "$(dirname "$0")/.."
export RUSTUP_HOME=$PWD/build/rust/rustup CARGO_HOME=$PWD/build/rust/cargo CARGO_TARGET_DIR=$PWD/build/sit_wasm_target
if [ ! -x build/rust/cargo/bin/cargo ]; then
  mkdir -p build/rust
  curl -sSf https://sh.rustup.rs -o build/rust/rustup-init.sh
  sh build/rust/rustup-init.sh -y --profile minimal --default-toolchain stable --target wasm32-unknown-unknown --no-modify-path
fi
build/rust/cargo/bin/cargo build --manifest-path sit_wasm/Cargo.toml --release --target wasm32-unknown-unknown
mkdir -p build/sit_wasm
cp build/sit_wasm_target/wasm32-unknown-unknown/release/sit_wasm.wasm build/sit_wasm/sit.wasm
cp "$(ls -d build/rust/cargo/registry/src/*/stuffit-0.4.0 | head -1)/LICENSE-MIT" build/sit_wasm/LICENSE-MIT
ls -l build/sit_wasm/sit.wasm
