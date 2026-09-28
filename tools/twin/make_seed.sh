#!/bin/bash
# web/board/sdseed/ (a copy of the board's card: pull_sd.py, or the card over
# USB_MSC) -> web/board/sdseed.zip + sdseed.json, which web/simtwin.js loads
# onto the twin's card whenever the version changes.
set -e
B=$(cd "$(dirname "$0")/../../web/board" && pwd)
cd "$B/sdseed"
rm -f "$B/sdseed.zip"
zip -q -0 -r "$B/sdseed.zip" . -x '.*'
V=$(sha1sum "$B/sdseed.zip" | cut -c1-12)
N=$(find . -type f | wc -l)
printf '{"version":"%s","files":%s,"bytes":%s}\n' "$V" "$N" "$(stat -c %s "$B/sdseed.zip")" > "$B/sdseed.json"
cat "$B/sdseed.json"
