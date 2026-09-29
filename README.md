# pyxis-fs

Shared Pyxis filesystem core. Requires GNU Make and a GNU C23 compiler.

```sh
make -j16  # build/libpyxis-fs.a
```

The archive provides local codecs and bounded platform interfaces. See
[core build and API boundaries](docs/core.md). Formatter and inspector executables
are not implemented yet.

The [initial format and host-tool contract](docs/format.md) is accepted for the
read-only milestone; the shared encoding layer is implemented.

## License

Original Pyxis material is licensed under [MPL-2.0](LICENSE). See
[LICENSING.md](LICENSING.md) for scope and third-party exceptions.
