/* A CLAP "plugin" that crashes as soon as it is initialised: the scan guard's test subject. */
#include <clap/clap.h>
#include <stdlib.h>

static bool crash_init(const char *path) {
	(void)path;
	abort(); /* SIGABRT, the same way a real plugin bug would end the editor. */
}
static void crash_deinit(void) {}
static const void *crash_get_factory(const char *id) {
	(void)id;
	return NULL;
}

CLAP_EXPORT const clap_plugin_entry_t clap_entry = { CLAP_VERSION_INIT, crash_init, crash_deinit, crash_get_factory };
