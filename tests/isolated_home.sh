# Sourced by every test script that starts Godot, Wine or a plugin host: points HOME and the XDG
# folders at a scratch folder, so no test reads or writes the user's real home directory (Godot
# keeps its editor settings, project data and our scan logs there; Wine its menus and caches).
# The one thing needed from the real home is Godot's export templates: linked in, read only.
# Runs once: a script started from run_all.sh finds it done (GAPH_TEST_HOME is set).
if [ -z "$GAPH_TEST_HOME" ]; then
	case "$(uname -s)" in
	MINGW* | MSYS* | CYGWIN*) ;; # Windows: Godot uses %APPDATA%, which HOME does not move
	*)
		real_templates="${XDG_DATA_HOME:-$HOME/.local/share}/godot/export_templates"
		real_mac_templates="$HOME/Library/Application Support/Godot/export_templates"
		export GAPH_TEST_HOME="${GAPH_TEST_TMP:-${TMPDIR:-/tmp}/godot-audio-plugin-host-tests}/home"
		mkdir -p "$GAPH_TEST_HOME/home" "$GAPH_TEST_HOME/config" "$GAPH_TEST_HOME/data/godot" "$GAPH_TEST_HOME/cache"
		if [ -d "$real_templates" ] && [ ! -e "$GAPH_TEST_HOME/data/godot/export_templates" ]; then
			ln -s "$real_templates" "$GAPH_TEST_HOME/data/godot/export_templates"
		fi
		mac_dir="$GAPH_TEST_HOME/home/Library/Application Support/Godot"
		if [ -d "$real_mac_templates" ] && [ ! -e "$mac_dir/export_templates" ]; then
			mkdir -p "$mac_dir"
			ln -s "$real_mac_templates" "$mac_dir/export_templates"
		fi
		export HOME="$GAPH_TEST_HOME/home" XDG_CONFIG_HOME="$GAPH_TEST_HOME/config"
		export XDG_DATA_HOME="$GAPH_TEST_HOME/data" XDG_CACHE_HOME="$GAPH_TEST_HOME/cache"
		unset real_templates real_mac_templates mac_dir
		;;
	esac
fi
