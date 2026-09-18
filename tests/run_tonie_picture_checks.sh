#!/bin/sh
# Focused Linux/WSL compile and execution. Artifacts remain in an isolated temp directory.
set -eu
cd "$(dirname "$0")/.."
output=$(mktemp -d)
trap 'rm -f "$output"/*.o "$output/check"; rmdir "$output"' EXIT
flags="-std=gnu11 -ffunction-sections -fdata-sections -DGPL_LICENSE_TERMS_ACCEPTED -DTRACE_NOPATH_FILE
-Iinclude -Iinclude/protobuf-c -Isrc/proto -Isrc/cyclone/common -Isrc/cyclone/cyclone_tcp
-Icyclone/common -Icyclone/cyclone_ssl -Icyclone/cyclone_tcp -Icyclone/cyclone_crypto
-Icyclone/cyclone_crypto/pkix -Icyclone/cyclone_crypto/pkc -Icyclone/cyclone_crypto/rng
-IcJSON -Iopus/include -Iogg/include -Ifat/source"
for source in toniebox_state tonie_picture tonie_user_metadata tb2_ruid; do
    cc $flags -c "src/$source.c" -o "$output/$source.o"
done
cc $flags tests/test_tonie_picture_mqtt.c "$output"/*.o -Wl,--gc-sections,--wrap=tonie_picture_resolve -o "$output/check"
"$output/check"
# Verify the two edited HTTP/RTNL integration translation units without a full product build.
cc $flags -fsyntax-only src/handler_api.c src/handler_rtnl.c
