@tool
extends Node
# A ClapInstrument in the real editor's inspector (via tests/editor_probe.sh) gets an "Open Plugin
# Window" button at the top. No window opens: the button is only found, not pressed.
# Environment: INSPECTORPROBE_PLUGIN, the test instrument's path.

func _ready() -> void:
	if not Engine.is_editor_hint():
		return
	await get_tree().process_frame
	var inst := ClapInstrument.new()
	inst.plugin_path = OS.get_environment("INSPECTORPROBE_PLUGIN")
	EditorInterface.inspect_object(inst)
	for i in 5:
		await get_tree().process_frame
	var found := _find_button(EditorInterface.get_inspector(), "Open Plugin Window")
	print("INSPECTORPROBE button=%s loaded=%s" % [found != null, inst.is_plugin_loaded()])
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
