#!/bin/sh
set -eu

# Install build tools and the two development libraries used by the client.
if [ "$(id -u)" -eq 0 ]; then
    SUDO=""
elif command -v sudo >/dev/null 2>&1; then
    SUDO="sudo"
else
    echo "Run this script as root or install sudo first." >&2
    exit 1
fi

if ! command -v apt-get >/dev/null 2>&1; then
    echo "This installer currently supports Debian/antiX systems using apt-get." >&2
    echo "Install: build-essential cmake pkg-config libcurl4-openssl-dev libjson-c-dev" >&2
    exit 1
fi

$SUDO apt-get update
$SUDO apt-get install -y \
    build-essential \
    cmake \
    pkg-config \
    libcurl4-openssl-dev \
    libjson-c-dev

echo "Dependencies installed. Run ./make.sh to build Coarena."
