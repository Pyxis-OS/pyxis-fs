# pyxis-fs

Shared Pyxis filesystem core. Requires GNU Make and a GNU C23 compiler.

```sh
make -j16
build/mkpyxisfs --image /tmp/pool.raw --size 256MiB \
  --volume home --owner 0a32efc079ed4c7bab58e224cf119315 --plan
```

The build produces `libpyxis-fs.a`, `mkpyxisfs` and `pyxisfs-inspect` under `build/`.
The example principal ID is illustrative; supply an explicitly provisioned owner.
Omit `--plan` to create a new sparse image containing empty volumes, then inspect it:

```sh
build/pyxisfs-inspect --image /tmp/pool.raw info
build/pyxisfs-inspect --image /tmp/pool.raw volumes
```

See [host-tool usage and limits](docs/host-tools.md),
[core build and API boundaries](docs/core.md) and the
[format contract](docs/format.md).

## License

Original Pyxis material is licensed under [MPL-2.0](LICENSE). See
[LICENSING.md](LICENSING.md) for scope and third-party exceptions.
