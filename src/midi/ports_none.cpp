// MIDI ports where there is no backend yet: Android (its MIDI service is a Java API), and any other
// platform. No ports; opening one says why.
#if !defined(_WIN32) && !defined(__APPLE__) && !(defined(__linux__) && !defined(__ANDROID__))

#include "midi_ports.h"

namespace gaph::midi {

static const char *const WHY = "MIDI ports are not supported on this platform yet";

std::vector<PortInfo> list_ports(bool, std::string &r_error) {
	r_error = WHY;
	return {};
}

std::unique_ptr<OutputPort> open_output(const std::string &, std::string &r_error) {
	r_error = WHY;
	return nullptr;
}

std::unique_ptr<InputPort> open_input(const std::string &, InputPort::Callback, std::string &r_error) {
	r_error = WHY;
	return nullptr;
}

const char *ports_backend_name() {
	return "none";
}

} // namespace gaph::midi

#endif
