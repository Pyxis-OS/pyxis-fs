# pyxis-fs

Shared native filesystem format library and Linux host tools. Requires GNU Make
and a GNU C23 compiler. Allocation, caching, capabilities and the runtime writer
belong to Caelum.

```sh
make -j16
build/mkpyxisfs-native --image /tmp/native.raw --size 256MiB --journal 8MiB \
  --volume home --source /path/to/source --volume scratch
build/pyxisfs-native-fsck --image /tmp/native.raw
build/pyxisfs-native-inspect --image /tmp/native.raw list --volume home
```

The build produces `libpyxis-fs-format.a`, `mkpyxisfs-native`,
`pyxisfs-native-fsck` and `pyxisfs-native-inspect` under `build/`.
See the [format contract](docs/native-format.md) and
[host-tool usage and limits](docs/native-host-tools.md).
The retired COW implementation, tests and historical documentation remain in the
[last old-core snapshot](https://git.internal/PyxisOS/pyxis-fs/src/commit/810d2af66d0281e2d8a3e8a396a041f4232f2ce9).

## License

Original Pyxis material is licensed under [MPL-2.0](LICENSE). See
[LICENSING.md](LICENSING.md) for scope and provenance.
