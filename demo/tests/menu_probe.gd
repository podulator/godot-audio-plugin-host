@tool
extends Node
# Reads the REAL "Add Effect" menu of the editor's Audio bus panel. Loaded as an autoload only
# through tests/editor_probe.sh, in a headless editor. Prints the items and
# quits. Needed because GDScript cannot call ClassDB::is_virtual, which the menu filters on.

func _ready() -> void:
	if not Engine.is_editor_hint():
		return
	for i in 30:
		await get_tree().process_frame
	var menus: Array = []
	_find_effect_menus(EditorInterface.get_base_control(), menus)
	if menus.is_empty():
		print("MENUPROBE no Add Effect menu found")
	else:
		var m: PopupMenu = menus[0]
		var items := []
		for i in m.item_count:
			items.append(m.get_item_text(i))
		print("MENUPROBE items: ", ", ".join(items))
	get_tree().quit()


# The bus panel's effect menu is the PopupMenu whose items include Godot's own "Reverb".
func _find_effect_menus(n: Node, out: Array) -> void:
	if n is PopupMenu:
		for i in n.item_count:
			if n.get_item_text(i) == "Reverb":
				out.append(n)
				return
	for c in n.get_children(true):
		_find_effect_menus(c, out)
