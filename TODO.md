# resize.exfat — outstanding work

All items below were identified after PR #5 (cluster relocation for shrink).
Pick them up in priority order.

---

## Correctness bugs (fix first)

### 1. FAT2 not updated during relocation
`try_relocate()` in `resize/resize.c` only writes FAT 0.  exFAT supports two
FATs (`bs->bsx.num_fats`).  When `num_fats == 2`, FAT 1 lives at byte offset
`(fat_offset_sect + fat_length_sect) * sector_size` and must be kept in sync
with FAT 0 for every `write_fat()` call made during relocation.

Affected function: `write_fat()` — extend it to accept the `pbr *` and write
both FATs when `num_fats == 2`.  Same fix needed in `normalize_contiguous_chain()`
and the predecessor-redirect calls in Phase 3.

### 2. `ftruncate` missing after shrink
`do_shrink()` updates the boot sector metadata but never truncates the
underlying file descriptor.  For block devices the partition boundary is
authoritative, but for image files the tail bytes remain on disk and waste
space.  Add:

```c
if (ftruncate(fd, (off_t)new_vol_sectors * sector_size)) {
    exfat_err("ftruncate: %s\n", strerror(errno));
    return -errno;
}
```

after the `fsync` in `do_shrink()` (after step 6, before the fsck call).

### 3. Multi-cluster metadata guard is incomplete
`try_relocate()` checks only the *start* cluster of the allocation bitmap and
upcase table against the new boundary.  If either chain spans more than one
cluster and a *later* cluster lies beyond the boundary, the tool proceeds
silently and `do_shrink` corrupts the volume.

Fix: after finding `bm_start_clu` and `uc_start_clu`, walk each chain via the
FAT and check every cluster in the chain, not just the first.

### 4. Contiguous multi-cluster directory not fully scanned
`scan_directory()` follows directory clusters via the FAT.  If a subdirectory
has `EXFAT_SF_CONTIGUOUS` set (NoFatChain) and spans more than one cluster,
`fat[dir_clu]` may be 0 or `EXFAT_FREE_CLUSTER`, causing the walk to stop
after the first cluster.  Files recorded only in the second+ cluster of such a
directory are invisible to the relocation pass and will be silently truncated
by `do_shrink`.

Fix: before the FAT-based cluster advance in `scan_directory()`, check whether
the directory's stream flags have `EXFAT_SF_CONTIGUOUS` set (store this when
the parent directory's STREAM dentry is read) and advance sequentially
(`clu + 1`) instead of following the FAT.

---

## Quality / usability improvements

### 5. Automated test script
There is no `make check` target.  Add `tests/resize_test.sh` that:
- creates a 128 MiB image, formats it, places a file crossing the shrink
  boundary (see notes below on feasible test data sizes), shrinks, and runs
  `fsck.exfat -n`.
- tests the dry-run blocked message.
- tests empty-image shrink.
- tests grow round-trip.

Wire it into `Makefile.am` as a `check` target.

**Note on test data sizing**: with 4 KiB clusters, a 64 MiB shrink target
leaves only ~62 MiB of usable cluster space.  A file larger than ~62 MiB
cannot be relocated into the new boundary.  Place the test file starting at
cluster 1000 (leaving ~4 MiB of free space below the boundary) and size it
so that only a small tail extends past the boundary.  See the manual test
steps in the PR #5 description for a concrete working example.

### 6. Progress reporting for large relocations
`try_relocate()` prints nothing while copying clusters, which looks like a
hang on large volumes.  Add a periodic `exfat_info` line, e.g.:

```c
if ((i % 256) == 0)
    exfat_info("Relocating clusters... %u/%u\r", i, ml.count);
```

Print a final newline after the loop.

### 7. O(n²) predecessor lookup in Phase 3
The inner loop that finds the relocated predecessor:

```c
for (j = 0; j < i; j++) {
    if (ml.entries[j].old_clu == eff_pred) { ... }
}
```

is O(n²) in the number of clusters being moved.  For volumes where thousands
of clusters need relocation this is noticeable.  Replace with a sorted array
+ binary search, or a simple hash map keyed on `old_clu`.

### 8. `PercentInUse` after relocation
Both `do_grow` and `do_shrink` set `perc_in_use = 0xFF` (unknown).  After a
successful relocation + shrink the exact used-cluster count is known; write
the correct percentage instead of 0xFF.

---

## Future / lower priority

### 9. `--force` to allow metadata relocation
The bitmap and upcase table could in principle be relocated (bitmap needs
special handling; upcase can be rebuilt).  A `--force` flag could unlock this
for the rare case where those clusters land beyond the boundary.

### 10. Pre-flight chain-straddle check
Verify that every chain is either fully inside or fully outside the new
boundary before writing anything.  A corrupted FS could have a chain that
straddles the boundary in a non-contiguous way and currently gets partially
truncated silently.

---

## Branch / repo hygiene

- A `main` branch needs to be created pointing at the upstream baseline
  (the commit before any Claude resize work) so that a single PR from
  `master` → `main` shows all accumulated changes.  See the session notes
  for the exact prompt to use.

- PR #5 (`claude/exfat-resize-feature-xdo3S` → `master`) is open and
  waiting for review/merge before the above fixes are stacked on top.
