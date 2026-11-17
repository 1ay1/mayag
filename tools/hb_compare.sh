#!/bin/sh
# cmp.sh CASEFILE : each line is  FONT|TEXT|EXTRA-ARGS  ; compares mg_shape vs hb-shape.
pass=0; fail=0
while IFS='|' read -r font text extra; do
  [ -z "$font" ] && continue
  case "$font" in \#*) continue;; esac
  f=${font%%:*}; idx=0
  case "$font" in *:[0-9]*) idx=${font##*:};; esac
  [ -f "$f" ] || continue
  upem=$(python3 -c "
import struct,sys
d=open('$f','rb').read()
o=0
if d[:4]==b'ttcf': o=struct.unpack('>I',d[12+4*$idx:16+4*$idx])[0]
n=struct.unpack('>H',d[o+4:o+6])[0]
for i in range(n):
  t,c,off,l=struct.unpack('>4sIII',d[o+12+16*i:o+28+16*i])
  if t==b'head': print(struct.unpack('>H',d[off+18:off+20])[0])")
  hbargs=$(echo "$extra" | sed 's/--variations=/--variations=/')
  hb=$(/opt/local/bin/hb-shape --face-index=$idx --font-size=$upem --no-glyph-names $hbargs "$f" "$text" 2>&1)
  mg=$(${MGS:-/tmp/mgs} "$f:$idx" "$text" $extra 2>&1)
  if [ "$hb" = "$mg" ]; then pass=$((pass+1)); else fail=$((fail+1)); echo "FAIL $font | $text | $extra"; echo "  hb: $hb"; echo "  mg: $mg"; fi
done < "$1"
echo "pass $pass fail $fail"
