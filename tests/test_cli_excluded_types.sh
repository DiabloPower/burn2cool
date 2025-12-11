#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=${ROOT}/cpu_throttle_ctl

function runcmd() { echo "$@"; "$@"; }

echo "Starting CLI excluded-types tests"

runcmd $BIN set-excluded-types none

# Toggle wifi on
if runcmd $BIN toggle-excluded wifi; then
  echo 'Toggle wifi succeeded'
else
  echo 'Toggle wifi failed'; exit 2
fi

# Get and check
res=$( $BIN get-excluded-types 2>/dev/null || true )
if [[ "$res" == *wifi* ]]; then
  echo 'wifi present: PASS'
else
  echo "wifi missing: FAIL -> ${res}"; exit 3
fi

# Toggle wifi off
if runcmd $BIN toggle-excluded wifi; then
  echo 'Toggle wifi off succeeded'
else
  echo 'Toggle wifi off failed'; exit 4
fi

# Merge add tokens
if runcmd $BIN set-excluded-types --merge int3400,sen3; then
  echo 'merge add succeeded'
else
  echo 'merge add failed'; exit 5
fi

# Check merged tokens present
res=$( $BIN get-excluded-types 2>/dev/null || true )
if [[ "$res" == *int3400* && "$res" == *sen3* ]]; then
  echo 'merge tokens present: PASS'
else
  echo "merge tokens missing: FAIL -> ${res}"; exit 6
fi

# Remove tokens
if runcmd $BIN set-excluded-types --remove int3400; then
  echo 'remove token succeeded'
else
  echo 'remove failed'; exit 7
fi

# Check removed
res=$( $BIN get-excluded-types 2>/dev/null || true )
if [[ "$res" == *int3400* ]]; then
  echo 'int3400 still present: FAIL'; exit 8
else
  echo 'int3400 removed: PASS'
fi

# Now test --remove --exact behavior
runcmd $BIN set-excluded-types none
runcmd $BIN set-excluded-types int3400,int3401
runcmd $BIN set-excluded-types --remove --exact int3400
res=$( $BIN get-excluded-types 2>/dev/null || true )
if [[ "$res" == *int3400* ]]; then
  echo 'exact remove failed: int3400 still present: FAIL'; exit 9
else
  echo 'exact remove succeeded: PASS'
fi

echo 'All CLI exclude-type tests passed.'

exit 0
