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

# ── Test 7: FAT2 dual-write — both FAT copies stay in sync ──
# Set num_fats=2 in the boot sector, duplicate FAT0 to FAT1, then
# shrink and verify that FAT0 and FAT1 are identical afterward.
test_fat2_sync() {
	make_image $((8 * 1024 * 1024))

	if ! command -v python3 >/dev/null 2>&1; then
		echo "  SKIP: fat2-sync (python3 not available)"
		return
	fi

	python3 - "$IMG" <<'PYEOF'
import struct, sys

def exfat_boot_checksum(sectors, sector_size):
    """Compute exFAT boot-region checksum over sectors 0-10."""
    cs = 0
    for si, sector in enumerate(sectors):
        is_boot = (si == 0)
        for bi, b in enumerate(sector):
            if is_boot and bi in (106, 107, 112):
                continue   # skip VolumeFlags and PercentInUse
            cs = ((cs & 1) << 31 | cs >> 1) + b
            cs &= 0xFFFFFFFF
    return cs

img = sys.argv[1]
with open(img, 'r+b') as f:
    bs = bytearray(f.read(512))
    sect_size_bits  = bs[108]
    fat_offset_sect = struct.unpack_from('<I', bs, 80)[0]
    fat_length_sect = struct.unpack_from('<I', bs, 84)[0]
    sector_size     = 1 << sect_size_bits

    # Set num_fats = 2 (byte 110: bpb64[64] + bsx64 offset 46)
    bs[110] = 2
    f.seek(0)
    f.write(bytes(bs))

    # Copy FAT0 to FAT1
    fat0_off = fat_offset_sect * sector_size
    fat1_off = fat0_off + fat_length_sect * sector_size
    fat_size = fat_length_sect * sector_size
    f.seek(fat0_off); fat_data = f.read(fat_size)
    f.seek(fat1_off); f.write(fat_data)

    # Recompute boot checksum for main region (sectors 0-10)
    sectors = []
    for si in range(11):
        f.seek(si * sector_size)
        sectors.append(f.read(sector_size))
    # sector 0 already has num_fats=2 (we wrote it above)
    sectors[0] = bytes(bs)

    cs = exfat_boot_checksum(sectors, sector_size)
    csum_sector = struct.pack('<I', cs) * (sector_size // 4)

    # Write main checksum sector (sector 11)
    f.seek(11 * sector_size)
    f.write(csum_sector)

    # Write backup boot region (sectors 12-22) = copy of sectors 0-10
    for si in range(11):
        f.seek((12 + si) * sector_size)
        f.write(sectors[si])

    # Write backup checksum sector (sector 23)
    f.seek(23 * sector_size)
    f.write(csum_sector)
PYEOF

	# Record original fat_length_sect before resize — needed to locate FAT1
	# after shrink because resize updates fat_length in the boot sector.
	local orig_fat_len
	orig_fat_len=$(python3 - "$IMG" <<'PYEOF'
import struct, sys
with open(sys.argv[1], 'rb') as f:
    bs = f.read(512)
    print(struct.unpack_from('<I', bs, 84)[0])
PYEOF
)

	# resize will exit non-zero because fsck.exfat rejects num_fats=2 as
	# unsupported; capture output and check that the resize itself completed.
	local rout
	rout=$("$RESIZE" "$IMG" $((4 * 1024 * 1024)) 2>&1) || true
	if ! echo "$rout" | grep -q "Shrink complete"; then
		fail "fat2-sync: resize did not complete (output: $rout)"
		return
	fi

	# Verify FAT0 == FAT1 after resize.
	# FAT1 is at fat0_off + orig_fat_len * sector_size (the original layout).
	# resize correctly mirrors all FAT writes to FAT1 using the pre-resize
	# fat_length; we compare the post-resize (clu_count+2) entries only.
	python3 - "$IMG" "$orig_fat_len" <<'PYEOF'
import struct, sys

img          = sys.argv[1]
orig_fat_len = int(sys.argv[2])
with open(img, 'rb') as f:
    bs = f.read(512)
    sect_size_bits  = bs[108]
    fat_offset_sect = struct.unpack_from('<I', bs, 80)[0]
    clu_count       = struct.unpack_from('<I', bs, 92)[0]
    sector_size     = 1 << sect_size_bits

    fat0_off = fat_offset_sect * sector_size
    fat1_off = fat0_off + orig_fat_len * sector_size
    # Compare only the entries that exist in the new volume
    n_entries = clu_count + 2  # EXFAT_RESERVED_CLUSTERS = 2
    size = n_entries * 4

    f.seek(fat0_off)
    fat0 = f.read(size)
    f.seek(fat1_off)
    fat1 = f.read(size)

    if fat0 != fat1:
        for i in range(n_entries):
            e0 = struct.unpack_from('<I', fat0, i*4)[0]
            e1 = struct.unpack_from('<I', fat1, i*4)[0]
            if e0 != e1:
                print(f"FAT mismatch at entry {i}: FAT0={e0:#010x} FAT1={e1:#010x}",
                      file=sys.stderr)
        sys.exit(1)
PYEOF
	if [ $? -eq 0 ]; then
		pass "fat2-sync: FAT0 == FAT1 after shrink"
	else
		fail "fat2-sync: FAT0 and FAT1 differ after shrink"
	fi
}

# ── Test 8: --force shrink when bitmap chain extends beyond boundary ──
# Craft an image where the bitmap FAT chain has a second cluster beyond
# the shrink target.  Verify --force allows the shrink and fsck passes.
test_force_metadata_reloc() {
	make_image $((8 * 1024 * 1024))

	if ! command -v python3 >/dev/null 2>&1; then
		echo "  SKIP: force-metadata-reloc (python3 not available)"
		return
	fi

	python3 - "$IMG" <<'PYEOF'
import struct, sys

img = sys.argv[1]
with open(img, 'r+b') as f:
    bs = f.read(512)
    sect_size_bits     = bs[108]
    sect_per_clus_bits = bs[109]
    fat_offset_sect    = struct.unpack_from('<I', bs, 80)[0]
    fat_length_sect    = struct.unpack_from('<I', bs, 84)[0]
    clu_offset_sect    = struct.unpack_from('<I', bs, 88)[0]
    clu_count          = struct.unpack_from('<I', bs, 92)[0]
    root_cluster       = struct.unpack_from('<I', bs, 96)[0]
    sector_size        = 1 << sect_size_bits
    cluster_size       = 1 << (sect_size_bits + sect_per_clus_bits)

    # Find primary bitmap dentry in root directory
    root_off = clu_offset_sect * sector_size + (root_cluster - 2) * cluster_size
    bm_de_off = None
    bm_start  = None
    for i in range(cluster_size // 32):
        f.seek(root_off + i * 32)
        de = f.read(32)
        if de[0] == 0x81 and (de[1] & 1) == 0:
            bm_de_off = root_off + i * 32
            bm_start  = struct.unpack_from('<I', de, 20)[0]
            break
    if bm_start is None:
        sys.exit(1)

    # The shrink target is 512 clusters (4 MiB).  We want the bitmap
    # to have a second cluster beyond that boundary.  Use cluster 1535
    # (last cluster of the 8 MiB volume, index 1533 = cluster 1535).
    # Extend the bitmap FAT chain: fat[bm_start] = 1535, fat[1535] = EOF.
    extra_clu = clu_count + 2 - 1   # last valid cluster number

    fat0_off = fat_offset_sect * sector_size
    # fat[bm_start] = extra_clu
    f.seek(fat0_off + bm_start * 4)
    f.write(struct.pack('<I', extra_clu))
    # fat[extra_clu] = EOF
    f.seek(fat0_off + extra_clu * 4)
    f.write(struct.pack('<I', 0xFFFFFFFF))

    # Mark extra_clu as allocated in the bitmap
    bm_off = clu_offset_sect * sector_size + (bm_start - 2) * cluster_size
    idx    = extra_clu - 2   # 0-based cluster index
    f.seek(bm_off + idx // 8)
    b = ord(f.read(1))
    b |= 1 << (idx % 8)
    f.seek(bm_off + idx // 8)
    f.write(bytes([b]))

    # Leave the bitmap dentry DataLength unchanged: only the FAT chain
    # now has an extra cluster, which is what check_chain_in_bounds tests.
PYEOF

	# Without --force this should be blocked
	local out
	out=$("$RESIZE" "$IMG" $((4 * 1024 * 1024)) 2>&1) || true
	if echo "$out" | grep -qi "blocked\|beyond"; then
		pass "force-metadata-reloc: refused without --force"
	else
		fail "force-metadata-reloc: should have been blocked without --force"
	fi

	# With --force it should succeed
	"$RESIZE" --force "$IMG" $((4 * 1024 * 1024)) >/dev/null 2>&1 || { fail "force-metadata-reloc: resize --force failed"; return; }

	if fsck_ok; then
		pass "force-metadata-reloc: fsck clean after --force shrink"
	else
		fail "force-metadata-reloc: fsck reported errors after --force shrink"
	fi
}

# ── Test 9: contiguous multi-cluster directory scan ──
# Craft an image where a subdirectory has EXFAT_SF_CONTIGUOUS set and
# spans two clusters.  A file referenced in the second cluster has its
# data beyond the shrink boundary.  Verify resize correctly finds and
# relocates that file.
test_contiguous_multicluster_dir() {
	make_image $((8 * 1024 * 1024))

	if ! command -v python3 >/dev/null 2>&1; then
		echo "  SKIP: contiguous-multicluster-dir (python3 not available)"
		return
	fi

	# Build an image where a 2-cluster contiguous subdirectory has:
	#   dir_clu1: (dpc-3) deleted entries + file_a dentry set (within bounds)
	#   dir_clu2: file_b dentry set (beyond boundary)
	# dir_clu1 contains no 0x00 (EXFAT_LAST) entry, so the scanner must
	# advance arithmetically into dir_clu2 to find file_b.
	python3 - "$IMG" <<'PYEOF'
import struct, sys

def dentry_checksum(dentries):
    cs = 0
    for di, de in enumerate(dentries):
        for bi, byte in enumerate(de):
            if di == 0 and bi in (2, 3):
                continue
            cs = ((cs << 15) | (cs >> 1)) & 0xFFFF
            cs = (cs + byte) & 0xFFFF
    return cs

def name_hash(name):
    cs = 0
    for ch in name.upper():
        code = ord(ch)
        cs = ((cs << 15) | (cs >> 1)) & 0xFFFF
        cs = (cs + (code & 0xFF)) & 0xFFFF
        cs = ((cs << 15) | (cs >> 1)) & 0xFFFF
        cs = (cs + (code >> 8)) & 0xFFFF
    return cs

def make_dentry_set(attr, name_str, start_clu, data_size, contiguous=False):
    file_de = bytearray(32)
    file_de[0] = 0x85
    file_de[1] = 2
    struct.pack_into('<H', file_de, 4, attr)

    flags = 0x01 | (0x02 if contiguous else 0x00)
    stream_de = bytearray(32)
    stream_de[0] = 0xC0
    stream_de[1] = flags
    stream_de[3] = len(name_str)
    struct.pack_into('<H', stream_de, 4,  name_hash(name_str))
    struct.pack_into('<Q', stream_de, 8,  data_size)
    struct.pack_into('<I', stream_de, 20, start_clu)
    struct.pack_into('<Q', stream_de, 24, data_size)

    name_de = bytearray(32)
    name_de[0] = 0xC1
    name_de[1] = 0x01
    for i, ch in enumerate(name_str):
        name_de[2 + i * 2] = ord(ch)

    cs = dentry_checksum([bytes(file_de), bytes(stream_de), bytes(name_de)])
    struct.pack_into('<H', file_de, 2, cs)
    return bytes(file_de), bytes(stream_de), bytes(name_de)

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
    dpc      = cluster_size // 32

    fat0_off = fat_offset_sect * sector_size
    heap_off = clu_offset_sect * sector_size

    root_off = heap_off + (root_cluster - 2) * cluster_size
    bm_start = None
    for i in range(dpc):
        f.seek(root_off + i * 32)
        de = f.read(32)
        if de[0] == 0x81 and (de[1] & 1) == 0:
            bm_start = struct.unpack_from('<I', de, 20)[0]
            bm_off   = heap_off + (bm_start - 2) * cluster_size
            break
    if bm_start is None:
        sys.exit(1)

    f.seek(bm_off)
    bitmap = bytearray(f.read((clu_count + 7) // 8))

    def is_free(clu):
        idx = clu - 2
        return not (bitmap[idx // 8] & (1 << (idx % 8)))

    def alloc(clu):
        idx = clu - 2
        bitmap[idx // 8] |= 1 << (idx % 8)

    free = [c for c in range(2, clu_count + 2) if is_free(c)]
    free_set = set(free)

    # Find two adjacent free clusters for the contiguous directory
    dir_clu1 = None
    for c in free:
        if c + 1 in free_set:
            dir_clu1 = c
            dir_clu2 = c + 1
            break
    if dir_clu1 is None:
        sys.exit(1)

    remaining = [c for c in free if c != dir_clu1 and c != dir_clu2]
    # file_a_clu within bounds, file_b_clu beyond boundary (>= 514)
    file_a_list = [c for c in remaining if c < 514]
    file_b_list = [c for c in remaining if c >= 514]
    if not file_a_list or not file_b_list:
        sys.exit(1)
    file_a_clu = file_a_list[0]
    file_b_clu = file_b_list[0]

    alloc(dir_clu1); alloc(dir_clu2)
    alloc(file_a_clu); alloc(file_b_clu)
    f.seek(bm_off)
    f.write(bytes(bitmap))

    # FAT: file_a and file_b are single-cluster FAT chains; dir is contiguous
    f.seek(fat0_off + file_a_clu * 4)
    f.write(struct.pack('<I', 0xFFFFFFFF))
    f.seek(fat0_off + file_b_clu * 4)
    f.write(struct.pack('<I', 0xFFFFFFFF))

    # Zero-fill both directory clusters
    f.seek(heap_off + (dir_clu1 - 2) * cluster_size)
    f.write(b'\x00' * cluster_size * 2)

    # dir_clu1: fill slots 0..(dpc-4) with deleted FILE entries (0x05)
    # so there is no 0x00 terminator before the end of the cluster.
    # The scanner must then advance to dir_clu2.
    dir1_off = heap_off + (dir_clu1 - 2) * cluster_size
    for si in range(dpc - 3):
        deleted = bytearray(32)
        deleted[0] = 0x05   # deleted FILE primary (InUse=0, TypeCode=5)
        f.seek(dir1_off + si * 32)
        f.write(bytes(deleted))

    # Slots (dpc-3)..(dpc-1): file_a dentry set (within boundary, valid)
    fa_file, fa_stream, fa_name = make_dentry_set(0x20, "a.tx",
                                                   file_a_clu, cluster_size)
    f.seek(dir1_off + (dpc - 3) * 32)
    f.write(fa_file + fa_stream + fa_name)

    # dir_clu2: file_b dentry set at slots 0-2, rest stays 0x00
    fb_file, fb_stream, fb_name = make_dentry_set(0x20, "b.tx",
                                                   file_b_clu, cluster_size)
    dir2_off = heap_off + (dir_clu2 - 2) * cluster_size
    f.seek(dir2_off)
    f.write(fb_file + fb_stream + fb_name)

    # Pattern in file_b cluster (the one that needs relocation)
    f.seek(heap_off + (file_b_clu - 2) * cluster_size)
    f.write(b'\xCC' * cluster_size)

    # Root directory: add the contiguous subdirectory entry
    subdir_size = 2 * cluster_size
    slot = None
    for i in range(dpc):
        f.seek(root_off + i * 32)
        de = f.read(32)
        if de[0] == 0x00:
            slot = i
            break
    if slot is None:
        sys.exit(1)

    sd_file, sd_stream, sd_name = make_dentry_set(0x10, "sub1",
                                                   dir_clu1, subdir_size,
                                                   contiguous=True)
    f.seek(root_off + slot * 32)
    f.write(sd_file + sd_stream + sd_name)
PYEOF

	# Verify the crafted image is fsck-clean before resize
	if ! fsck_ok; then
		fail "contiguous-dir: image not fsck-clean before resize"
		return
	fi

	# Shrink to 4 MiB — resize must scan dir_clu2 to find file_b
	"$RESIZE" "$IMG" $((4 * 1024 * 1024)) >/dev/null 2>&1 || { fail "contiguous-dir: resize failed"; return; }

	if fsck_ok; then
		pass "contiguous-dir: fsck clean after shrink with contiguous multi-cluster dir"
	else
		fail "contiguous-dir: fsck reported errors"
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
	test_fat2_sync
	test_force_metadata_reloc
	test_contiguous_multicluster_dir
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
