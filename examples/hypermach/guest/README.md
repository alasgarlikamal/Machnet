# Build
RUSTFLAGS="-C panic=abort -C code-model=small -C link-args=-eentrypoint" cargo build -r --target x86_64-unknown-none