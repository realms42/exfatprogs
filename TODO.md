# TODO — resize/shrink feature known gaps

## Correctness Bugs (high priority)

### 1. FAT2 not updated
exFAT supports two FATs (`num_fats` in the boot sector). The relocation code only writes FAT 0. If `bs->bsx.num_fats == 2`, FAT 1 (at `fat_offset + fat_length` sectors) must be kept in sync.

### 2. Contiguous multi-cluster directory scan
If a subdirectory has `EXFAT_SF_CONTIGUOUS` set and spans more than one cluster, `scan_directory` stops after the first cluster (because `fat[dir_clu]` is 0). Files in the second+ cluster of such a directory are invisible to the relocation pass and will be silently truncated by `do_shrink`.

### 3. Multi-cluster metadata guard
The metadata guard only checks the start cluster of the upcase table and bitmap. If either chain spans multiple clusters and a later cluster lies beyond the new boundary, `do_shrink` silently corrupts the volume. The full FAT chain of each metadata object must be walked before any writes.

### 4. No truncation of the image file
After `do_shrink` completes the image file retains its original size. For a block device the partition boundary enforces the limit, but for a regular file the trailing bytes are wasted. A `ftruncate(fd, new_vol_sectors * sector_size)` after `fsync` is the right close.

### 5. No validation that new_clu_count doesn't cut through a chain
The code refuses if the highest cluster is beyond the boundary but does not verify that every cluster in a chain is either entirely inside or entirely outside. A corrupted FS could have a chain straddling the boundary in a non-contiguous way. A pre-flight check would make the tool more defensive.

## Quality / Robustness

### 6. PercentInUse not recomputed after relocation
Both `do_grow` and `do_shrink` set `perc_in_use = 0xFF` (unknown). After relocation the exact count is known and the correct value could be written.

### 7. No progress reporting for large relocations
Moving thousands of clusters with no output looks like a hang. A simple `exfat_info("Relocating cluster %u/%u...\r", i+1, ml.count)` would help.

### 8. Predecessor linear scan is O(n²)
Finding the relocated predecessor in Phase 3 does a linear scan over the move list for every entry. For volumes with many out-of-range clusters this is slow. A hash map or sorting by `old_clu` would make it O(n log n).

### 9. No --force flag for metadata-beyond-boundary
Currently a hard refusal. Some advanced users might want to relocate metadata (bitmap can be moved; upcase can be rebuilt). A future `--force` flag could handle this.

### 10. No automated test coverage
There are no automated tests (`make check` or shell test suite). A `tests/` directory with scripts covering at minimum: dry-run block, relocate+shrink, empty shrink, and grow round-trip would prevent regressions.

## Priority Order

1. **FAT2 sync** — correctness bug, easy fix
2. **ftruncate after shrink** — correctness for image files, two lines
3. **Contiguous multi-cluster directory scan** — correctness, moderate effort *(in progress on this branch)*
4. **Automated test script** — safety net for all future work
5. **Multi-cluster metadata guard** — correctness, low effort
