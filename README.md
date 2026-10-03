# pyxis-fs

Shared npfs (next Pyxis filesystem) format library and Linux host tools. Requires
GNU Make and a GNU C23 compiler. Allocation, caching, capabilities and the runtime
writer belong to Caelum.

```sh
make -j16
build/mkfs.npfs --image /tmp/npfs.raw --size 256MiB --journal 8MiB \
  --volume home --source /path/to/source --volume scratch
build/fsck.npfs --image /tmp/npfs.raw
build/npfs-inspect --image /tmp/npfs.raw list --volume home
```

The build produces `libnpfs-format.a`, `mkfs.npfs`,
`fsck.npfs` and `npfs-inspect` under `build/`.
See the [format contract](docs/npfs-format.md) and
[host-tool usage and limits](docs/npfs-host-tools.md).
The retired COW implementation, tests and historical documentation remain in the
[last old-core snapshot](https://git.internal/PyxisOS/pyxis-fs/src/commit/810d2af66d0281e2d8a3e8a396a041f4232f2ce9).

## License

Original Pyxis material is licensed under [MPL-2.0](LICENSE). See
[LICENSING.md](LICENSING.md) for scope and provenance.
