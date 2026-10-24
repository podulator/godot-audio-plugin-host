# Developing

<img src="images/VST_Compatible_Logo_Steinberg_with_TM.svg" alt="VST Compatible logo" width="110" align="right">

Everything about building, testing and changing the host. Using it is in the [README](../README.md).

## Building

```sh
git clone <this repository> && cd <its folder>
git submodule update --init --depth 1
git -C thirdparty/vst3sdk submodule update --init --depth 1 base pluginterfaces public.sdk
python3 -m venv .venv && .venv/bin/pip install scons
.venv/bin/scons target=template_debug -j$(nproc)      # what the editor loads
.venv/bin/scons target=template_release -j$(nproc)    # what exported games load
```

`--depth 1` fetches only the pinned commit of each submodule, about 4 MB in all instead of 46 MB. (`.gitmodules` marks them shallow too, but git honours that only when the pinned commit is the newest on its branch.) Do not clone with `--recursive`: the VST3 SDK has more submodules of its own (VSTGUI, the documentation, tutorials, some 300 MB), and the host needs only `base`, `pluginterfaces` and `public.sdk`, which the third line fetches. Submodules come without their tags, so `git describe` inside them can name an older tag; the commits are the right ones.

The libraries are written straight into `demo/addons/clap_host/bin/` and `demo/addons/vst3_host/bin/`, so the demo project uses them at once.

| platform | needs | notes |
| --- | --- | --- |
| Linux | GCC or Clang, the X11 headers (`libx11-dev`) | libX11 itself is loaded only when a plugin window opens, so the libraries run without it |
| Windows | MSVC (Visual Studio 2022) or MinGW | `platform=windows`; MSVC is what CI uses |
| macOS | Xcode's command-line tools | universal (arm64 + x86_64); the oldest macOS is `macos_deployment_target`, default 10.15 |

`VERSION` holds the release version; the build writes it into each macOS framework's `Resources/Info.plist`.

## Example plugins

`examples/` holds the smallest gain effect in each format: `clap-gain/` in plain C, `vst3-gain/` in C++ on the SDK's `SingleComponentEffect`. They are there to read and to copy. `.venv/bin/scons -C examples` builds both as installable bundles; `test_plugin=yes` also builds the test plugins below.

## Tests

```sh
tests/run_all.sh
```

runs everything that can run on this machine, and needs Godot (4.5 or later) on `PATH` as `godot`:

| part | what it checks |
| --- | --- |
| `tests/core/` | the plain C++ core (queues, timing, sidechains), under AddressSanitizer and ThreadSanitizer |
| `tests/clap/`, `tests/vst3/` | each host against real plugins, under AddressSanitizer: loading, parameters, state, sidechain, sample-exact timing |
| `demo/tests/*.tscn` | headless Godot scenes: both formats on real buses, sidechains, state, timing, the lock, menu classes, plugin folders |
| `tests/platform/` | the same core checks with only this repository's plugins: what CI runs on every platform |
| `tests/export/` | a real Linux export (debug and release) loading its plugin, and a macOS app assembled on Linux |
| `tests/windows/` | plugin windows on Windows: under Wine on Linux (nothing shows on screen), natively on Windows |
| `tests/macos/` | plugin windows on macOS (macOS only) |
| `tests/scan_guard/` | a plugin that crashes the editor while it is scanned, and the editor surviving it |
| editor probes | a real (headless) editor: the Add Effect menu, plugin windows opening on click, the help pages |

Our own plugins mean nothing has to be installed. Tests for third-party plugins (Dragonfly Reverb for a real reverb tail and a real plugin window, OneTrick URCHIN for refusing instruments) skip themselves where those are missing.

- `tests/plugins/gaph_test_plugin.c`: a deterministic CLAP effect (out = main x gain + sidechain x mix, sample-accurate events, state), with a GUI on Windows and macOS.
- `tests/plugins/gaph_test_vst3.cpp`: its VST3 twin (the same Gain and Sidechain Mix, normalised, a sidechain bus, sample-accurate changes, state), with an editor on Windows and macOS.

Their GUIs size themselves 300 x 200 times the scale the host sends, and can ask the host for a new size, so the GUI tests (`tests/gui/`) check window type, scale, sizing and resize requests through the host's real code.

Two tests depend on timing. Run them on an idle machine: a Godot build alongside makes them fail. Scratch files go to `$GAPH_TEST_TMP`, or a folder in the system's temporary directory.

## CI (GitHub Actions)

One reusable workflow per platform, and **Build all platforms** as the entry point. Ordinary pushes run nothing: on a private repository, macOS minutes cost ten times as much as Linux ones, and Windows twice. It runs in two ways:

