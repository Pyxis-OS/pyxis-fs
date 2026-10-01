# pyxis-fs

Shared Pyxis filesystem core. Requires GNU Make and a GNU C23 compiler.

```sh
make -j16
build/mkpyxisfs --image /tmp/pool.raw --size 256MiB \
  --volume home --source /tmp/source-home \
  --owner 0a32efc079ed4c7bab58e224cf119315 --plan
```

The build produces `libpyxis-fs.a`, `mkpyxisfs` and `pyxisfs-inspect` under `build/`.
The example principal ID is illustrative; supply an explicitly provisioned owner.
Omit `--plan` to create the new sparse image; omit `--source` for an empty volume.
Inspect, check or extract into a fresh destination:

```sh
build/pyxisfs-inspect --image /tmp/pool.raw info
build/pyxisfs-inspect --image /tmp/pool.raw volumes
build/pyxisfs-inspect --image /tmp/pool.raw check
build/pyxisfs-inspect --image /tmp/pool.raw list --volume home --path .
build/pyxisfs-inspect --image /tmp/pool.raw \
  extract --volume home --path . --output /tmp/extracted-home
```

`access` evaluates supplied principal/root/ceiling policy without authenticating
the principal. `check` verifies both retained states structurally; it does not
verify file contents or repair the image. To inspect a regular whole-disk image,
supply both `--gpt-partition N --sector-size 512|4096` explicitly.

See [host-tool usage and limits](docs/host-tools.md),
[core build and API boundaries](docs/core.md) and the
[format contract](docs/format.md). Run the maintained host contract suite with
`make -j16 check`; scope and requirements are in [testing](docs/testing.md).

## License

Original Pyxis material is licensed under [MPL-2.0](LICENSE). See
[LICENSING.md](LICENSING.md) for scope and third-party exceptions.
