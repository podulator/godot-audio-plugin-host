extends Node
# Run INSIDE an exported build by tests/export/run.sh. The shipped plugin must come from bin/clap
# next to the executable (res:// is inside the .pck). The unshipped one sits only in a system folder
# (CLAP_PATH), which an exported game does not search by default: it must not load.

func _ready() -> void:
	var fx := ClapPluginEffect.new()
	fx.plugin_path = "clap_gain.clap" # examples/clap-gain
	var ok := fx.is_plugin_loaded()
	var unshipped := ClapPluginEffect.new()
	unshipped.plugin_path = "unshipped_gain.clap"
	print("EXPORTPROBE loaded=%s exported=%s system=%s error='%s'" % [ok, OS.has_feature("template"), unshipped.is_plugin_loaded(), fx.get_load_error()])
	get_tree().quit(0 if ok else 1)
