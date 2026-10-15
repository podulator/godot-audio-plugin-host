// The VST3 SDK's ConnectionProxy needs a ThreadChecker (it asserts messages stay on the thread
// that connected them), but the SDK only ships one for Linux and one for Windows: on macOS the
// symbol is missing and the VST3 library fails to link. This single version replaces both. It is
// the SDK's Linux checker with std::thread::id in place of pthread_t, which works everywhere.

#include "public.sdk/source/common/threadchecker.h"

#include <cstdio>
#include <exception>
#include <thread>

namespace Steinberg {
namespace Vst {

class PortableThreadChecker : public ThreadChecker {
public:
	bool test(const char *failmessage = nullptr, bool exit = false) override {
		if (thread == std::this_thread::get_id()) {
			return true;
		}
		if (failmessage) {
			std::fprintf(stderr, "%s", failmessage);
		}
		if (exit) {
			std::terminate();
		}
		return false;
	}

private:
	std::thread::id thread = std::this_thread::get_id();
};

std::unique_ptr<ThreadChecker> ThreadChecker::create() {
	return std::unique_ptr<ThreadChecker>(new PortableThreadChecker);
}

} // namespace Vst
} // namespace Steinberg
