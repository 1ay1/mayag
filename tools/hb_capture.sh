#!/bin/sh
# capture.sh CORPUS OUT : FONT|TEXT|ARGS|EXPECTED per line, EXPECTED from hb-shape.
: > "$2"
while IFS='|' read -r font text extra; do
  [ -z "$font" ] && continue
  f=${font%%:*}; idx=0
  case "$font" in *:[0-9]*) idx=${font##*:};; esac
  [ -f "$f" ] || continue
  upem=$(python3 -c "
import struct
d=open('$f','rb').read(); o=0
if d[:4]==b'ttcf': o=struct.unpack('>I',d[12+4*$idx:16+4*$idx])[0]
n=struct.unpack('>H',d[o+4:o+6])[0]
for i in range(n):
  t,c,off,l=struct.unpack('>4sIII',d[o+12+16*i:o+28+16*i])
  if t==b'head': print(struct.unpack('>H',d[off+18:off+20])[0])")
  hb=$(/opt/local/bin/hb-shape --face-index=$idx --font-size=$upem --no-glyph-names $extra "$f" "$text" 2>&1)
  printf '%s|%s|%s|%s\n' "$font" "$text" "$extra" "$hb" >> "$2"
done < "$1"