- **By hand** (Actions > Build all platforms > Run workflow), with a checkbox per job, so a Linux-only run costs one Linux job. The result is a run artifact.
- **On a version tag** (`v*`): everything, and a release. The tag must match `VERSION`, which is checked first, so a mistyped tag costs one Linux minute rather than a macOS build. Then all three platforms are built, the full suite runs on Linux and the platform tests on Windows and macOS, and if all of that passes, a **draft** GitHub release is created with `godot-audio-plugin-host-<tag>.zip`, to check and publish by hand. A tag with a hyphen (`v1.0.0-beta`) is marked pre-release. macOS is signed with the Developer ID only if the repository variable `MACOS_CODESIGN` is `true`.

To release: set `VERSION`, commit, then `git tag v$(cat VERSION) && git push origin v$(cat VERSION)`.

| checkbox | runner | does |
| --- | --- | --- |
| Linux, Windows, macOS | ubuntu-22.04, windows-latest (MSVC), macos-latest | build `template_release` and `template_debug` |
| Run the full test suite on Linux | ubuntu-22.04 | `tests/run_all.sh` against Godot 4.5.0, the oldest version supported, with export templates |
| Platform test on Linux / Windows / macOS | each OS | `tests/platform/run.sh`, plus the plugin-window tests on Windows and macOS |
| macOS: sign with the Developer ID and notarise | macos-latest | see below |

The Linux build runs on Ubuntu 22.04 on purpose: a library linked against a newer glibc does not start on an older system. A final job merges every platform's libraries into one artifact, **godot-audio-plugin-host**, holding only `addons/clap_host` and `addons/vst3_host`, each with its own copy of `README.md`, `LICENSE` and `THIRD_PARTY_NOTICES.md` (nothing at the zip's root: the editor's installer extracts only the addon folders), ready to unzip into a project. Every job keeps an SCons cache, so a rebuild after a small change is quick.

### macOS signing

Every macOS build signs its frameworks **ad hoc** (no certificate) and verifies them, which proves they are signable. A downloaded copy still needs the quarantine flag cleared. Signed with a **Developer ID** and **notarised** by Apple, they load on any Mac as they are: tick the checkbox once the six secrets exist. [macos-signing.md](macos-signing.md) has every step, including making the certificate on Linux, without a Mac.

## Class reference

Every class, property, method and signal is documented in `doc_classes/`, in Godot's own XML format, and compiled into the debug and editor builds: it shows in the editor's help (F1) and in the inspector's tooltips. Release builds leave it out. `tests/run_all.sh` checks that the help shows in the editor, and that `doc_classes/` matches the API (Godot's doc tool, run over a copy, must change nothing). After changing the API:

```sh
godot --headless --path demo --doctool /tmp/docs --gdextension-docs
```

and merge the new entries from `/tmp/docs/doc_classes/` into `doc_classes/`.

## Layout

