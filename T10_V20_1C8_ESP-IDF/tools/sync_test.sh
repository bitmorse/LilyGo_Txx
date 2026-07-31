#!/usr/bin/env bash
# End-to-end test of the device sync file server (docs/DEVICE_FILE_SYNC.md §6).
# Run from a machine on the SAME Wi-Fi as the device (not the agent's sandboxed
# shell, which Tailscale breaks).
#
#   tools/sync_test.sh [HOST]
#
# HOST defaults to the station IP:port; pass t10.local:8080 to use mDNS instead.
# The token is auto-fetched from /info (a dev-only field); override with TOKEN=xxx.
set -u

HOST="${1:-192.168.123.199:8080}"
BASE="http://$HOST"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

pass=0; fail=0
ok(){ echo "  PASS  $1"; pass=$((pass+1)); }
no(){ echo "  FAIL  $1"; fail=$((fail+1)); }

echo "== GET /info (no token) =="
info=$(curl -s -m 6 "$BASE/info" || true)
echo "  $info"
echo "$info" | grep -q '"softap_ssid"' && ok "reachable" || {
    no "unreachable — device up and on this Wi-Fi?"; exit 1; }

TOKEN="${TOKEN:-$(echo "$info" | python3 -c 'import sys,json;print(json.load(sys.stdin).get("token",""))' 2>/dev/null)}"
[ -n "$TOKEN" ] && echo "  token: $TOKEN" || { no "no token in /info"; exit 1; }
AUTH="Authorization: Bearer $TOKEN"

echo "== bad token rejected =="
code=$(curl -s -m 6 -o /dev/null -w '%{http_code}' -H "Authorization: Bearer WRONG" "$BASE/manifest")
[ "$code" = "401" ] && ok "wrong token -> 401" || no "wrong token -> $code (expected 401)"

mbps(){ awk -v b="$1" 'BEGIN{printf "%.2f MB/s", b/1048576}'; }   # bytes/sec -> MB/s

echo "== GET /manifest =="
mtime=$(curl -s -m 30 -H "$AUTH" -o "$TMP/man.json" -w '%{time_total}' "$BASE/manifest")
man=$(cat "$TMP/man.json")
if ! echo "$man" | python3 -m json.tool >/dev/null 2>&1; then
    no "manifest not valid JSON: $man"; echo "(401 => token rotated; re-read boot log)"; exit 1
fi
ok "valid JSON  (${mtime}s)"
if ! read -r ID BYTES SHA NAME < <(python3 - "$man" <<'PY'
import sys, json
m = json.loads(sys.argv[1])
if not m.get("files"):
    print("NONE 0 - -"); raise SystemExit
f = m["files"][0]
print(f["id"], f["bytes"], f.get("sha256", "-"), f["name"])
PY
); then no "manifest parse failed"; exit 1; fi
if [ "$ID" = "NONE" ]; then
    echo "  (no files on SD — record something first, then re-run)"; exit 0
fi
echo "  file0: id=$ID  name=$NAME  bytes=$BYTES  sha256=$SHA"

echo "== resumable download (curl -C - retries the missing bytes) =="
: > "$TMP/full"                                    # start empty; -C - resumes
attempts=0; t0=$(date +%s)
while :; do
    attempts=$((attempts+1))
    curl -s -m 60 -H "$AUTH" -C - -o "$TMP/full" "$BASE/file/$ID" >/dev/null 2>&1 || true
    have=$(wc -c < "$TMP/full" | tr -d ' ')
    [ "$have" -ge "$BYTES" ] && break
    [ "$attempts" -ge 40 ] && { echo "  gave up after $attempts attempts ($have/$BYTES)"; break; }
    echo "  ... $have/$BYTES bytes — resuming (attempt $((attempts+1)))"
done
elapsed=$(( $(date +%s) - t0 ))
if [ "$elapsed" -gt 0 ]; then
    avg=$(awk -v b="$have" -v s="$elapsed" 'BEGIN{printf "%.1f KB/s", b/1024/s}')
else avg="n/a"; fi
echo "  $have bytes in $attempts attempt(s), ${elapsed}s (avg $avg)"
[ "$have" = "$BYTES" ] && ok "size == manifest bytes" || no "size $have != $BYTES"
dl=$(shasum -a 256 "$TMP/full" | awk '{print $1}')
[ "$dl" = "$SHA" ] && ok "reassembled sha256 == manifest sha256" || no "sha256 mismatch (dl=$dl)"

echo "== Range / resumable =="
half=$(( BYTES / 2 ))
read -r asecs abps < <(curl -s -H "$AUTH" -D "$TMP/h_a" -r "0-$((half-1))" -o "$TMP/a" \
    -w '%{time_total} %{speed_download}' "$BASE/file/$ID")
read -r bsecs bbps < <(curl -s -H "$AUTH" -r "$half-" -o "$TMP/b" \
    -w '%{time_total} %{speed_download}' "$BASE/file/$ID")
echo "  part A: ${asecs}s ($(mbps "$abps"))   part B: ${bsecs}s ($(mbps "$bbps"))"
grep -qi '206 Partial' "$TMP/h_a" && ok "partial GET -> 206" || no "not 206"
grep -qi '^Content-Range:' "$TMP/h_a" && ok "Content-Range present" || no "no Content-Range"
grep -qi '^Accept-Ranges: *bytes' "$TMP/h_a" && ok "Accept-Ranges: bytes" || no "no Accept-Ranges"
etag=$(grep -i '^ETag:' "$TMP/h_a" | tr -d '"\r' | awk '{print $2}')
[ "$etag" = "$SHA" ] && ok "ETag == sha256" || no "ETag=$etag != $SHA"
cat "$TMP/a" "$TMP/b" > "$TMP/join"
[ "$(shasum -a 256 "$TMP/join" | awk '{print $1}')" = "$SHA" ] \
    && ok "two Range halves reconstruct the file" || no "reconstruction mismatch"

echo
echo "==== $pass passed, $fail failed ===="
[ "$fail" -eq 0 ]
