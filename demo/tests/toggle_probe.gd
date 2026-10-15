@tool
extends Node
# The bus may already hold effects saved in the project (the user's own): the probe finds its
# own by identity, and removes them again before quitting, so it never alters the saved layout.
#
# Run with DISPLAY unset (tests/editor_probe.sh ... -u DISPLAY): every attempt to open a plugin
# window then logs "cannot open the plugin window", which marks whether the editor tried.
#   A: selecting the effect             -> an attempt, between the A markers
#   B: selecting it AND toggling it in the same frame (what a checkbox click does)
#                                       -> no attempt, between the B markers
#   C: selecting an effect that is switched off -> no attempt, between the C markers

func _ready() -> void:
	if not Engine.is_editor_hint():
		return
	await get_tree().process_frame
	# It needs two real plugins: without a loaded plugin, open_editor() stops before it ever
	# tries a window, and every case would count zero attempts (seen in CI, which has none).
	for path in ["/usr/lib/clap/DragonflyHallReverb.clap", "/usr/lib/clap/DragonflyPlateReverb.clap"]:
		if not FileAccess.file_exists(path):
			print("TOGGLEPROBE SKIP %s not installed" % path)
			get_tree().quit()
			return
	var a := ClapPluginEffect.new()
	a.plugin_path = "/usr/lib/clap/DragonflyHallReverb.clap"
	var b := ClapPluginEffect.new()
	b.plugin_path = "/usr/lib/clap/DragonflyPlateReverb.clap"
	AudioServer.add_bus_effect(1, a)
	AudioServer.add_bus_effect(1, b)
	for i in 3:
		await get_tree().process_frame

	print("TOGGLEPROBE A begin")
	# A's enabled state must not change here; only B toggles.
	EditorInterface.edit_resource(a)
	for i in 3:
		await get_tree().process_frame
	print("TOGGLEPROBE A end")

	print("TOGGLEPROBE B begin")
	var slot := -1
	for i in AudioServer.get_bus_effect_count(1):
		if AudioServer.get_bus_effect(1, i) == b:
			slot = i
	print("TOGGLEPROBE b is at bus 1 slot %d of %d (bus 1 = %s)" % [slot, AudioServer.get_bus_effect_count(1), AudioServer.get_bus_name(1)])
	EditorInterface.edit_resource(b)
	AudioServer.set_bus_effect_enabled(1, slot, false) # same frame, as the checkbox does
	print("TOGGLEPROBE enabled right after toggling: ", AudioServer.is_bus_effect_enabled(1, slot))
	for i in 3:
		await get_tree().process_frame
	print("TOGGLEPROBE B end")

	print("TOGGLEPROBE C begin") # b is switched off now
	EditorInterface.edit_resource(a)
	for i in 3:
		await get_tree().process_frame
	EditorInterface.edit_resource(b)
	for i in 3:
		await get_tree().process_frame
	print("TOGGLEPROBE C end")
	for fx in [b, a]:
		for i in range(AudioServer.get_bus_effect_count(1) - 1, -1, -1):
			if AudioServer.get_bus_effect(1, i) == fx:
				AudioServer.remove_bus_effect(1, i)
	get_tree().quit()
