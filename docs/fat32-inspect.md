# FAT32 host inspection

`fat32-inspect` opens an explicitly selected GPT disk image or Linux whole-disk
block device read-only. Select the one-based GPT table entry with
`--gpt-partition`; a partition-device path or bare FAT image is not a GPT disk.
The tool never repairs, changes dirty flags or writes the source. Keep the source
quiescent throughout the command: opening read-only supplies no snapshot or
exclusion against another writer.

```sh
build/fat32-inspect --image /tmp/scratch-gpt.raw --gpt-partition 1 info
build/fat32-inspect --image /tmp/scratch-gpt.raw --gpt-partition 1 check
build/fat32-inspect --image /tmp/scratch-gpt.raw --gpt-partition 1 list /
build/fat32-inspect --image /tmp/scratch-gpt.raw --gpt-partition 1 list /EFI/BOOT
build/fat32-inspect --image /tmp/scratch-gpt.raw --gpt-partition 1 read /EFI/BOOT/BOOTX64.EFI \
  > /tmp/BOOTX64.EFI
```

Options precede the command. `info` prints the selected geometry and FAT
clean/error flags. These flags are observations, not a whole-volume integrity
proof. `check` requests the shared reader's whole-volume check and prints counts
only after success. `list /PATH` prints immediate children as tab-separated type,
file length, full preserved UTF-8 name and available ASCII alias. Names are never
truncated. `read /PATH` streams the selected regular file's bytes to stdout;
diagnostics use stderr. Paths begin at `/` and use the reader's bounded ASCII
lookup profile. A listed Unicode long name can be addressed through its ASCII
alias when one is available. Unsupported names or layouts fail explicitly.

A list or read validates the metadata and chains required by that operation;
success does not prove unrelated metadata sound. The reader preflights that
checked set before emitting entries or file bytes, but subsequent source I/O or
output failure can still leave a prefix. Check the command's exit status before
using redirected data. The shell creates or truncates a redirected destination
before the tool runs; choose an independent destination that cannot refer to the
source disk image.

The source must be a regular file or block device. Regular images have nonzero
512-byte-aligned lengths; Linux devices must report 512-byte logical sectors.
Source length must fit signed 64-bit host file offsets. Reads are checked against
that source length and limited to 64 KiB per host request. Short reads are completed
and interrupted host reads resume the same unpublished request; a source error
or premature end fails the operation and reports the requested sector range.
The host does not retry a failed reader operation or switch discovery modes.

## Workspace and work budgets

The host allocates the reader's required open workspace, then replaces it with
the operation's required workspace if larger. It never allocates an entire file
or directory tree for output. `--memory-budget SIZE` caps this caller-owned
workspace, rather than process resident memory; ordinary libc and stdio storage
is additional. The default is 32 MiB. Unsuffixed sizes are bytes; KiB/MiB/GiB use
binary multipliers and KB/MB/GB use decimal multipliers.

`--step-budget N` and `--sector-budget N` bound examined metadata/work and backing
sectors for each reader call, including open. They default to 64,000,000 steps
and 4,000,000 sectors. `--depth N` sets the operation's maximum directory depth,
defaulting to 64, and participates in the workspace sizing request. These are
host configuration defaults, not filesystem format limits. All values must be
positive integers; budgets are never interpreted as unlimited.

```sh
build/fat32-inspect --image /tmp/scratch-gpt.raw --gpt-partition 1 \
  --memory-budget 64MiB --step-budget 128000000 --sector-budget 8000000 \
  --depth 128 check
```

Exceeding the workspace or work/depth budget fails without weakening validation.
The tool reports unsupported format/geometry/name, corrupt metadata, source I/O,
lookup and output errors separately. Exit status is 0 for success, 1 for an
operation/open/output failure and 2 for invalid command-line syntax. Buffered
stdout errors and a closed output pipe are failures, including errors detected
when stdout closes.
