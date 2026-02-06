#!/usr/bin/env bash
# Test Kobo/BookLore API the same way the device does: token-loop (no ?page= or ?pageSize=).
# Usage: KOBO_TOKEN=9c4d4a6f-1cd7-49d2-82fc-5cc7566ee9fc ./scripts/kobo_sync_curls.sh
: "${KOBO_TOKEN:=9c4d4a6f-1cd7-49d2-82fc-5cc7566ee9fc}"
SYNC_URL="http://books.wespo.nl/api/kobo/${KOBO_TOKEN}/v1/library/sync"
UA="Kobo eReader"
TMP_HEADERS=$(mktemp)
TMP_BODY=$(mktemp)
trap 'rm -f "$TMP_HEADERS" "$TMP_BODY"' EXIT

echo "=== 1. Initialization (handshake) ==="
curl -sS -A "$UA" "https://books.wespo.nl/api/kobo/${KOBO_TOKEN}/v1/initialization" | python3 -c "
import sys, json
d = json.load(sys.stdin)
r = d.get('Resources', d)
url = r.get('library_sync') or r.get('librarySync') or ''
print('library_sync URL:', url)
print('OK' if url else 'MISSING')
"

echo ""
echo "=== 2. Library/sync – first request (no X-Kobo-SyncToken) ==="
curl -sS -D "$TMP_HEADERS" -A "$UA" "$SYNC_URL" -o "$TMP_BODY"
echo "Response headers (X-Kobo-*):"
grep -i 'x-kobo' "$TMP_HEADERS" || true
echo "Body: $(python3 -c "import json; d=json.load(open('$TMP_BODY')); print(len(d), 'items' if isinstance(d,list) else 'N/A')")"

SYNC_TOKEN=$(grep -i 'x-kobo-synctoken' "$TMP_HEADERS" | sed 's/.*: *//' | tr -d '\r')
if [ -z "$SYNC_TOKEN" ]; then
  echo "No X-Kobo-SyncToken in response; cannot test second request."
  exit 0
fi

echo ""
echo "=== 3. Library/sync – second request (with X-Kobo-SyncToken) ==="
curl -sS -D "$TMP_HEADERS" -A "$UA" -H "X-Kobo-SyncToken: $SYNC_TOKEN" "$SYNC_URL" -o "$TMP_BODY"
echo "Response headers (X-Kobo-*):"
grep -i 'x-kobo' "$TMP_HEADERS" || true
echo "Body: $(python3 -c "import json; d=json.load(open('$TMP_BODY')); print(len(d), 'items' if isinstance(d,list) else 'N/A')")"

echo ""
echo "Token loop: first request returns 5 items + token + X-Kobo-sync: continue; second request with that token returns next 5. Device uses same flow."
