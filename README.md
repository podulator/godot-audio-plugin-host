<h1><img src="https://raw.githubusercontent.com/podulator/godot-audio-plugin-host/v1.0.0/docs/images/gaph-fla-512.png" alt="Godot Audio Plugin Host icon" width="128" align="absmiddle">&nbsp;Godot Audio Plugin Host</h1>

Run **CLAP** and **VST® 3** audio effect plugins on Godot's audio buses, in the editor and in exported games. Two GDExtensions, one per format:

| addon | effect class | sidechain send |
| --- | --- | --- |
| `addons/clap_host/` | `ClapPluginEffect` | `ClapSidechainSend` |
| `addons/vst3_host/` | `Vst3PluginEffect` | `Vst3SidechainSend` |

Each addon works on its own, and both can be used in one project.

- Every installed plugin appears in the Audio panel's **Add Effect** menu, next to Godot's own.
- Parameters show as sliders in the inspector, can be set and scheduled from scripts, and are applied on the exact sample.
- The plugin's complete state is saved with the scene.
- Sidechain input from any other bus.
- The plugin's own interface opens in a window, in the editor.
- Plugins can ship with an exported game.

## Requirements

- **Godot 4.5 or later** (tested with 4.5, 4.6 and 4.7).
- One of these platforms. The libraries for all of them ship in the addons.

| platform | architectures | plugin windows (editor) |
| --- | --- | --- |
| Linux | x86_64 (glibc 2.35 or later) | X11, and XWayland on Wayland desktops |
| Windows | x86_64 | yes |
| macOS | universal (Apple silicon and Intel), macOS 10.15 or later | yes |
| Android | arm64 and x86_64, Android 7 (API 24) or later; **CLAP only**, tested with Godot 4.7 | no (exported games only) |

Android has no VST3 plugins, so it has no VST3 host either: the `vst3_host` addon is empty there, and `Vst3PluginEffect` does not exist in an Android game. Web is not supported: hosting means loading native plugin binaries, which a browser cannot.

## Installing

1. Download a release, or the **godot-audio-plugin-host** artifact from a CI run.
2. Copy its `addons/clap_host` and/or `addons/vst3_host` into your project's `addons/` folder.
3. Restart the editor. GDExtensions load by themselves; there is nothing to enable under Project Settings > Plugins.

