#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Automated regression tests for resize.exfat.
#
# Usage:
#   ./tests/test_resize.sh            # run all tests
#   ./tests/test_resize.sh <name>...  # run specific test(s)
#
# Requirements: mkfs.exfat, resize.exfat (both on PATH or in ../mkfs and
# ../resize), fsck.exfat.  Tests use image files only (no loop devices).

set -e

MKFS=${MKFS:-mkfs.exfat}
RESIZE=${RESIZE:-resize.exfat}
FSCK=${FSCK:-fsck.exfat}

# Add directories of the tools to PATH so that resize.exfat can find
# fsck.exfat when it forks the verifier.
for _t in "$MKFS" "$RESIZE" "$FSCK"; do
	_d=$(dirname "$_t")
	case "$_d" in
	.|'') ;;
	*) export PATH="$(cd "$_d" && pwd):$PATH" ;;
	esac
done

PASS=0
FAIL=0
IMG=$(mktemp /tmp/resize_test_XXXXXX.img)

die() { echo "FATAL: $*" >&2; rm -f "$IMG"; exit 1; }

pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

require_tool() {
	command -v "$1" >/dev/null 2>&1 || die "$1 not found in PATH"
}

require_tool "$MKFS"
require_tool "$RESIZE"
require_tool "$FSCK"

# make_image <size_bytes> [cluster_size]
make_image() {
	local size=$1
	local csz=${2:-4096}

	dd if=/dev/zero of="$IMG" bs=1 count=0 seek="$size" 2>/dev/null
	"$MKFS" -c "$csz" "$IMG" >/dev/null 2>&1
}

fsck_ok() {
	"$FSCK" -n "$IMG" >/dev/null 2>&1
}

# ── Test 1: dry-run on an empty volume reports SHRINK POSSIBLE, no writes ──
test_dryrun_empty_shrink() {
	make_image $((8 * 1024 * 1024))         # 8 MiB
	local before after
	before=$(md5sum "$IMG")
	"$RESIZE" -n "$IMG" $((4 * 1024 * 1024)) >/dev/null 2>&1 && true
	after=$(md5sum "$IMG")
	if [ "$before" = "$after" ]; then
		pass "dry-run does not modify image"
	else
		fail "dry-run modified image"
	fi
}

# ── Test 2: shrink empty volume ──
test_shrink_empty() {
	make_image $((8 * 1024 * 1024))
	local target=$((4 * 1024 * 1024))
	"$RESIZE" "$IMG" "$target" >/dev/null 2>&1 || { fail "shrink empty failed"; return; }
	local actual
	actual=$(stat -c '%s' "$IMG")
	if [ "$actual" -eq "$target" ]; then
		pass "shrink empty: image truncated to target size"
	else
		fail "shrink empty: image size $actual != $target"
	fi
	if fsck_ok; then
		pass "shrink empty: fsck clean"
	else
		fail "shrink empty: fsck reported errors"
	fi
}

# ── Test 3: grow volume ──
test_grow() {
	make_image $((4 * 1024 * 1024))
	# extend the file first so resize can grow into it
	dd if=/dev/zero of="$IMG" bs=1 count=0 seek=$((8 * 1024 * 1024)) 2>/dev/null
	"$RESIZE" "$IMG" $((8 * 1024 * 1024)) >/dev/null 2>&1 || { fail "grow failed"; return; }
	if fsck_ok; then
		pass "grow: fsck clean"
	else
		fail "grow: fsck reported errors"
	fi
}

# ── Test 4: grow then shrink round-trip leaves fsck-clean volume ──
test_grow_shrink_roundtrip() {
	local small=$((4 * 1024 * 1024))
	local large=$((8 * 1024 * 1024))

	make_image "$small"
	dd if=/dev/zero of="$IMG" bs=1 count=0 seek="$large" 2>/dev/null
	"$RESIZE" "$IMG" "$large" >/dev/null 2>&1 || { fail "roundtrip grow failed"; return; }
	"$RESIZE" "$IMG" "$small" >/dev/null 2>&1 || { fail "roundtrip shrink failed"; return; }
	local actual
	actual=$(stat -c '%s' "$IMG")
	if [ "$actual" -eq "$small" ]; then
		pass "roundtrip: image truncated to original size"
	else
		fail "roundtrip: image size $actual != $small"
	fi
	if fsck_ok; then
		pass "roundtrip: fsck clean"
	else
		fail "roundtrip: fsck reported errors"
	fi
}

