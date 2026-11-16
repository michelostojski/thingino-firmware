#!/bin/sh
#
# gen-jzmmc-patch.sh - generate the jzmmc NOSTS write-status kernel patch
#
# Run from the thingino-firmware root AFTER the kernel has been extracted at
# least once (so output/*/build/linux-*/ exists).  Produces:
#   package/all-patches/linux/3.10.14/NNNN-jzmmc-tolerate-nosts-write-status.patch
#
# The patch narrows jzmmc's ERROR_STAT mask 0x3f -> 0x37 so a write that the
# card completes (PRG_DONE) but returns no CRC-status token for (NOSTS, bit 3)
# is accepted instead of failing op=53 / -EIO.  Context lines are taken from
# YOUR real jzmmc_v12.c; any pre-existing in-tree ERROR_STAT edit is normalised
# so the committed patch contains ONLY the one-line change.
#
set -e

ROOT=$(pwd)
PDIR="$ROOT/package/all-patches/linux/3.10.14"
[ -d "$PDIR" ] || { echo "ERROR: $PDIR not found - run from thingino-firmware root"; exit 1; }

KSRC=$(find output -maxdepth 4 -type d -path '*/build/linux-*' 2>/dev/null \
        | grep -E '/build/linux-' | head -1)
[ -n "$KSRC" ] || { echo "ERROR: extracted kernel not found under output/*/build/linux-*"; \
                    echo "       run 'make linux-extract' first"; exit 1; }
F="$KSRC/drivers/mmc/host/jzmmc_v12.c"
[ -f "$F" ] || { echo "ERROR: $F not found"; exit 1; }
grep -qE '#define[[:space:]]+ERROR_STAT[[:space:]]+0x3[0-9a-fA-F]' "$F" \
    || { echo "ERROR: ERROR_STAT define not found in $F"; exit 1; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
# orig = pristine value 0x3f ; new = patched value 0x37 (normalise either way)
sed -E 's/#define([[:space:]]+)ERROR_STAT([[:space:]]+)0x3[0-9a-fA-F]/#define\1ERROR_STAT\20x3f/' "$F" > "$TMP/orig"
sed -E 's/#define([[:space:]]+)ERROR_STAT([[:space:]]+)0x3[0-9a-fA-F]/#define\1ERROR_STAT\20x37/' "$F" > "$TMP/new"

if cmp -s "$TMP/orig" "$TMP/new"; then
    echo "ERROR: no ERROR_STAT line changed - nothing to patch"; exit 1
fi

N=$(ls "$PDIR" 2>/dev/null | grep -oE '^[0-9]{4}' | sort -n | tail -1)
N=$(printf "%04d" $(( 10#${N:-0} + 1 )))
OUT="$PDIR/${N}-jzmmc-tolerate-nosts-write-status.patch"

{
cat <<'HDR'
jzmmc: accept data-write completion on PRG_DONE without a CRC status token

The HiSilicon Hi3861L over SDIO (MSC1) completes block writes and asserts
PRG_DONE + DATA_TRAN_DONE, but does not drive the CRC-status token back to the
host. jzmmc then flags STAT_CRC_WRITE_ERROR_NOSTS (bit 3, 0x8) and aborts the
otherwise-successful transfer (op=53 / -EIO), which prevents the hichannel
WiFi driver from registering wlan0.

Narrow the error mask from 0x3f to 0x37 so NOSTS no longer fails a write that
the card has in fact programmed. Both the PIO path (jzmmc_polling_status) and
the DMA path (check_error_status) consume ERROR_STAT, so this single change
covers both.

Signed-off-by: thingino cg721 <ostojski@users.noreply.github.com>

HDR
diff -u "$TMP/orig" "$TMP/new" \
  | sed -E \
      -e 's#^--- .*/orig.*#--- a/drivers/mmc/host/jzmmc_v12.c#' \
      -e 's#^\+\+\+ .*/new.*#+++ b/drivers/mmc/host/jzmmc_v12.c#'
} > "$OUT"

echo "wrote $OUT"
echo "----------------------------------------"
cat "$OUT"
echo "----------------------------------------"
echo "sanity: dry-run apply against the extracted tree"
( cd "$KSRC" && patch -p1 --dry-run < "$OUT" ) || \
    echo "NOTE: dry-run offset/fuzz is OK - buildroot re-applies to pristine source at build time"
