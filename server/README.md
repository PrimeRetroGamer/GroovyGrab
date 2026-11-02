# MiSTer_GroovyGrab

The MiSTer binary for the [GroovyGrab](../README.md) core. See the main README for setup. It only runs while that core is loaded, and doesn't replace your stock `MiSTer` binary.

## Changes from Groovy

- A command to trim the core's pixel rate, and a version number that identifies this core.
- Oversized modelines and blits are refused.
- A session belongs to the address that opened it, and ends after 30 s of silence.
- Fixes for a freed session still in use, and a leak when a session restarted.
- A new screensaver logo.
- Options in `MiSTer.ini` that this binary doesn't know are ignored.

## Building

With the ARM toolchain `gcc-arm-10.2-2020.11-x86_64-arm-none-linux-gnueabihf` on the path, in this folder:

```
make _AF_XDP=0
```

This writes `MiSTer`, which is installed as `/media/fat/MiSTer_GroovyGrab`.

## License

GPL-3.0, see [LICENSE](LICENSE). Libraries in `lib` keep their own licenses.

## Thanks

psakhis for Groovy, and @sorgelig and everyone who works on [MiSTer](https://github.com/MiSTer-devel/Main_MiSTer).

## Original project

This is a fork of [psakhis's Main_MiSTer](https://github.com/psakhis/Main_MiSTer/tree/0272823) at commit `0272823`, which is itself a fork of [MiSTer's Main](https://github.com/MiSTer-devel/Main_MiSTer). See the [MiSTer wiki](https://github.com/MiSTer-devel/Wiki_MiSTer/wiki) for its documentation.
