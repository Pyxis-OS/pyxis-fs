# pyxis-fs

Shared Pyxis filesystem core. Requires GNU Make and a GNU C23 compiler.

The new [native format](docs/native-format.md) is available beside the old core.
Build and create a standalone pool with an explicit journal size:

```sh
make -j16
build/mkpyxisfs-native --image /tmp/native.raw --size 256MiB --journal 8MiB \
  --volume home --source /path/to/source --volume scratch
build/pyxisfs-native-fsck --image /tmp/native.raw
build/pyxisfs-native-inspect --image /tmp/native.raw list --volume home
```

See [native host-tool usage and limits](docs/native-host-tools.md). Caelum still
mounts the old format; use the old tools below for those mounts.

```sh
make -j16
build/mkpyxisfs --image /tmp/pool.raw --size 256MiB \
  --volume home --source /tmp/source-home \
  --owner 0a32efc079ed4c7bab58e224cf119315 --plan
```

The build produces `libpyxis-fs.a`, `mkpyxisfs`, `pyxisfs-inspect` and `pyxisfs-write` under `build/`.
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
`sudo python3 tests/ram_run.py --suite check` in the
[bounded RAM setup](docs/ram-validation.md). The same launcher provides
`--suite extended`, the small comparative `--suite baseline`, and a configurable
`--suite sustained` comparison. See the
[sustained configuration and accounting](docs/ram-validation.md#sustained-comparison)
before collecting serial runs. The larger recovery workload remains suspended
pending a separately approved RAM execution plan.

## License

Original Pyxis material is licensed under [MPL-2.0](LICENSE). See
[LICENSING.md](LICENSING.md) for scope and third-party exceptions.
