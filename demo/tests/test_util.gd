## Shared helpers for the test scenes. Loaded by path (preload), not class_name, so a script
## that uses it works before the project has been re-imported.


## Frees an AudioStreamPlayer whose stream is an AudioStreamGenerator, safely.
##
## Godot's AudioStreamGeneratorPlayback keeps a RAW pointer to its AudioStreamGenerator (checked in
## 4.5 and 4.6: servers/audio/effects/audio_stream_generator.h, line 82). Freeing the player while
## its playback is still being mixed leaves the audio thread reading freed memory: the generator's
## mix rate read -120 instead of 44100, the resampler's step went negative, and its refill loop
## spun forever HOLDING THE AUDIO LOCK, so the next remove_bus() waited forever (Godot 4.5, found
## with a debug build, 12.10.2026). Stopping first, and waiting a few audio blocks so the audio
## thread has let go of the playback, closes that window on any Godot version.
static func free_generator_player(player: AudioStreamPlayer) -> void:
	player.stop()
	await player.get_tree().create_timer(0.1).timeout
	player.queue_free()
	await player.get_tree().process_frame


## The absolute path of one of this repository's own plugins (examples/build, made by
## `scons -C examples test_plugin=yes`, which tests/run_all.sh runs first): "gaph_test.clap",
## "gaph_test_vst3.vst3", "clap_gain.clap" or "vst3_gain.vst3". GAPH_TEST_PLUGINS overrides the folder.
static func own_plugin(file: String) -> String:
	var dir := OS.get_environment("GAPH_TEST_PLUGINS")
	if dir.is_empty():
		dir = ProjectSettings.globalize_path("res://").path_join("../examples/build").simplify_path()
	return dir.path_join(file)
