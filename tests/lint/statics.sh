#!/bin/sh
# tests/lint/statics.sh ROOT ALLOW -- no mutable shared statics in mayag.
# Every `static` local or data member that is not const / constexpr /
# thread_local / a Cell must be listed, with its reason, in ALLOW.
root=$1; allow=$2; bad=0
hits=$(cd "$root" && grep -rnE '^\s{4,}static [^(]*[;={]' --include=*.hpp . \
  | grep -vE 'static (constexpr|const |inline|thread_local|_assert|Cell<|\[\[)|static_cast' \
  | grep -vE '^[^:]+:[0-9]+:\s*//' \
  | grep -vE '\) *(const )?(noexcept )?(override )?\{|\) *(const )?(noexcept )?;' \
  | grep -vE '^[^:]+:[0-9]+:\s+static [A-Za-z_:<>]+ [a-z_]+\(' \
  | sed 's#^\./##')
used=""
while IFS= read -r h; do
  [ -z "$h" ] && continue
  file=${h%%:*}; code=$(echo "$h" | cut -d: -f3- | sed 's/^ *//')
  key="$file: $code"
  if grep -qxF "$key" "$allow"; then used="$used
$key"; else echo "shared static: $h"; bad=1; fi
done <<EOT
$hits
EOT
# stale grants
grep -vE '^\s*(#|$)' "$allow" | while IFS= read -r g; do
  echo "$used" | grep -qxF "$g" || { echo "stale grant (no longer matches): $g"; exit 1; }
done || bad=1
[ $bad = 0 ] && echo "statics: no unlisted shared static" || { echo "statics: FAIL -- make it const, a Cell, or list it with a reason"; exit 1; }