# ── Test 5: dry-run with data beyond boundary reports SHRINK BLOCKED ──
test_dryrun_blocked() {
	make_image $((8 * 1024 * 1024))

	# Mount and write a file that occupies the upper half — use dd into
	# the raw image at an offset inside the cluster heap to simulate an
	# allocated cluster beyond the halfway mark.  Instead of mounting,
	# we write a small file via a Python helper if available, otherwise
	# skip.
	if ! command -v python3 >/dev/null 2>&1; then
		echo "  SKIP: dryrun-blocked (python3 not available)"
		return
	fi

	# Use python3 to place a 4 KiB pattern at the last cluster of an 8 MiB
	# exFAT image.  We read the boot sector to find clu_offset and
	# cluster_size, then write directly.
	python3 - "$IMG" <<'PYEOF'
import struct, sys

img = sys.argv[1]
with open(img, 'r+b') as f:
    bs = f.read(512)
    sect_size_bits     = bs[108]
    sect_per_clus_bits = bs[109]
    fat_offset_sect    = struct.unpack_from('<I', bs, 80)[0]
    clu_offset_sect    = struct.unpack_from('<I', bs, 88)[0]
    clu_count          = struct.unpack_from('<I', bs, 92)[0]
    root_cluster       = struct.unpack_from('<I', bs, 96)[0]
    sector_size        = 1 << sect_size_bits
    cluster_size       = 1 << (sect_size_bits + sect_per_clus_bits)

    # Find bitmap start cluster from root directory dentry
    root_off = clu_offset_sect * sector_size + (root_cluster - 2) * cluster_size
    bm_start = None
    for i in range(cluster_size // 32):
        f.seek(root_off + i * 32)
        de = f.read(32)
        if de[0] == 0x81 and (de[1] & 1) == 0:
            bm_start = struct.unpack_from('<I', de, 20)[0]
            break
    if bm_start is None:
        sys.exit(1)

    bm_off_in_img = clu_offset_sect * sector_size + (bm_start - 2) * cluster_size
    target_clu    = clu_count - 1   # 0-based index into cluster array

    # Mark last cluster as allocated in the bitmap
    byte_off = bm_off_in_img + target_clu // 8
    bit      = target_clu % 8
    f.seek(byte_off)
    b = ord(f.read(1))
    b |= (1 << bit)
    f.seek(byte_off)
    f.write(bytes([b]))

    # Write EOF FAT entry so the cluster looks like a valid 1-cluster chain
    fat_entry_off = fat_offset_sect * sector_size + (target_clu + 2) * 4
    f.seek(fat_entry_off)
    f.write(struct.pack('<I', 0xFFFFFFFF))

    # Write recognisable data into the cluster
    data_off = clu_offset_sect * sector_size + target_clu * cluster_size
    f.seek(data_off)
    f.write(b'\xAB' * cluster_size)
PYEOF

	local out
	out=$("$RESIZE" -n "$IMG" $((4 * 1024 * 1024)) 2>&1) || true
	if echo "$out" | grep -qi "blocked\|relocat"; then
		pass "dryrun-blocked: reports data beyond boundary"
	else
		fail "dryrun-blocked: unexpected output: $out"
	fi
}

# ── Test 6: relocate-then-shrink on non-empty volume ──
test_relocate_shrink() {
	make_image $((8 * 1024 * 1024))

	if ! command -v python3 >/dev/null 2>&1; then
		echo "  SKIP: relocate-shrink (python3 not available)"
		return
	fi

	# Write a recognisable pattern into the last cluster via raw I/O,
	# then set its bitmap bit so resize sees it as allocated.
	python3 - "$IMG" <<'PYEOF'
import struct, sys

img = sys.argv[1]
with open(img, 'r+b') as f:
    bs = f.read(512)
    sect_size_bits     = bs[108]
    sect_per_clus_bits = bs[109]
    fat_offset_sect    = struct.unpack_from('<I', bs, 80)[0]
    clu_offset_sect    = struct.unpack_from('<I', bs, 88)[0]
    clu_count          = struct.unpack_from('<I', bs, 92)[0]
    root_cluster       = struct.unpack_from('<I', bs, 96)[0]
    sector_size        = 1 << sect_size_bits
    cluster_size       = 1 << (sect_size_bits + sect_per_clus_bits)

    # Find bitmap start cluster from root directory dentry
    root_off = clu_offset_sect * sector_size + (root_cluster - 2) * cluster_size
    bm_start = None
    bm_off_in_img = None
    for i in range(cluster_size // 32):
        f.seek(root_off + i * 32)
        de = f.read(32)
        if de[0] == 0x81 and (de[1] & 1) == 0:  # primary bitmap
            bm_start = struct.unpack_from('<I', de, 20)[0]
            break
    if bm_start is None:
        sys.exit(1)

    bm_off_in_img = clu_offset_sect * sector_size + (bm_start - 2) * cluster_size

    # Mark last cluster as allocated in the bitmap
    target_clu = clu_count - 1       # 0-based index
    byte_off = bm_off_in_img + target_clu // 8
    bit     = target_clu % 8
    f.seek(byte_off)
    b = ord(f.read(1))
    b |= (1 << bit)
    f.seek(byte_off)
    f.write(bytes([b]))

    # Write EOF into FAT for that cluster so it looks like a 1-cluster chain
    fat_entry_off = fat_offset_sect * sector_size + (target_clu + 2) * 4
    f.seek(fat_entry_off)
    f.write(struct.pack('<I', 0xFFFFFFFF))

    # Write pattern data into the cluster
    data_off = clu_offset_sect * sector_size + target_clu * cluster_size
    f.seek(data_off)
    f.write(b'\xBE\xEF' * (cluster_size // 2))
PYEOF

	"$RESIZE" "$IMG" $((4 * 1024 * 1024)) >/dev/null 2>&1 || { fail "relocate-shrink: resize failed"; return; }
	if fsck_ok; then
		pass "relocate-shrink: fsck clean after relocate+shrink"
	else
		fail "relocate-shrink: fsck reported errors"
	fi
	local actual
	actual=$(stat -c '%s' "$IMG")
	if [ "$actual" -eq $((4 * 1024 * 1024)) ]; then
		pass "relocate-shrink: image truncated to target size"
	else
		fail "relocate-shrink: image size $actual != $((4 * 1024 * 1024))"
	fi
}

# ── main ──
TESTS=(
	test_dryrun_empty_shrink
	test_shrink_empty
	test_grow
	test_grow_shrink_roundtrip
	test_dryrun_blocked
	test_relocate_shrink
)

if [ $# -gt 0 ]; then
	TESTS=("$@")
fi

echo "resize.exfat test suite"
echo "======================="
for t in "${TESTS[@]}"; do
	echo "[ $t ]"
	"$t"
done

echo ""
echo "Results: $PASS passed, $FAIL failed"
rm -f "$IMG"
[ "$FAIL" -eq 0 ]
