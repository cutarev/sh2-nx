#!/bin/sh
# Regenerates src/recomp/gen from game/sh2pc.exe with the xboxrecomp pipeline (third_party/xboxrecomp).
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
X=$ROOT/third_party/xboxrecomp
PY=$ROOT/.venv/bin/python3
B=$ROOT/build
mkdir -p "$B"
[ -z "$TRACE" ] && [ -f "$B/trace.json" ] && TRACE="$B/trace.json"  # oracle targets (tools/oracle.py)
cd "$ROOT"
if [ "$1" != --recomp-only ]; then
# Enhanced Edition code patches (tools/patch_exe.py), then the PE as an XBE for the pipeline.
"$PY" tools/patch_exe.py game/sh2pc.exe "$B/sh2pc_ee.exe"
"$PY" tools/pe2xbe.py "$B/sh2pc_ee.exe" "$B/sh2pc.xbe" --imports "$B/imports.json"
cd "$X"
"$PY" -m tools.xbe_parser "$B/sh2pc.xbe" --json "$B/sh2pc_analysis.json" --quiet
# Pass 1 finds the code; its immediates and data pointers seed the functions only data refers to.
"$PY" -m tools.disasm "$B/sh2pc.xbe" --text-only -o "$B/disasm" --force > "$B/disasm.log" 2>&1
(cd "$ROOT" && "$PY" tools/seeds.py)
"$PY" -m tools.disasm "$B/sh2pc.xbe" --text-only -o "$B/disasm" --force --seed-functions "$B/seeds.json" > "$B/disasm.log" 2>&1
"$PY" -m tools.func_id "$B/sh2pc.xbe" --functions "$B/disasm/functions.json" --strings "$B/disasm/strings.json" \
    --xrefs "$B/disasm/xrefs.json" -o "$B/func_id" > "$B/func_id.log" 2>&1
"$PY" -m tools.abi_analysis "$B/sh2pc.xbe" --disasm-dir "$B/disasm" --func-id-dir "$B/func_id" --output-dir "$B/abi" > "$B/abi.log" 2>&1
else cd "$X"; fi
# 0x570124 / 0x57011E: jmp thunks to MSVCR70 _setjmp3 / longjmp (libpng error handling uses them).
"$PY" -m tools.recomp "$B/sh2pc.xbe" --all --split 250 --game-name sh2 --gen-dir "$ROOT/src/recomp/gen" \
    --setjmp 0x570124 --longjmp 0x57011E ${TRACE:+--trace-functions "$TRACE"} \
    --exclude-manual "$ROOT/src/game/ee.c" \
    --disasm-dir "$B/disasm" --func-id-dir "$B/func_id" --abi-dir "$B/abi" -o "$B/recomp" > "$B/recomp.log" 2>&1
grep -E "functions \(|unresolved call targets" "$B/recomp.log"
