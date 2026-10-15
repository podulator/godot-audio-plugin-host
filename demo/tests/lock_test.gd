extends Node
# How long adding a plugin effect holds Godot's audio lock:
#   godot --headless --path demo res://tests/lock_test.tscn
# AudioServer.add_bus_effect() runs _instantiate() under the lock, so its duration is (an upper
# bound on) how long audio is held up. Nothing slow may happen there any more: no plugin is
# loaded, created or destroyed under the lock (see HostedEffect::attach).

var failures := 0


func _ready() -> void:
	var hall := "/usr/lib/clap/DragonflyHallReverb.clap"
	if not FileAccess.file_exists(hall):
		print("SKIP  Dragonfly not installed")
		print("LOCK OK (1 skipped)")
		get_tree().quit(0)
		return

	# How long creating one instance takes, i.e. what used to happen under the lock.
	var probe := ClapPluginEffect.new()
	var t0 := Time.get_ticks_usec()
	probe.plugin_path = hall
	probe.is_plugin_loaded()
	var load_us := Time.get_ticks_usec() - t0
	print("      loading Hall (library + one instance) takes %d us" % load_us)

	AudioServer.add_bus(1)
	AudioServer.add_bus(2)

	# A fresh effect whose plugin is not loaded yet: the very first add.
	var fx := ClapPluginEffect.new()
	fx.plugin_path = hall
	t0 = Time.get_ticks_usec()
	AudioServer.add_bus_effect(1, fx)
	var first_us := Time.get_ticks_usec() - t0
	_check(first_us < 2000, "first add_bus_effect holds the lock %d us (no plugin created under it)" % first_us)
	_check(fx.get_active_instance_count() == 0, "  ...and the bus passes audio through meanwhile")
	for i in 5:
		await get_tree().process_frame
	_check(fx.get_active_instance_count() == 1, "the plugin arrives a few frames later, built outside the lock")

	# A second bus: picks up the spare made in the meantime.
	t0 = Time.get_ticks_usec()
	AudioServer.add_bus_effect(2, fx)
	var spare_us := Time.get_ticks_usec() - t0
	_check(fx.get_active_instance_count() == 2, "second bus takes the spare: running at once")
	_check(spare_us < 2000, "  ...holding the lock %d us" % spare_us)

	# Removing: the plugin is destroyed in the next idle, not under the lock.
	t0 = Time.get_ticks_usec()
	AudioServer.remove_bus_effect(2, 0)
	var remove_us := Time.get_ticks_usec() - t0
	_check(remove_us < 2000, "remove_bus_effect holds the lock %d us (plugin destroyed later)" % remove_us)
	await get_tree().process_frame
	print("LOCK %s" % ("OK" if failures == 0 else "FAILED (%d)" % failures))
	get_tree().quit(1 if failures else 0)


func _check(ok: bool, what: String) -> void:
	print(("PASS  " if ok else "FAIL  ") + what)
	if not ok:
		failures += 1
