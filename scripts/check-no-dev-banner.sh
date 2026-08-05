#!/bin/sh

set -eu

repo_root=$(git rev-parse --show-toplevel)
cd "$repo_root"

set +e
matches=$(git grep -n --fixed-strings \
    -e "tc-dev-environment-banner" \
    -e "tc-dev-banner-height" \
    -- "contrib/data/www/web")
grep_status=$?
set -e

if [ "$grep_status" -gt 1 ]; then
    echo "DEV banner guard failed while scanning the built WebUI." >&2
    exit "$grep_status"
fi

if [ -n "$matches" ]; then
    echo "Refusing DEV banner markers in contrib/data/www/web:" >&2
    echo "$matches" >&2
    exit 1
fi

echo "DEV banner guard passed."
