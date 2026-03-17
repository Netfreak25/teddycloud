#!/bin/bash
# Setup dev-sandbox/run/ with test data. Called from Makefile.
set -e
cd "$(dirname "$0")/.."
SRC=dev-sandbox
DST=dev-sandbox/run
CONTRIB=contrib

echo "[ DEV    ] Setup $DST/"
mkdir -p "$DST"/{config,data/content,data/library,data/www,data/firmware,data/cache,certs/server,certs/client}

cp -r "$SRC"/config/. "$DST"/config/
rm -f "$DST"/config/tonieboxes.json
cp -r "$SRC"/content/. "$DST"/data/content/
cp -r "$SRC"/library/. "$DST"/data/library/

# Copy contrib/data/www (symlink web on Linux for live updates)
if [ -d "$CONTRIB/data/www" ]; then
  for item in "$CONTRIB"/data/www/*; do
    name=$(basename "$item")
    if [ "$name" = "web" ]; then
      rm -rf "$DST/data/www/web"
      ln -sf ../../../../$CONTRIB/data/www/web "$DST/data/www/web"
    else
      cp -r "$item" "$DST/data/www/"
    fi
  done
fi

rm -rf "$DST/data/www/custom_img"
mkdir -p "$DST/data/www/custom_img"
cp -r "$SRC/custom_img/." "$DST/data/www/custom_img/"

# Certs: copy from dev-sandbox or generate
if [ ! -f "$DST/certs/server/ca-root.pem" ]; then
  if [ -f "$SRC/certs/server/ca-root.pem" ]; then
    echo "[ DEV    ] Copy dev certs from $SRC/certs/server/"
    cp -r "$SRC/certs/server/." "$DST/certs/server/"
  elif [ -f bin/teddycloud ]; then
    echo "[ DEV    ] Generate server certificates (one-time, ~5 min)..."
    bin/teddycloud --base_path "$DST" --generate-server-certs || { echo "[ ERR  ] Cert generation failed"; exit 1; }
  fi
fi

echo "[ OK     ] Dev sandbox ready"
