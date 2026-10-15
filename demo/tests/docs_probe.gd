@tool
extends Node
# Inside the real editor (tests/editor_probe.sh): is our class reference loaded? Opens the help
# page for ClapPluginEffect (what F1 shows) and checks it for sentences from doc_classes/.
# The editor builds its help in the background at start, so this waits for the page to fill.

const NEEDLES := ["Runs a CLAP audio plugin", "sample-accurate", "Godot runs one instance",
		"ready-made plugin instances", "Emitted when the plugin's window closes"]


func _ready() -> void:
	if not Engine.is_editor_hint():
		return
	var page := ""
	var deadline := Time.get_ticks_msec() + 20000
	while Time.get_ticks_msec() < deadline:
		await get_tree().create_timer(0.5).timeout
		EditorInterface.get_script_editor().goto_help("class_name:ClapPluginEffect")
		await get_tree().process_frame
		page = _find_page(EditorInterface.get_base_control())
		if page.contains(NEEDLES[0]):
			break
	print("DOCSPROBE page length ", page.length())
	for n in NEEDLES:
		print("DOCSPROBE ", "has   " if page.contains(n) else "LACKS ", n)
	get_tree().quit()


# The help page: the RichTextLabel whose text begins with the class header.
func _find_page(n: Node) -> String:
	if n is RichTextLabel:
		var t: String = n.get_parsed_text()
		if t.contains("Inherits:") and t.contains("ClapPluginEffect"):
			return t
	for c in n.get_children(true):
		var r := _find_page(c)
		if r != "":
			return r
	return ""
