#!/bin/sh
# Render the HOW TO USE pages on the host and check they fit the round panel.
#   tools/preview_help.sh <outdir>
set -e
here=$(cd "$(dirname "$0")" && pwd); fw=$here/../fw; out=${1:-.}
mkdir -p "$out"
cc -O1 -Wall -I"$fw/common" -I"$fw/gifplayer" "$here/host_help/host_lcd.c" "$fw/gifplayer/help.c" -o "$out/help_host"
"$out/help_host" "$out"
