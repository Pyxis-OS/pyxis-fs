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
The retired COW implementation and its test suite remain in Git; older documents
and measurement records are marked historical.

## License

Original Pyxis material is licensed under [MPL-2.0](LICENSE). See
[LICENSING.md](LICENSING.md) for scope and provenance.
