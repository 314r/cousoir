#!/usr/bin/env bash
# Runs a command with its home, temporary, cache, data, config and state
# folders inside build/sandbox, so that the tests, Calibre and trial runs of
# Cousoir write nothing outside the source tree: no ~/.qttest, no
# ~/.cache/cousoir, no files in /tmp, and Calibre's preferences left alone.
#
#   tools/sandbox.sh ctest --test-dir build --output-on-failure
#   tools/sandbox.sh ./build/cousoir
#
# The library Calibre opens is not reachable from the sandbox: set
# COUSOIR_LIBRARY_PATH to a library inside it. SANDBOX sets another folder.
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
sandbox=${SANDBOX:-$root/build/sandbox}
mkdir -p "$sandbox"/{home,tmp,cache,data,config,state,golden}
sandbox=$(cd "$sandbox" && pwd)

export HOME="$sandbox/home"
export TMPDIR="$sandbox/tmp"
export XDG_CACHE_HOME="$sandbox/cache"
export XDG_DATA_HOME="$sandbox/data"
export XDG_CONFIG_HOME="$sandbox/config"
export XDG_STATE_HOME="$sandbox/state"
export CALIBRE_CONFIG_DIRECTORY="$sandbox/config/calibre"
export COUSOIR_GOLDEN_TMP="$sandbox/golden"
# Qt logs to the journal when its error output is no terminal: keep it here.
export QT_FORCE_STDERR_LOGGING=1
# Python's own cache of compiled files, should Calibre write one.
export PYTHONPYCACHEPREFIX="$sandbox/cache/pycache"

exec "$@"
