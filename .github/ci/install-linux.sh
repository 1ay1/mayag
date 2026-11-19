#!/bin/sh
# install-linux.sh gcc|clang : the C++26 toolchain mayag's CI needs on
# ubuntu-24.04. jaal requires P1061 (structured binding packs), first in
# GCC 16 and clang 22; neither ships in the runner image.
#   gcc   -> g++-16 from ppa:ubuntu-toolchain-r/test
#   clang -> clang-22 from apt.llvm.org, with libstdc++-16 headers
# apt mirrors flake, so every network step retries.
set -eu
retry() {
    for i in 1 2 3 4 5; do
        "$@" && return 0
        echo "attempt $i failed: $*; retrying in 20s" >&2
        sleep 20
    done
    return 1
}
SUDO=""
[ "$(id -u)" -ne 0 ] && SUDO=sudo

retry $SUDO add-apt-repository -y ppa:ubuntu-toolchain-r/test
retry $SUDO apt-get -o Acquire::Retries=5 update
case "${1:-gcc}" in
    gcc)
        retry $SUDO apt-get -o Acquire::Retries=5 install -y --no-install-recommends g++-16 ninja-build
        ;;
    clang)
        retry $SUDO apt-get -o Acquire::Retries=5 install -y --no-install-recommends libstdc++-16-dev ninja-build wget
        retry wget -qO /tmp/llvm.sh https://apt.llvm.org/llvm.sh
        chmod +x /tmp/llvm.sh
        retry $SUDO /tmp/llvm.sh 22
        ;;
    *)
        echo "usage: install-linux.sh gcc|clang" >&2
        exit 2
        ;;
esac
