@tool
extends Node
# Reads the REAL "New ..." list of a player's Stream property in the editor: an
# EditorResourcePicker for AudioStream, which hides virtual classes (GDScript cannot ask
# ClassDB::is_virtual). Loaded as an autoload only through tests/editor_probe.sh, in a headless
# editor. Prints our entries, and what picking the first instrument makes, then quits.

func _ready() -> void:
	if not Engine.is_editor_hint():
		return
	for i in 30:
		await get_tree().process_frame
	var picker := EditorResourcePicker.new()
	picker.base_type = "AudioStream"
	var ours := Array(picker.get_allowed_types()).filter(func(t): return t.begins_with("Clap") or t.begins_with("Vst3"))
	ours.sort()
	print("STREAMPROBE items: ", ", ".join(ours))
	for t in ours:
		if t.begins_with("Clap_") or t.begins_with("Vst3_"):
			var made: Object = ClassDB.instantiate(t) # what the picker does (editor_resource_picker.cpp)
			print("STREAMPROBE %s makes %s, loaded %s" % [t, made.get_class() if made else "null", made.is_plugin_loaded() if made else false])
	picker.free()
	get_tree().quit()
