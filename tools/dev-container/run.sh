#!/bin/sh
# Runs a command in the hassmic-dev image (Dockerfile here) on this checkout, mounted at /src:
#   tools/dev-container/run.sh 'make -j8 DEVICE=donut STUBS=1 all && make unit'
#   tools/dev-container/run.sh tools/dev-container/suite.sh        everything CI runs
# Device binaries land in build/<codename>/ of the checkout as with a local build.  Git Bash on Windows works.
cd "$(dirname "$0")/../.."
dir=$(pwd -W 2>/dev/null || pwd)
MSYS_NO_PATHCONV=1 exec docker run --rm -v "$dir:/src" -v hassmic-venv:/src/.venv -w /src hassmic-dev bash -c "$*"
