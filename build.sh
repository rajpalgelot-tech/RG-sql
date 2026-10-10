#!/usr/bin/env bash
# RajSQL build helper.
#
#   ./build.sh        build natively (inside docker, no local toolchain needed),
#                     then run the test suite
#   ./build.sh wasm   build the WebAssembly module inside the emscripten image
#
# Prerequisite: Docker.
set -euo pipefail
cd "$(dirname "$0")"

MODE="${1:-native}"

if [ "$MODE" = "wasm" ]; then
  docker run --rm -v "$(pwd)":/src -w /src emscripten/emsdk:latest bash build-wasm.sh
  exit 0
fi

if [ "$MODE" != "native" ]; then
  echo "usage: ./build.sh [native|wasm]" >&2
  exit 1
fi

docker run --rm -v "$(pwd)":/src -w /src frolvlad/alpine-gxx sh -c '
  set -eu
  echo "==> building rajsql_tests"
  g++ -std=c++17 -O2 -Wall -Wextra -Isrc \
    tests/test_all.cpp \
    src/value.cpp src/lexer.cpp src/parser.cpp src/pager.cpp \
    src/wal.cpp src/bptree.cpp src/database.cpp \
    -o rajsql_tests
  echo "==> building rajsql_cli"
  g++ -std=c++17 -O2 -Wall -Wextra -Isrc \
    src/cli/main.cpp \
    src/value.cpp src/lexer.cpp src/parser.cpp src/pager.cpp \
    src/wal.cpp src/bptree.cpp src/database.cpp \
    -o rajsql_cli
  echo "==> running tests"
  ./rajsql_tests
'
