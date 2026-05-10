# TODO — resize/shrink feature known gaps

All ten original items are fully implemented on this branch.

## Completed

### 1. FAT2 sync ✓
`write_fat()` mirrors every write to FAT 1 when `num_fats == 2`.
The zero-fill passes in `do_grow` and `do_shrink` are likewise duplicated
to the second FAT region.

### 2. Contiguous multi-cluster directory scan ✓
`scan_directory()` accepts `dir_is_contiguous` / `dir_size` and
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

### 9. `--force` flag ✓
The flag exists and is wired up (`-f` / `--force`).  Phase 4 handles
metadata chains that extend beyond the new boundary:
- `truncate_chain_at_boundary()` truncates the bitmap chain and frees
  out-of-bounds clusters.
- `relocate_meta_chain()` copies upcase-table and root-directory clusters
  below the boundary, updating FAT/bitmap; the new head cluster is
  returned so callers can update dentries and boot sectors.
- `update_upcase_start_clu()` rescans the root dir to update the upcase
  dentry's `start_clu` if the head moved.
- `update_boot_root_cluster()` writes the new `root_cluster` at byte
  offset 96 in both boot sectors when the root directory head moves.

### 10. Automated test coverage ✓
`tests/test_resize.sh` covers nine scenarios (13 pass/fail assertions):
- dry-run no-write guarantee
- empty shrink with `ftruncate` verification
- grow
- grow + shrink round-trip
- dry-run-blocked detection
- relocate-then-shrink with fsck verification
- FAT2 dual-write: FAT0 == FAT1 after shrink on a `num_fats=2` image
- `--force` bypass: blocked without flag, fsck-clean with flag
- contiguous multi-cluster directory scan: file in 2nd cluster relocated

`Makefile.am` has a `check-local` target so `make check` runs the suite
with the just-built `mkfs.exfat`, `resize.exfat`, and `fsck.exfat`.