| path | what it is |
| --- | --- |
| `src/core/` | plain C++, compiled into both libraries, no Godot: effect runtime, lock-free queue, parameter timing (`block_clock.h`), sidechain channels, run loop, and the plugin windows (X11, Win32, Cocoa: `native_window.h`) |
| `src/godot_common/` | Godot glue both libraries compile in, as functions and helpers rather than shared classes: parameter properties, Add Effect menu classes, plugin folders, export, scan guard, editor window |
| `src/clap/`, `src/vst3/` | each format's host, Godot classes, editor plugin and entry point |
| `doc_classes/` | the class reference, one XML file per class |
| `examples/` | the smallest gain plugin in each format, also used by the tests |
| `vst3_sdk_sources.txt` | the VST3 SDK files compiled in; read by SConstruct and the tests |
| `thirdparty/` | submodules: godot-cpp (4.5 branch), the CLAP SDK 1.2.9, the VST3 SDK 3.8.1 (all MIT) |
| `demo/` | a Godot project with both addons, the test scenes (`tests/`) and a by-hand GUI test (`gui_test/`) |
| `tests/` | see [Tests](#tests) |
| `docs/` | this file, macOS signing, the VST logo |

The core registers no Godot classes, because two extensions registering the same class name is a load failure. Everything Godot-facing is named per format.

## Design notes

Verified against the Godot 4.5 source:

- `AudioServer` calls effects in fixed 512-frame blocks. `EffectRuntime` splits larger blocks anyway, so plugins are never handed more than they were activated for.
- One effect instance is created **per stereo channel pair per bus**, so a 7.1 bus means four plugin instances. A resource keeps a registry of its live instances, so a parameter set on the resource reaches all of them.
- Instances are created and destroyed on the main thread **while the audio lock is held**, and are recreated whenever the bus layout changes. So nothing slow happens there: an instance takes a ready-made spare plugin if there is one (`spare_instances`, default 1), or passes audio through until the next frame builds one outside the lock. Removed plugins are destroyed on the next frame too. Measured with Dragonfly Hall: creating one takes ~4 ms, the lock is now held for under 0.2 ms. Swapping a running plugin goes through `AudioServer.lock()`, so the processor pointer needs no atomics.
- A plugin may change its parameter list while running (CLAP `params.rescan`, VST3 `restartComponent`). The list is re-read, values for vanished parameters are dropped, the inspector rebuilds, and `parameters_changed` is emitted.
- Parameter changes cross to the audio thread through a lock-free single-producer, single-consumer queue. Each carries a timestamp, and lands at the matching sample one block later, so changes keep their spacing exactly (measured: every 5 ms change lands 240 samples apart at 48 kHz, despite Godot mixing two blocks back to back). `set_parameter_at(id, value, delay)` schedules a change ahead with the same precision. See `src/core/block_clock.h`.
- Switching plugins creates the new instances outside the audio lock, swaps pointers under it, and destroys the old ones after it, so audio is held up only for the swap itself.
- A `.clap` binary or `.vst3` module is loaded once and shared (its entry must run once).
- VST3 plugins are two objects, a processor and a controller. The host creates and connects both, sends every parameter change to both, and forwards edits made in the controller to the processor. HostedEffect forwards any plugin-made change to every instance, which also keeps the several copies on a surround bus in step.
- A sidechain is a send effect writing into a named channel and the plugin's runtime reading it, both on Godot's single mixing thread. A write counter stops a reader from re-using a block when the source has stopped (a looping block would buzz). See `src/core/sidechain.h`.
- The per-plugin menu entries are classes registered at editor start through GDExtension's C API, one per plugin, whose create function returns a generic effect with the path set (`src/godot_common/menu_classes.h`). Godot only accepts plain identifiers as class names.
- VST3 instruments share the effect category; only the subcategory ("Instrument") tells them apart, so they are filtered out explicitly.

The plugin windows: `src/core/native_window.h` names one window class per platform, all with the same interface (`X11Window`, `Win32Window`, `CocoaWindow`), and `plugin_window_api()` gives what else differs: the window type each format asks for, and whether the host sends the plugin a scale (Linux and Windows: pixels, so yes; macOS: points, so no, as CLAP's `gui.h` requires). On Windows, Godot's own main loop dispatches the messages of every window on its thread, so plugin windows need no loop of their own.

## Known issues in detail

### In plugins

- **Plugins that allocate in `ModuleEntry` and never free it** (OneTrick URCHIN, for example, and some plugin frameworks' VST3 wrappers when their binary is unloaded) show up as leaks under LeakSanitizer. The VST3 tests suppress exactly those, and were checked to report nothing else.

### In Godot

- **Freeing a playing `AudioStreamGenerator` player can hang Godot's audio thread** (found with a debug build of 4.5; the same bug as godotengine/godot#92349, and the raw pointer is still in 4.6's source). The playback keeps a raw pointer to its generator; freed while it is still being mixed, the generator's mix rate reads as garbage (-120), the resampler's step goes negative, and its refill loop spins forever holding the audio lock, so the next `remove_bus()` waits forever. This is Godot's bug, not the extensions': our tests triggered it by freeing such players, and two plugin formats tearing down made the window wide enough to hit (Godot 4.5: about 5 runs in 6; in 4.6 and 4.7 the timing happens not to). The tests now stop the player and wait a little before freeing it (`demo/tests/test_util.gd`), and the whole suite passes on Godot 4.5 again. Games that free generator players should do the same.

- **Headless first import crashes** ([godotengine/godot#111645](https://github.com/godotengine/godot/issues/111645), open since 4.5): `godot --headless --import` on a project with any GDExtension and no `.godot/` folder aborts in doc generation after the import has finished. Import a second time, or use `xvfb-run` instead of `--headless`. `tests/run_all.sh` does the former.
- **"1 ObjectDB instance was leaked at exit"** in the smoke test is the test's `AudioStreamGeneratorPlayback`; it appears with Godot's built-in effects too.

## Licences and the VST trademark

The project is MIT; everything compiled in is MIT too (`THIRD_PARTY_NOTICES.md`, which CI copies into each addon). VST is a registered trademark of Steinberg Media Technologies GmbH, and using the name follows Steinberg's VST Usage Guidelines (`thirdparty/vst3sdk/VST3_Usage_Guidelines.pdf`). The VST Compatible logo in `docs/images/` is Steinberg's own artwork, from their `vst3_doc` repository (`artwork/`, at the commit the SDK pins). The guidelines want it unaltered, at least 20 x 18.7 mm in print, on every page that refers to VST: so it heads the README, this file and `examples/README.md`, and must go on an Asset Library listing too. Pages where an image cannot go (the editor's help) carry the trademark sentence instead.