On macOS, clear the download flag once: see [Platform notes](#macos).

To build from source instead, see [docs/DEVELOPING.md](https://github.com/podulator/godot-audio-plugin-host/blob/main/docs/DEVELOPING.md).

## In the editor

1. Install plugins as usual (see [where plugins are found](#where-plugins-are-found)), or put them in your project's `res://bin/clap` or `res://bin/vst3` folder.
2. Open the **Audio** panel at the bottom of the editor and click **Add Effect** on a bus. Each plugin is listed by format and name: `Clap_DragonflyHallReverb`, `Vst3_SurgeXTEffects`, and so on.
3. Click the effect to see its parameters under **Parameters** in the inspector, and to open the plugin's own window. The window stays open until you close it, as in a DAW.

The menu entries are built when the editor starts. A plugin installed while it runs appears after a restart.

A scene stores only the generic class (`ClapPluginEffect`) and the plugin's path, never the menu entry. So a scene opens on a machine without that plugin, and the effect then passes audio through unchanged.

## From a script

```gdscript
var fx := ClapPluginEffect.new()
fx.plugin_path = "DragonflyHallReverb.clap"   # relative to a plugin folder; absolute paths work too
AudioServer.add_bus_effect(AudioServer.get_bus_index("Reverb"), fx)

if not fx.is_plugin_loaded():
    push_error(fx.get_load_error())

var size := fx.find_parameter("Size")         # parameters are addressed by id
fx.set_parameter(size, 40.0)
print(fx.get_parameter_text(size))            # the plugin's own text, e.g. "40 m"

fx.set_parameter_at(size, 10.0, 0.5)          # in half a second, on the exact sample

for p in ClapPluginEffect.scan_plugins():     # every effect in the plugin folders
    print(p.name, "  ", p.path)
```

- `get_parameters()` lists every parameter (id, name, range, default, flags). `parameter_changed(id, value)` is emitted when the plugin changes one itself, for example from its own window. `parameters_changed` is emitted when a plugin changes its parameter list.
- A `.clap` file can hold several plugins: `plugin_id` picks one (empty means the first).
- `get_plugin_info()` describes the loaded plugin: name, vendor, version, features, ports.
- `open_editor()`, `close_editor()`, `is_editor_open()` and `editor_closed` control the plugin's window (editor only).

Every class, property, method and signal is described in the editor's help: press F1, or use **Search Help**, and look for `ClapPluginEffect`.

### VST 3 differences

`Vst3PluginEffect` has the same API. The format differs in three ways:

- `plugin_path` is the `.vst3` bundle, which is a folder on every platform.
- `plugin_id` is the VST3 class id as 32 hex digits; empty means the first effect in the bundle.
- Parameter values are always **0..1** (VST3 exchanges them normalised). `get_parameter_text()` shows the real value with its unit.

Instruments are left out intentionally: this host runs effects.

## What is saved with a scene

- `plugin_state`: the plugin's own complete state, as bytes only the plugin reads. It brings the plugin back exactly as it was, including settings that are not parameters.
- `param_values`: the parameters set by hand (in the inspector or from a script), by the plugin's stable parameter id. Applied after the state, so they win where the two disagree.

Setting `plugin_state` from a script loads a preset: it replaces every value set by hand so far.

## Sidechains

A plugin with a second input, such as a ducking compressor or a vocoder, can listen to another bus. Put a send on the bus that should drive it, and name the same channel on the effect:

```gdscript
var send := ClapSidechainSend.new()       # on the source bus; it passes that bus through unchanged
send.channel = "kick"
AudioServer.add_bus_effect(AudioServer.get_bus_index("Drums"), send)

fx.sidechain = "kick"                     # on the ClapPluginEffect, on any other bus
```

Use `Vst3SidechainSend` for a `Vst3PluginEffect`. The two formats keep separate channels, so a bus feeding both needs one send of each.

Godot mixes buses from the last one up. If the source bus comes **after** the plugin's bus in the Audio panel, the plugin hears the same block; otherwise it hears it one block (512 frames, about 11 ms) later. When Godot stops processing a silent source bus, the sidechain falls silent too.

## Where plugins are found

In order:

1. Each folder in **Project Settings > Audio > Plugin Host > CLAP** (or **VST3**) **> Search Paths**, by default `res://bin/clap` and `res://bin/vst3`. (Turn on **Advanced Settings** to see the section.)
2. The platform's standard folders: in the editor, and when you run the game from it, if **Use System Paths** is on (the default), so any installed plugin can be tried out. In an exported game only if **Use System Paths In Exports** is on (off by default): a shipped game plays the plugins it ships, and never picks up whatever a player has installed.

| | CLAP | VST3 |
| --- | --- | --- |
| Linux | `~/.clap`, `/usr/lib/clap` | `~/.vst3`, `/usr/lib/vst3`, `/usr/local/lib/vst3` |
| Windows | `%COMMONPROGRAMFILES%\CLAP`, `%LOCALAPPDATA%\Programs\Common\CLAP` | `%COMMONPROGRAMFILES%\VST3`, `%LOCALAPPDATA%\Programs\Common\VST3` |
| macOS | `~/Library/Audio/Plug-Ins/CLAP`, `/Library/Audio/Plug-Ins/CLAP` | `~/Library/Audio/Plug-Ins/VST3`, `/Library/Audio/Plug-Ins/VST3` |

Folders in the `CLAP_PATH` environment variable come before the CLAP ones, as the CLAP standard asks, and `GAPH_VST3_PATH` does the same for VST3. Both take a list (`:` between folders, `;` on Windows).

A scene stores a plugin's path **relative to the folder it was found in**, such as `DragonflyHallReverb.clap`, so it opens on any machine that has the plugin in one of its folders. An absolute path that does not exist on this machine falls back to its file name.

Some VST3 bundles describe themselves in `Contents/Resources/moduleinfo.json`. Those are listed without loading their code at all; the rest are loaded briefly to ask them.

## Shipping plugins with a game

Put the plugins in `res://bin/clap` and `res://bin/vst3` (or any `res://` folder listed in Search Paths). Exporting then puts them where the game looks first, because an operating system cannot load a library from inside a `.pck` file:

| export | the plugins go to |
| --- | --- |
| Linux, Windows | `bin/clap` and `bin/vst3` next to the game's executable |
| macOS | inside the app, in `<game>.app/Contents/PlugIns/` |
| Android | inside the APK, as native libraries (see [Android](#android)) |

Ship the build of each plugin for the platform you export to. On macOS every CLAP and VST3 plugin is a bundle folder, so a single-file `.clap` (a Linux or Windows build) is left out of a macOS export, and the export log says so.

A plugin you auditioned from the system folders is not part of the export. The export lists each effect whose plugin it cannot ship, by bus and file, in the export dialog's messages; in the game, that effect passes audio through unchanged. Turn on **Use System Paths In Exports** for a game that should use plugins the player has installed.

Two things from Godot's macOS exporter (4.5 to 4.7), not from this addon:

- It adds a `Resources/Info.plist` to each plugin bundle and warns "Info.plist missing or invalid". The plugin loads as before.
- It does not sign plugins. If the export is code-signed, ship plugins that are already signed, as vendors' plugins are; an unsigned one makes signing the app fail.

### Android

Android runs native code only from inside the app, so an Android plugin is a shared library that the export puts into the APK. Put each plugin's Android builds in an `android/<abi>/` folder inside a plugin folder, named `lib<name>.so`, next to the desktop `.clap`:

```text
res://bin/clap/la2a.clap                              desktop
res://bin/clap/android/arm64-v8a/libla2a.so           phones and tablets
res://bin/clap/android/x86_64/libla2a.so              the emulator
```

Scenes need no changes: an effect that uses `la2a.clap` loads `libla2a.so` in the Android game. See also the [Android platform notes](#android-1).

## Plugin windows

Clicking a plugin effect in the editor opens the plugin's own interface, in a window of the platform's own kind. The plugin is sized to the editor's display scale (Editor Settings > Interface > Editor > Display Scale), so it matches the editor's text:

| platform | the window | scale |
| --- | --- | --- |
| Linux | an X11 window; on Wayland desktops through XWayland | the editor's display scale |
| Windows | a window that stays above the editor | the editor's display scale |
| macOS | a standard window | set by macOS itself (Retina) |

Plugin windows exist in the editor only, never in an exported game. A plugin that offers no interface has none: its parameters are still in the inspector.

Plugin windows on Windows and macOS are new in this release, and tested in CI with our own test plugins rather than yet with a wide range of commercial ones. Reports of plugins that misbehave are very welcome !!

## Platform notes

### macOS

A build that is not notarised (see [Developing](https://github.com/podulator/godot-audio-plugin-host/blob/main/docs/DEVELOPING.md)) is blocked as downloaded from the internet. Clear the flag once, from the project folder:

```sh
xattr -dr com.apple.quarantine addons/clap_host addons/vst3_host
```

### Android

**At most about 15 plugin libraries per game.** Android gives each app only 128 thread-local keys. Plugins written in Rust (nice-plug, nih-plug) use about 7 per library and never give them back, so after roughly 15 libraries the next one cannot start, and the whole game would abort. The host checks before loading a library, and refuses it with a clear `get_load_error()` instead.

**Workaround:** put several plugins into one CLAP library, which counts once however many plugins it holds. Ship the same bundle on every platform, so a scene refers to the same file and plugin id everywhere.

## Limitations

- **Effects only.** No instruments, no MIDI in or out, and no offline rendering yet.
- **Stereo pairs.** Godot runs one instance of an effect per stereo channel pair, so on a 5.1 or 7.1 bus there are three or four plugin instances, kept in step by the host.
- **Latency.** Parameter changes are heard one audio block (about 11.6 ms at 44.1 kHz) after they are made, evenly spaced. Plugins that report their own latency are not compensated for.
- **Blocks of 512 frames.** That is Godot's mixing block; the host never hands a plugin more than it was prepared for.

## Troubleshooting

**A plugin is missing from the Add Effect menu.** Check that it is an effect (instruments are left out) and that it is in one of the [plugin folders](#where-plugins-are-found), then restart the editor. The Output panel shows a one-line summary of each scan. Every step is logged in `clap_scan.log` and `vst3_scan.log`, in `godot/godot-audio-plugin-host/` inside your user configuration folder:
- `~/.config` on Linux,
- `%APPDATA%` on Windows,
- `~/Library/Application Support` on macOS.

**A plugin crashed the editor.** Plugins are loaded in the editor's own process, so a plugin that crashes takes the editor with it. The host notes each plugin before loading it, so at the next start it knows which one it was: that plugin is skipped, with a warning, and every other plugin is scanned as usual. It is tried again once its file changes, after an update or a reinstall, or when its line is deleted from `clap_scan.cfg` or `vst3_scan.cfg` in the same folder as the logs.

**An effect passes audio through unchanged.** Its plugin was not found or did not load: `is_plugin_loaded()` is false and `get_load_error()` says why. A scene from another machine finds its plugins by name, in this machine's plugin folders.

**The plugin window does not open.** Plugin windows are available in the editor only, and only for enabled effects. On Linux they need an X server; on a Wayland desktop that is XWayland, which most desktops run by default.

**macOS says the library is damaged or cannot be checked.** The quarantine flag: see [Platform notes](#macos).

## Known issues in Godot

- **Freeing an `AudioStreamPlayer` while its `AudioStreamGenerator` is still playing can hang Godot's audio thread** (a Godot bug, tracked as [godotengine/godot#92349](https://github.com/godotengine/godot/issues/92349)). Stop the player and wait a frame or two before freeing it.
- **`godot --headless --import` aborts on a fresh project with any GDExtension** ([godotengine/godot#111645](https://github.com/godotengine/godot/issues/111645)) after the import has finished. Run the import a second time.

## Licences

<img src="https://raw.githubusercontent.com/podulator/godot-audio-plugin-host/v1.0.0/docs/images/VST_Compatible_Logo_Steinberg_with_TM.svg" alt="VST Compatible logo" width="110" align="right" hspace="16">

- This project: MIT, see [LICENSE](LICENSE).
- Compiled into the libraries, all MIT licensed: the CLAP headers, godot-cpp and the VST 3 SDK. Their notices are in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md), which also ships in each addon folder.
- VST is a registered trademark of Steinberg Media Technologies GmbH. The VST Compatible logo is Steinberg's, used under their VST Usage Guidelines.
- The icon is adapted from the [Godot logo](https://github.com/godotengine/godot/tree/master/misc/logo) by Andrea Calabró, licensed under [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/).

## Contributing

Building, the tests, CI, the source layout and the design are in [docs/DEVELOPING.md](https://github.com/podulator/godot-audio-plugin-host/blob/main/docs/DEVELOPING.md). The source is commented: each file opens with what it does and why, and the comments explain the threading rules, the plugin formats' lifecycles, and the less common C++.

Godot Audio Plugin Host is not affiliated with or endorsed by the Godot Foundation.
