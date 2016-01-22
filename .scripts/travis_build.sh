#!/usr/bin/env bash

# The return code will capture an error from ANY of the functions in the pipe
set -o pipefail
make |& tee build.log | grep "Building"
RESULT=$?

if [ $RESULT -eq 0 ]; then
  echo build succeeded
else
  echo build failed
  tail -500 build.log
  exit 1
fi

ctest -L quicktests
