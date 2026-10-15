# Example plugins

<img src="../docs/images/VST_Compatible_Logo_Steinberg_with_TM.svg" alt="VST Compatible logo" width="110" align="right">

The two smallest complete audio effects we could write, one per format. Each has one parameter, **Gain**, on a stereo input and output. They are here to read, to copy as a starting point, and to test the host with: the test suites build and load both, so plugin hosting is tested on every platform, CI included, without any third-party plugins.

| folder | format | language | the plugin is |
| --- | --- | --- | --- |
| `clap-gain/` | CLAP | C | one file, the CLAP headers only |
| `vst3-gain/` | VST® 3 | C++ | one file, on the VST 3 SDK's `SingleComponentEffect` helper |

Both apply parameter changes on the exact sample they are stamped with, as hosts expect for automation. The CLAP gain runs 0..2 directly; the VST 3 gain receives 0..1 (VST 3 exchanges every parameter normalized) and maps it to 0..2, so its default of 0.5 means "unchanged".

## Building

```sh
.venv/bin/scons -C examples        # from the repository root
```

Results, as installable bundles in the layout each format requires:

```
examples/build/clap_gain.clap                                 Linux, Windows: a shared library
examples/build/vst3_gain.vst3/Contents/x86_64-linux/vst3_gain.so   (…-win/vst3_gain.vst3 on Windows)
```

On macOS both are bundle folders (`Contents/MacOS/<name>` plus an `Info.plist`). To use them in Godot, copy them into `res://bin/clap` and `res://bin/vst3` (see the main README), or into the system plugin folders.

## Licences

Ours, MIT, like the rest of the repository. The VST 3 example is built with the VST 3 SDK (MIT; see `THIRD_PARTY_NOTICES.md`). VST is a registered trademark of Steinberg Media Technologies GmbH.
