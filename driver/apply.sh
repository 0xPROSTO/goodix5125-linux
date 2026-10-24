#!/usr/bin/env bash
# Adds the goodix5125 driver to a libfprint source tree (tested on v1.94.100).
#   driver/apply.sh path/to/libfprint
set -euo pipefail

HERE=$(cd "$(dirname "$0")/.." && pwd)
TREE=${1:?usage: apply.sh path/to/libfprint}
DST=$TREE/libfprint/drivers/goodix5125

mkdir -p "$DST"
cp "$HERE/driver/goodix5125.c" "$HERE/lib/gx5125.h" "$DST/"
for f in proto device tls sensor image match; do
    cp "$HERE/lib/$f.c" "$DST/gx_$f.c"
done

# Register the driver (idempotent).
if ! grep -q "'goodix5125'" "$TREE/meson.build"; then
    sed -i "s|^    'goodixmoc': {},|    'goodixmoc': {},\n    'goodix5125': { 'helper': ['openssl'] },|" "$TREE/meson.build"
fi
if ! grep -q "drivers/goodix5125/" "$TREE/libfprint/meson.build"; then
    python3 - "$TREE/libfprint/meson.build" <<'EOF'
import sys
p = sys.argv[1]
s = open(p).read()
entry = """    'goodix5125' : files(
        'drivers/goodix5125/goodix5125.c',
        'drivers/goodix5125/gx_proto.c',
        'drivers/goodix5125/gx_device.c',
        'drivers/goodix5125/gx_tls.c',
        'drivers/goodix5125/gx_sensor.c',
        'drivers/goodix5125/gx_image.c',
        'drivers/goodix5125/gx_match.c',
    ),
"""
anchor = "    'goodixmoc' : files("
assert anchor in s, "driver_sources anchor not found"
s = s.replace(anchor, entry + anchor, 1)
open(p, "w").write(s)
EOF
fi

# Upstream bug: the no-introspection branch iterates a dict with one variable.
sed -i 's|^    foreach driver_test: drivers_tests$|    foreach driver_test, _args: drivers_tests|' "$TREE/tests/meson.build"

grep -q "'goodix5125'" "$TREE/meson.build" && grep -q "drivers/goodix5125/" "$TREE/libfprint/meson.build"
echo "goodix5125 added to $TREE"
