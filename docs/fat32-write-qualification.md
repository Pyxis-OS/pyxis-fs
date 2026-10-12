# FAT32 raw-write qualification, 2026-10-12

Baseline: pyxis-fs `9b4479e`; engine: `fb22ceb56925`. Ordinary `make -j16`
passed GNU C23 freestanding warnings. Changed images were disposable copies under
`/tmp/fat32-write-*`; provided `/tmp/fat32-adversarial` inputs were read only.
No test framework, CI, fixture generator or fault-injection API was added.

## Matched existing-reader baseline

Source: `/tmp/fat32-adversarial/base.img`, GPT 1, 70 MiB disk, 64 MiB partition
at sector 2048, 512-byte sectors, one sector/cluster, two mirrored FATs and 129022
clusters. Seven unprofiled separate subprocesses before/after captured stdout/stderr:

```sh
build/fat32-inspect --image /tmp/fat32-adversarial/base.img --gpt-partition 1 check
build/fat32-inspect --image /tmp/fat32-adversarial/base.img --gpt-partition 1 list /
build/fat32-inspect --image /tmp/fat32-adversarial/base.img --gpt-partition 1 read /DIR/CHILD/HELLO.TXT
```

Python `time.perf_counter()` measured invocation-to-completion. All outputs/exits
matched; check reported two files, three directories and five allocated clusters.
Milliseconds:

| Operation | Before min / median / max | After min / median / max |
| --- | --- | --- |
| check | 1.328 / 1.661 / 2.240 | 1.327 / 1.401 / 2.995 |
| list / | 0.571 / 0.879 / 0.958 | 0.483 / 0.541 / 1.109 |
| read HELLO.TXT | 0.563 / 0.952 / 1.082 | 0.551 / 0.955 / 1.009 |

Cached Linux process startup/scheduling dominates these samples. No established
read regression; no physical-media performance or writer speedup claim. There was
no existing raw writer baseline. Raw JSON remains local at
`/tmp/fat32-write-baseline.json` and `/tmp/fat32-write-after.json`.

## Mutation and external cross-checks

Fresh copies passed mkdir `/Scratch`, create a long-name file, append, zero-extend
to 700 bytes, staged replace with `replacement payload`, and check/list/read.
Final check: three files, four directories, seven allocated clusters. Extraction,
`fsck.fat -n` exit 0, `mdir` and `mtype` confirmed names/aliases/content:

```sh
dd if=/tmp/fat32-write-host.img of=/tmp/fat32-write-host-part.img \
  bs=512 skip=2048 count=131072 status=none
fsck.fat -n /tmp/fat32-write-host-part.img
mdir -i /tmp/fat32-write-host-part.img ::/Scratch
mtype -i /tmp/fat32-write-host-part.img '::/Scratch/Long created file.txt'
```

A root name of 255 ASCII bytes, another file, mkdir and 28 child long-name files
passed root/child directory growth: 32 files, four directories, 42 allocated
clusters; fsck exit 0 and mdir enumerated every child. An empty created file
extended to 1025 then appended `tail`; inspector read matched 1025 zeros plus
the four tail bytes. Byte comparison preserved all outside-partition bytes and
both FAT copies' first eight bytes, including clean/error flags. Restarted
append ID 0 refused STALE.

A one-FAT layout (`mkfs.fat -F32 -S512 -s1 -f1 -h2048`) passed mkdir/create,
1100-byte extension, replace, whole check, fsck exit 0 and exact mtype output.
The additional `-b0` layout passed engine checks/writes; fsck requested a missing
backup boot sector, so it is not an external-checker pass.

An mtools-created valid Unicode LFN/ASCII alias was admitted by whole check
(`ascii=no`); root creation refused UNSUPPORTED, while creation/append in `/DIR`
worked. This measures per-directory gating, not direct Unicode sibling
coexistence during an owned content update; that permission is source-inspected.

## Refusals and real capacity failure

Existing long-name/alias/case-fold collisions, missing receipts, dot-component
and trailing-dot names refused with unchanged full-image SHA-256. Fresh copies
of existing directory-tail, root-label attributes/name, cross-link, root-loop,
LFN-checksum, dot-parent and FSInfo-signature corrupt inputs refused create;
each full-image SHA-256 remained unchanged.

Ordinary mtools copy of a 66056192-byte filler left one free FAT cluster.
Creating `hello` used it; extension to 1025 refused NO_SPACE before effects.
Whole check reported free=0 and the owned file still read `hello`.

Independent review used a sparse image on a private 256 KiB tmpfs (`unshare
--user --map-root-user --mount bash`, then `mount -t tmpfs -o size=256K tmpfs
MOUNTPOINT`). Create `foo` succeeded; ordinary extension to 524288 bytes reported
real ENOSPC at DATA sector 4560, old first cluster 7, staged first 8, 1023 staged/
allocated clusters and 1047 prepared records. The old receipt/file retained
`foo`; the queued check did not run. No simulated callbacks or automatic cleanup.

## Limits

These are Linux host-image results; parent integration records interactive QEMU
separately. No physical writes, NVMe authority, USB power loss, torn sectors,
interrupted FAT/directory publication or external repair were qualified.
Preflight budgets, exact receipt validation, barriers, FAT nibble/padding
preservation and permanent stop are source-inspected contracts, distinct from
measured crash recovery. CI establishes builds only for its exact submitted head.
