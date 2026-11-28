#!/usr/bin/env bash
# Builds forma. Usage: ./build.sh [clean] [debug] [test] [install [PREFIX]] [run [ARGS...]]
#   clean    wipe the build directory first
#   debug    Debug build (default: RelWithDebInfo)
#   test     run the tests after building
#   install  install to PREFIX (default: ~/.local, no sudo needed)
#   run      start forma after building; everything after "run" is passed to it
set -euo pipefail
cd "$(dirname "$0")"

BUILD_DIR=build
TYPE=RelWithDebInfo
CLEAN=0 TEST=0 INSTALL=0 RUN=0 PREFIX="$HOME/.local"
while [ $# -gt 0 ]; do
  case "$1" in
    clean) CLEAN=1 ;;
    debug) TYPE=Debug ;;
    test) TEST=1 ;;
    install) INSTALL=1
             case "${2:-}" in ""|clean|debug|test|run|-h|--help) ;; *) PREFIX="$(realpath -m "$2")"; shift ;; esac ;;
    run) RUN=1; shift; break ;;
    -h|--help) sed -n '2,7p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option: $1 (see ./build.sh --help)" >&2; exit 1 ;;
  esac
  shift
done

if [ "$CLEAN" = 1 ]; then rm -rf "$BUILD_DIR"; fi
cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="$TYPE" -DCMAKE_INSTALL_PREFIX="$PREFIX"
cmake --build "$BUILD_DIR" -j"$(nproc)"
if [ "$TEST" = 1 ]; then
  ctest --test-dir "$BUILD_DIR" --output-on-failure
fi
if [ "$INSTALL" = 1 ]; then cmake --install "$BUILD_DIR"; fi
if [ "$RUN" = 1 ]; then
  exec "./$BUILD_DIR/forma" "$@"
fi
