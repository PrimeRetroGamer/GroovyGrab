# GroovyGrab core

The MiSTer core for [GroovyGrab](../README.md). See the main README for setup. It installs beside stock Groovy and doesn't replace it.

Other Groovy senders could work in theory, but for those, use [stock Groovy](https://github.com/psakhis/Groovy_MiSTer).

## Changes from Groovy

- **Pixel rate trim.** Stock Groovy can only change its clock with a reset, which nudges the picture. This core adds or removes about 12 ns per pixel during blanking, so it can follow a capture card's clock with no visible correction.
- Hsync keeps running while the raster is parked.
- Volatile framebuffer and audio are on by default.
- 44.1 and 22.05 kHz audio run at the exact rate.
- The odd field's vsync is exactly half a line off the even one.
- A rare LZ4 decoding error is fixed.
- Signals between the system and audio clocks are synchronized.
- Magenta OSD, and the core is named GroovyGrab.

## Building

With Quartus Prime 17.0.2 Lite, in this folder:

```
quartus_sh --flow compile Groovy -c Groovy
```

This writes `output_files/Groovy.rbf`. Rename it to `GroovyGrab_YYYYMMDD.rbf` to install it.

## License

This folder contains components under multiple licenses, and each file's own notice is authoritative. Built cores are distributed under GPL-3.0. See [LICENSE](LICENSE) for the summary and `licenses` for the full texts.

## Thanks

psakhis for Groovy, and the [MiSTer template](https://github.com/MiSTer-devel/Template_MiSTer) that `sys` comes from.

From Groovy's credits:

- @Calamity, for testing the core and implementing GroovyMAME for it
- @sorgelig, for developing and maintaining MiSTer
- @jotego, for the analog adjustment module, from [JTFRAME](https://github.com/jotego/jtframe)
- @alanswx, for their [lessons](https://github.com/alanswx/Tutorials_MiSTer)
- @wickerwaka, for their tips on using DDR
- @coolbho3k, for their [overclock](https://github.com/coolbho3k/MiSTer-Overclock-Scripts)
- The Mednafen, MAME and RetroArch teams
- @alexxnr, for testing and encouragement

## Original project

This is a fork of [Groovy_MiSTer](https://github.com/psakhis/Groovy_MiSTer) by psakhis, based on commit [`109908c`](https://github.com/psakhis/Groovy_MiSTer/tree/109908c), plus `7301c44`, which adds missing JTFRAME dependencies. Its README covers Groovy itself and other senders. `history.txt` lists the changes in Groovy and in this fork.
