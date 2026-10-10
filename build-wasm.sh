#!/usr/bin/env bash
# Build the RajSQL WebAssembly module.
# Intended to run INSIDE the emscripten/emsdk docker image, with the repo
# root mounted at /src (see build.sh wasm).
set -euo pipefail
cd "$(dirname "$0")"

em++ -std=c++17 -O2 -fexceptions \
  src/value.cpp \
  src/lexer.cpp \
  src/parser.cpp \
  src/pager.cpp \
  src/wal.cpp \
  src/bptree.cpp \
  src/database.cpp \
  src/capi.cpp \
  -o web/rajsql.js \
  -s WASM=1 \
  -s MODULARIZE=1 \
  -s EXPORT_ES6=1 \
  -s EXPORT_NAME='RajSQL' \
  -s "EXPORTED_RUNTIME_METHODS=['FS','ccall','cwrap']" \
  -s EXPORTED_FUNCTIONS='_rsql_open,_rsql_exec,_rsql_schema,_rsql_crash_demo' \
  -s ALLOW_MEMORY_GROWTH=1 \
  -s ENVIRONMENT='web' \
  -lidbfs.js

echo "WASM build complete: web/rajsql.js + web/rajsql.wasm"
