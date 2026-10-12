# FAT32 raw-write qualification, 2026-10-12

The baseline was pyxis-fs `9b4479e`; the writer change is the commit introducing
this document. Commands used `make -j16`, GCC's GNU C23 freestanding warnings,
and the existing Linux host tools. All changed images were disposable copies
under `/tmp/fat32-write-*`; provided `/tmp/fat32-adversarial` inputs were read
only. No test framework, CI, fixture generator or fault-injection API was added.

## Matched existing-reader baseline

The unchanged source was `/tmp/fat32-adversarial/base.img`, GPT partition 1:
70 MiB GPT disk, 64 MiB FAT32 partition at sector 2048, 512-byte sectors,
one sector/cluster, two mirrored FATs, 129022 clusters. It contains two files,
three directories and five allocated clusters. Seven unprofiled samples before
and after were separate subprocesses with captured stdout/stderr:

```sh
build/fat32-inspect --image /tmp/fat32-adversarial/base.img --gpt-partition 1 check
build/fat32-inspect --image /tmp/fat32-adversarial/base.img --gpt-partition 1 list /
build/fat32-inspect --image /tmp/fat32-adversarial/base.img --gpt-partition 1 read /DIR/CHILD/HELLO.TXT
```

Python `time.perf_counter()` measured invocation-to-completion, without profilers.
Every output and exit status matched. Times below are milliseconds:

| Operation | Before min / median / max | After min / median / max |
| --- | --- | --- |
| check | 1.328 / 1.661 / 2.240 | 1.327 / 1.401 / 2.995 |
| list / | 0.571 / 0.879 / 0.958 | 0.483 / 0.541 / 1.109 |
| read HELLO.TXT | 0.551 / 0.952 / 1.082 | 0.551 / 0.955 / 1.009 |

Before samples (ms): check 1.661, 1.493, 1.864, 1.379, 2.240, 1.819,
1.328; list 0.920, 0.878, 0.882, 0.879, 0.586, 0.958, 0.572;
read 1.082, 0.952, 0.975, 0.607, 1.005, 0.563, 0.598.
After samples (ms): check 2.995, 1.540, 1.401, 1.424, 1.347, 1.327,
1.334; list 0.498, 0.929, 0.679, 0.510, 0.541, 0.483, 1.109;
read 0.970, 0.887, 1.009, 0.964, 0.579, 0.955, 0.551.

These tiny cached Linux host invocations are dominated by process scheduling and
startup. They show no established read regression; they do not establish physical
media performance or a new-writer speedup. The old engine had no comparable raw
write operation. Raw sample logs remain local at `/tmp/fat32-write-baseline.json`
and `/tmp/fat32-write-after.json`.

## Successful mutations and cross-checks

A fresh base copy ran one session: mkdir `/Scratch`, create its long-name file,
append, zero-extend to 700 bytes, replace with `replacement payload`, and check/list/read.
Checks succeeded before/after publication; final counts were three files, four
directories and seven allocated clusters. Extracting the selected partition and
cross-checking gave `fsck.fat -n` exit 0, `mdir` showed the preserved long name and
ASCII alias, and `mtype` returned the exact replacement:

```sh
dd if=/tmp/fat32-write-host.img of=/tmp/fat32-write-host-part.img \
  bs=512 skip=2048 count=131072 status=none
fsck.fat -n /tmp/fat32-write-host-part.img
mdir -i /tmp/fat32-write-host-part.img ::/Scratch
mtype -i /tmp/fat32-write-host-part.img '::/Scratch/Long created file.txt'
```

A separate fresh copy created a 255-byte ASCII component in root, another file,
a new directory and 28 child long-name files. Root and child directory chains
grew. The complete check reported 32 files, four directories and 42 allocated
clusters; `fsck.fat -n` exited 0 and `mdir` enumerated all child names/aliases.

An empty newly created file extended to 1025 bytes and appended `tail`.
`fat32-inspect read` returned exactly 1025 zero bytes followed by four tail bytes.
Byte comparison against its baseline proved all sectors outside the selected
partition unchanged and both FAT copies' first eight bytes (including clean/error
flags) preserved exactly. A restarted host session refused append ID 0 as stale.

A normally formatted one-FAT partition (`mkfs.fat -F32 -S512 -s1 -f1 -h2048`)
also passed mkdir, create, extend to 1100, staged replace and whole check.
`fsck.fat -n` exited 0 and `mtype` returned `replacement`. An additional mkfs
layout with `-b0` passed engine writes/check; fsck requested a missing backup boot
sector, so that layout is not reported as an external checker pass.

A valid Unicode LFN with ASCII alias was added with mtools before the session.
Whole check admitted it and reported `ascii=no`. Root creation refused
UNSUPPORTED; creation in `/DIR` and append to its receipt succeeded. This measures
per-directory namespace gating and absence of a whole-volume ASCII gate. Direct
same-directory Unicode coexistence during an already-owned content update is
supported by inspected code but was not separately constructed through an
external concurrent writer.

## Refusals

A fresh copy attempted existing long-name, short-alias and case-independent
namespace collisions, append without a creation receipt, dot-component paths
and a trailing-dot component. All refused; a full-image SHA-256 comparison was
unchanged.

Fresh copies of existing adversarial images with live entries after the directory
end marker, invalid root label attributes/name, file cross-link, root loop,
LFN checksum mismatch, incorrect dot parent and FSInfo signature corruption
all refused create. Each full-image SHA-256 remained unchanged. These are
existing malformed read-review inputs, not new mutation injection facilities.

A real FAT allocation-full image was produced by ordinary mtools copying a
66056192-byte filler into the base partition, leaving one free cluster.
Creation of a five-byte owned file used that cluster. Extending it to 1025 bytes
refused NO_SPACE before effects; check reported free=0 and the original owned
file still read `hello`.

The independent review also exercised an actual backing capacity failure in a
private user/mount namespace with a 256 KiB tmpfs: a small created file succeeded,
then ordinary zero extension exceeded the backing capacity. The writer stopped
at the data phase, retained target/staged allocation and old receipt, and did not
run the next inspection command. This is a real host ENOSPC capacity result,
not simulated write/flush callbacks. The actual command was an interactive
`unshare --user --map-root-user --mount bash`, followed inside that private
namespace by `mount -t tmpfs -o size=256K tmpfs /tmp/fat32-review-tmpfs` and
`cp --sparse=always /tmp/fat32-adversarial/base.img /tmp/fat32-review-tmpfs/image.img`.
The ordinary CLI commands were `create /Failure.txt = foo`, `extend 0 524288`,
`check`, `quit`. Exit was 1; stderr reported `No space left on device`, data-phase
sector 4560, old cluster 7, staged first cluster 8, 1023 staged/allocated clusters,
and 1047 prepared records. The queued check did not run and the retained old
file read `foo`. Logs were `/tmp/fat32-review-enospc.stdout` and `.stderr`;
its copied image was `/tmp/fat32-review-enospc.img`. The namespace ended with no
remaining mount. This does not prove power-cut or torn-sector recovery.

## Limits

Qualification uses disposable Linux host images and native ordinary build
integration. The parent integration records interactive QEMU consumer validation
separately. No physical device writes, NVMe writer authority, USB host power loss,
torn sector, interrupted multi-FAT update, uncertain directory publication or
external repair was qualified here. Source inspection establishes preflight
budgets, exact receipt validation, barrier ordering, upper FAT nibble/padding
preservation and the permanent failure latch; those are distinct from measured
crash recovery. CI only establishes build coverage for its exact submitted head.
