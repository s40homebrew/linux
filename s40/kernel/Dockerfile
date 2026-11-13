FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends build-essential gcc-arm-linux-gnueabi binutils-arm-linux-gnueabi bc bison flex libssl-dev libelf-dev python3 git xz-utils file device-tree-compiler && rm -rf /var/lib/apt/lists/*
WORKDIR /work
