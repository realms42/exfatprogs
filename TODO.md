# TODO — resize/shrink feature known gaps

All ten original items have been addressed on this branch.  Two have
remaining work noted below.

## Completed

### 1. FAT2 sync ✓
`write_fat()` now mirrors every write to FAT 1 when `num_fats == 2`.
The zero-fill passes in `do_grow` and `do_shrink` are likewise duplicated
to the second FAT region.

### 2. Contiguous multi-cluster directory scan ✓
`scan_directory()` now accepts `dir_is_contiguous` / `dir_size` and
advances through clusters arithmetically instead of via FAT when the
directory has `EXFAT_SF_CONTIGUOUS` set.  Recursive calls pass the
subdir's own contiguousness flags.

### 3. Multi-cluster metadata guard ✓
`check_chain_in_bounds()` walks the full FAT chain of the allocation
bitmap, upcase table, and root directory before any write is permitted.
Previously only start clusters were checked.

### 4. Image file truncation after shrink ✓
`do_shrink` calls `ftruncate(fd, new_vol_sectors * sector_size)` after
`fsync` for regular files.  Block devices are unaffected.

### 5. Pre-flight chain validation ✓
After Phase 0.5 (FAT normalisation), every file chain is walked with a
step counter capped at `clu_count` (cycle detection) and a per-cluster
seen-bitmap (cross-link detection).  Either condition produces a clear
error before any data is moved.

### 6. PercentInUse recomputed ✓
`count_used_clusters()` reads the already-updated bitmap and counts set
bits.  `do_grow` and `do_shrink` now write the accurate percentage to
both boot sectors instead of the `0xFF` sentinel.

### 7. Progress reporting for large relocations ✓
Phase 3 emits `relocating cluster N/M\r` via `exfat_info` so large
relocations show live progress instead of appearing to hang.

### 8. Predecessor lookup O(n log n) ✓
A sorted `(old→new)` `reloc_map` is built from the move list after
Phase 2.  `bsearch()` replaces the former O(n) linear scan per cluster.

### 9. `--force` flag — **partially done**
The flag exists and is wired up (`-f` / `--force`).  When a metadata
chain extends beyond the new boundary the tool now warns instead of
hard-refusing and proceeds with user-data relocation.

**Remaining:** `--force` does not actually *relocate* metadata chains.
If a bitmap, upcase-table, or root-directory cluster lies beyond the
boundary it will be silently truncated by the subsequent `do_shrink`.
The full implementation requires:
- Walking and relocating the bitmap chain (then flushing the in-memory
  working copy to the new cluster location).
- Walking and relocating the upcase table chain; updating the upcase
  dentry `start_clu` if the head moves.
- Walking and relocating the root-directory chain; updating
  `bs->bsx.root_cluster` in both boot sectors if the head moves.

Until this is done `--force` is suitable only when the user intends to
run `fsck.exfat -y` immediately afterward to repair the truncated
metadata.

### 10. Automated test coverage — **partially done**
`tests/test_resize.sh` exists and covers six scenarios:
- dry-run no-write guarantee
- empty shrink with `ftruncate` verification
- grow
- grow + shrink round-trip
- dry-run-blocked detection
- relocate-then-shrink with fsck verification

**Remaining:**
- No `make check` / `Makefile.am` integration; tests must be run by hand
  with explicit `MKFS=`, `RESIZE=`, `FSCK=` variables.
- No test coverage for FAT2 dual-write (requires a two-FAT image).
- No test coverage for contiguous multi-cluster directory scan.
- No test coverage for `--force` bypass (needs a crafted image where
  metadata chains extend beyond the boundary).
