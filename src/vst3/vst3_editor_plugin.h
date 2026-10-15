#pragma once

// Editor-only glue: an EditorPlugin with two jobs. It opens a plugin's window when its effect
// is selected (clicking it on a bus shows it in the inspector, which asks every EditorPlugin
// whether it _handles the object, then calls _edit), and leaves it open when something else is
// selected, as plugin windows in a DAW do. And it adds the export step that copies the
// project's VST3 plugins beside the exported game (godot_common/export_copy.h). Registered at
// the EDITOR initialisation level, which never runs in an exported game.

#include "vst3_plugin_effect.h"
#include "godot_common/effect_toggle.h"
#include "godot_common/export_copy.h"

#include <godot_cpp/classes/editor_export_platform.hpp>
#include <godot_cpp/classes/editor_export_plugin.hpp>
#include <godot_cpp/classes/editor_plugin.hpp>

namespace godot {

class Vst3ExportPlugin : public EditorExportPlugin {
	GDCLASS(Vst3ExportPlugin, EditorExportPlugin);

public:
	String _get_name() const override { return "Vst3PluginHostExport"; }
	bool _supports_platform(const Ref<EditorExportPlatform> &) const override { return true; }
	void _export_begin(const PackedStringArray &p_features, bool, const String &p_path, uint32_t) override {
		gaph_godot::export_plugin_folders("vst3", p_path, p_features, this);
	}
	void _export_file(const String &p_path, const String &, const PackedStringArray &) override {
		gaph_godot::warn_unshipped_plugins("vst3", "Vst3PluginEffect", p_path, this);
	}

protected:
	static void _bind_methods() {}
};

class Vst3EditorPlugin : public EditorPlugin {
	GDCLASS(Vst3EditorPlugin, EditorPlugin);

public:
	bool _handles(Object *p_object) const override { return Object::cast_to<Vst3PluginEffect>(p_object) != nullptr; }
	void _edit(Object *p_object) override {
		// _edit(nullptr) means "something else is selected now": the window stays open (3b).
		Vst3PluginEffect *fx = Object::cast_to<Vst3PluginEffect>(p_object);
		if (!fx) {
			return;
		}
		// Not at once: a click on the enable checkbox selects the row too (effect_toggle.h).
		pending = Ref<Vst3PluginEffect>(fx);
		pending_enabled = gaph_godot::effect_enabled_snapshot(pending);
		callable_mp(this, &Vst3EditorPlugin::open_pending).call_deferred();
	}

	void _enter_tree() override {
		exporter.instantiate();
		add_export_plugin(exporter);
	}
	void _exit_tree() override {
		remove_export_plugin(exporter);
		exporter.unref();
	}

protected:
	static void _bind_methods() {}

private:
	void open_pending() {
		Ref<Vst3PluginEffect> fx = pending;
		pending.unref();
		if (fx.is_null()) {
			return;
		}
		const std::vector<bool> now = gaph_godot::effect_enabled_snapshot(fx);
		// Opens only for a click on the effect itself (the enabled state didn't change), and only
		// if the effect is enabled: a switched-off effect stays closed.
		if (now == pending_enabled && gaph_godot::any_enabled(now)) {
			fx->open_editor();
		}
	}

	Ref<Vst3ExportPlugin> exporter;
	Ref<Vst3PluginEffect> pending;
	std::vector<bool> pending_enabled;
};

} // namespace godot
