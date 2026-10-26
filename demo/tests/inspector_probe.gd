@tool
extends Node
# A ClapInstrument and a Vst3Instrument in the real editor's inspector (via tests/editor_probe.sh)
# get an "Open Plugin Window" button at the top. No window opens: the button is only found, not
# pressed. Environment: INSPECTORPROBE_PLUGIN and INSPECTORPROBE_VST3, the test instruments.

func _ready() -> void:
	if not Engine.is_editor_hint():
		return
	await get_tree().process_frame
	# INSPECTORPROBE_PLUGIN for CLAP, INSPECTORPROBE_VST3 for VST3: the same button for both.
	for pair in [["clap", ClapInstrument.new(), "INSPECTORPROBE_PLUGIN"], ["vst3", Vst3Instrument.new(), "INSPECTORPROBE_VST3"]]:
		var inst: AudioStream = pair[1]
		inst.plugin_path = OS.get_environment(pair[2])
		EditorInterface.inspect_object(inst)
		for i in 5:
			await get_tree().process_frame
		var found := _find_button(EditorInterface.get_inspector(), "Open Plugin Window")
		print("INSPECTORPROBE %s button=%s loaded=%s" % [pair[0], found != null, inst.is_plugin_loaded()])
		EditorInterface.inspect_object(null)
	get_tree().quit()


func _find_button(node: Node, text: String) -> Button:
	if node is Button and (node as Button).text == text:
		return node
	for child in node.get_children():
		var b := _find_button(child, text)
		if b:
			return b
	return null
