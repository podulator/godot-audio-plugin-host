#pragma once

// In the editor: .mid and .midi2 files in the project are imported as MidiClip resources, so
// load("res://music/theme.mid") gives a MidiClip, and an exported game carries the clip, not the
// file. One import, no options; a file the reader refuses fails its import with the reason.

#include "midi_clip_resource.h"

#include "core/midi_file.h"

#include <godot_cpp/classes/editor_import_plugin.hpp>
#include <godot_cpp/classes/editor_plugin.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/resource_saver.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

namespace godot {

class MidiClipImporter : public EditorImportPlugin {
	GDCLASS(MidiClipImporter, EditorImportPlugin);

public:
	String _get_importer_name() const override { return "midi_host.clip"; }
	String _get_visible_name() const override { return "MIDI Clip"; }
	PackedStringArray _get_recognized_extensions() const override { return PackedStringArray({ "mid", "midi", "smf", "midi2" }); }
	String _get_save_extension() const override { return "res"; }
	String _get_resource_type() const override { return "MidiClip"; }
	int32_t _get_preset_count() const override { return 1; }
	String _get_preset_name(int32_t) const override { return "Default"; }
	TypedArray<Dictionary> _get_import_options(const String &, int32_t) const override { return TypedArray<Dictionary>(); }
	bool _get_option_visibility(const String &, const StringName &, const Dictionary &) const override { return true; }
	float _get_priority() const override { return 1.0f; }
	int32_t _get_import_order() const override { return 0; }
	int32_t _get_format_version() const override { return 1; }
	bool _can_import_threaded() const override { return true; } // touches nothing shared

	Error _import(const String &p_source_file, const String &p_save_path, const Dictionary &, const TypedArray<String> &, const TypedArray<String> &) const override {
		const PackedByteArray bytes = FileAccess::get_file_as_bytes(p_source_file);
		ERR_FAIL_COND_V_MSG(bytes.is_empty(), ERR_FILE_CANT_READ, vformat("MidiClip import: cannot read '%s'.", p_source_file));
		gaph::midi::Clip c;
		std::string err;
		ERR_FAIL_COND_V_MSG(!gaph::midi::read_midi_file(bytes.ptr(), size_t(bytes.size()), c, err), ERR_FILE_CORRUPT,
				vformat("MidiClip import: '%s': %s.", p_source_file, String::utf8(err.c_str())));
		Ref<MidiClip> clip;
		clip.instantiate();
		clip->set_clip(std::move(c));
		return ResourceSaver::get_singleton()->save(clip, p_save_path + String(".") + _get_save_extension());
	}

protected:
	static void _bind_methods() {}
};

// The editor plugin that installs the importer; nothing else.
class MidiEditorPlugin : public EditorPlugin {
	GDCLASS(MidiEditorPlugin, EditorPlugin);

public:
	void _enter_tree() override {
		importer.instantiate();
		add_import_plugin(importer);
	}
	void _exit_tree() override {
		remove_import_plugin(importer);
		importer.unref();
	}

protected:
	static void _bind_methods() {}

private:
	Ref<MidiClipImporter> importer;
};

} // namespace godot
