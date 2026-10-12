# pyxis-fs

Shared npfs codecs, FAT32 engines and Linux host tools. Requires
GNU Make and a GNU C23 compiler. Allocation, caching, capabilities and the
mounted backend adapter belong to Caelum.

```sh
make -j16
build/mkfs.npfs --image /tmp/npfs.raw --size 256MiB --journal 8MiB \
  --volume home --source /path/to/source --volume scratch
build/fsck.npfs --image /tmp/npfs.raw
build/npfs-inspect --image /tmp/npfs.raw list --volume home
```

The build produces `libnpfs-format.a`, `mkfs.npfs`,
`fsck.npfs`, `npfs-inspect`, `libfat32-read.a`, `libfat32-write.a`,
`libfat32-mounted.a`, `fat32-inspect` and the disposable-image session tool
`fat32-write` under `build/`.
See the [FAT32 reader contract](docs/fat32-read.md) and
[read-only inspector](docs/fat32-inspect.md) for GPT media, and the
[raw writer contract](docs/fat32-write.md) for bounded creation receipts and durability,
and the [mounted engine](docs/fat32-mounted.md) for retained objects and ordinary mutation.
When libfuse3 development files are installed, it also builds the optional
read-only Linux mount `npfs-fuse SOURCE MOUNTPOINT`, exposing each live volume
as a directory. Use `cp -r --no-preserve=mode MOUNTPOINT/VOLUME DESTINATION`
to copy a writable tree. See the [format contract](docs/npfs-format.md) and
[host-tool usage and limits](docs/npfs-host-tools.md).
The retired COW implementation, tests and historical documentation remain in the
[last old-core snapshot](https://git.internal/PyxisOS/pyxis-fs/src/commit/810d2af66d0281e2d8a3e8a396a041f4232f2ce9).

## License

Original Pyxis material is licensed under [MPL-2.0](LICENSE). See
[LICENSING.md](LICENSING.md) for scope and provenance.
