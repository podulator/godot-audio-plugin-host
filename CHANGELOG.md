# Changelog

## 1.1.1 (unreleased)

- Parameter changes made after a gap in the audio (an Android app back from the background, a laptop waking up, a switched audio device) were heard as much later as the gap was long, and changes scheduled across the gap too. The host now notices the restart and times them from it.

## 1.1.0 (2026-10-24)

Android support, for CLAP plugins.

- The CLAP addon runs in Android games, on arm64 and x86_64, Android 7 (API 24) or later. Put each plugin's Android build in `res://bin/clap/android/<abi>/lib<name>.so` and the export puts it into the APK. Scenes need no changes. See the README's [Android](https://github.com/podulator/godot-audio-plugin-host#android) section.
- Android has no VST3 plugins, so it has no VST3 host either. The VST3 addon is empty there, so a project with both addons exports and starts on Android without errors.
- On Android, a plugin library is refused with a clear load error, rather than allowed to abort the game, when the app's thread-local keys are nearly used up. See [Platform notes](https://github.com/podulator/godot-audio-plugin-host#platform-notes) for the limit and the workaround.
- The release zip holds only the two addon folders, each with its own README, LICENSE and THIRD_PARTY_NOTICES.md, so installing from the editor keeps them with the addon.
- The CLAP host tells plugins its real version (it said 0.1.0), taken from `VERSION`.

## 1.0.0

Initial release, for Linux, macOS and Windows.

Run CLAP and VST® 3 audio effect plugins on Godot's audio buses, in the editor and in exported games. Two GDExtensions, one per format. Each addon works on its own, and both can be used in one project.

- Every installed plugin appears in the Audio panel's Add Effect menu, next to Godot's own.
- Parameters show as sliders in the inspector, can be set and scheduled from scripts, and are applied on the exact sample.
- The plugin's complete state is saved with the scene.
- Sidechain input from any other bus.
- The plugin's own interface opens in a window, in the editor.
- Plugins can ship with an exported game.
