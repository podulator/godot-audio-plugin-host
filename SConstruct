#!/usr/bin/env python
# Builds two independent GDExtensions from one tree:
#   clap_host  -> demo/addons/clap_host/bin/
#   vst3_host  -> demo/addons/vst3_host/bin/
# src/core (and the header-only src/godot_common) is compiled into both. Neither registers any
# Godot class, because two extensions registering the same class name is a hard load failure.

# The oldest macOS the libraries run on. godot-cpp's own default is whatever the build machine's
# Xcode targets, which on a current Mac means only current macOS. 10.15 is the floor our code sets:
# std::filesystem (plugin folders, the VST3 SDK's module loader) exists in macOS's C++ library
# from 10.15 on. Ignored on other platforms; override with macos_deployment_target=... .
ARGUMENTS.setdefault("macos_deployment_target", "10.15")

env = SConscript("thirdparty/godot-cpp/SConstruct")
env.Append(CPPPATH=["#src/"])

# One version for everything that carries one: VERSION. It goes into the macOS frameworks'
# Info.plist, and into the code as GAPH_VERSION_NUMBER (the version the CLAP host tells plugins).
# Unquoted on purpose: a quoted string survives the compiler command line differently on GCC and
# MSVC, so the code turns the bare number into a string itself (clap_processor.cpp).
with open(File("#VERSION").abspath) as f:
    version = f.read().strip()
env.Append(CPPDEFINES=[("GAPH_VERSION_NUMBER", version)])


def macos_info_plist(framework, name):
    """A macOS framework's Resources/Info.plist. Code signing needs one: it names the bundle and its
    binary, and is sealed into the signature. Godot reads it too, and writes a stand-in with a
    warning when it is missing (macOS exports)."""
    return env.Textfile(framework + "/Resources/Info.plist", [
        '<?xml version="1.0" encoding="UTF-8"?>',
        '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">',
        '<plist version="1.0">',
        "<dict>",
        "\t<key>CFBundleInfoDictionaryVersion</key><string>6.0</string>",
        "\t<key>CFBundleDevelopmentRegion</key><string>en</string>",
        "\t<key>CFBundleExecutable</key><string>{}</string>".format(name),
        "\t<key>CFBundleName</key><string>{}</string>".format(name),
        "\t<key>CFBundleIdentifier</key><string>org.godot-audio-plugin-host.{}</string>".format(name.replace("_", "-")),
        "\t<key>CFBundleVersion</key><string>{}</string>".format(version),
        "\t<key>CFBundleShortVersionString</key><string>{}</string>".format(version),
        "\t<key>CFBundlePackageType</key><string>FMWK</string>",
        "\t<key>CFBundleSupportedPlatforms</key><array><string>MacOSX</string></array>",
        "\t<key>LSMinimumSystemVersion</key><string>{}</string>".format(ARGUMENTS["macos_deployment_target"]),
        "</dict>",
        "</plist>",
    ], TEXTFILESUFFIX="")


def vst3_sdk_sources():
    """The SDK files listed in vst3_sdk_sources.txt, with the module loader for this platform."""
    loader = {"windows": "module_win32.cpp", "macos": "module_mac.mm"}.get(env["platform"], "module_linux.cpp")
    out = []
    with open(File("#vst3_sdk_sources.txt").abspath) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            line = line.replace("module_linux.cpp", loader)
            out.append("thirdparty/vst3sdk/" + line)
    return out


def build_extension(name, source_dirs, extra_cpppath=(), extra_sources=(), extra_defines=(), extra_flags=(), extra_link=None, exceptions=False, docs_prefix=None):
    lib_env = env.Clone()
    if exceptions:
        # Undo godot-cpp's switch-off for this library: the -fno-exceptions flag (GCC, Clang) or
        # the _HAS_EXCEPTIONS=0 define (MSVC). See the VST3 notes below for why.
        lib_env["CXXFLAGS"] = [f for f in lib_env["CXXFLAGS"] if f != "-fno-exceptions"]
        lib_env["CPPDEFINES"] = [d for d in lib_env["CPPDEFINES"] if not (isinstance(d, (tuple, list)) and d[0] == "_HAS_EXCEPTIONS")]
    lib_env.Append(CPPPATH=list(extra_cpppath), CPPDEFINES=list(extra_defines), CXXFLAGS=list(extra_flags))
    if extra_link:
        lib_env.Append(**extra_link)
    # The plugin window (src/core/native_window.h), in both libraries. Windows: user32 (windows,
    # messages) and gdi32 (the background brush). macOS: AppKit, from Objective-C++ that is built
    # with ARC (cocoa_window.mm checks); -fobjc-arc does nothing to the C++ files.
    if env["platform"] == "windows":
        lib_env.Append(LIBS=["user32", "gdi32"])
    elif env["platform"] == "macos":
        lib_env.Append(CCFLAGS=["-fobjc-arc"], LINKFLAGS=["-framework", "Cocoa"])
    # One object folder per library, platform, target and architecture. Debug and release objects
    # differ (debug has godot-cpp's hot-reload support), and sharing paths once linked debug
    # objects into a release library: "undefined symbol ...Wrapped::_constructing_recreate_owner"
    # when an exported release game loaded it (seen 12.10.2026, with a stale .sconsign).
    objdir = "build/{}/{}.{}.{}".format(name, env["platform"], env["target"], env["arch"])
    sources = []
    # .mm (Objective-C++) only on macOS: src/core/cocoa_window.mm, the plugin window there.
    patterns = ["/*.cpp", "/*.mm"] if env["platform"] == "macos" else ["/*.cpp"]
    for d in ["src/core"] + list(source_dirs):
        for f in [g for p in patterns for g in Glob(d + p)]:
            # Each library gets its own object files, since the environments differ.
            sources.append(lib_env.SharedObject("{}/{}/{}".format(objdir, d, f.name.rsplit(".", 1)[0]), f))
    for path in extra_sources:
        sources.append(lib_env.SharedObject("{}/{}".format(objdir, path.rsplit(".", 1)[0]), "#" + path))

    # The class reference (F1 in the editor, and the inspector's tooltips), compiled in from
    # doc_classes/<Prefix>*.xml. Each library carries only its own format's classes, and only in
    # debug and editor builds, which are the ones the editor loads; release builds stay lean.
    # godot-cpp's GodotCPPDocData builder turns the XML into a generated .cpp.
    if docs_prefix and env["target"] in ["editor", "template_debug"]:
        xml = sorted(Glob("doc_classes/{}*.xml".format(docs_prefix)))
        if xml:
            sources.append(lib_env.GodotCPPDocData("{}/doc_data.gen.cpp".format(objdir), source=xml))

    outdir = "demo/addons/{}/bin".format(name)
    if env["platform"] == "macos":
        target = "{}/lib{}.{}.{}.framework/lib{}.{}.{}".format(
            outdir, name, env["platform"], env["target"], name, env["platform"], env["target"]
        )
    else:
        target = "{}/lib{}{}{}".format(outdir, name, env["suffix"], env["SHLIBSUFFIX"])
    library = lib_env.SharedLibrary(target, source=sources)
    Default(library)
    if env["platform"] == "macos":
        Default(macos_info_plist(target.rsplit("/", 1)[0], target.rsplit("/", 1)[1]))
    return library


library_clap = build_extension(
    "clap_host",
    ["src/clap"],
    ["#thirdparty/clap/include"],
    extra_link={} if env["platform"] in ("windows", "macos") else {"LIBS": ["dl"]},
    docs_prefix="Clap",
)

# The VST3 SDK insists on one of RELEASE or DEVELOPMENT being defined (pluginterfaces/base/
# fplatform.h), and some of its files use std::wstring_convert, deprecated since C++17 but still
# working; that warning is the SDK's to fix, so it is silenced rather than worked around.
#
# EXCEPTIONS. godot-cpp builds with them off. Two SDK files use exceptions internally and catch
# everything they throw: module_linux.cpp (a std::filesystem walk inside try/catch(...)) and
# moduleinfoparser.cpp (parseJson catches its own parse_error). So the VST3 library is built with
# exceptions ON, all of it: on MSVC, godot-cpp switches them off with _HAS_EXCEPTIONS=0, which
# changes the standard library's own code, so turning them back on for a few files would mix two
# incompatible builds of the STL in one binary. Our own code throws nothing either way, and none
# can propagate out of the SDK. The CLAP library stays exception-free.
vst3_env_defines = [("RELEASE", 1)]
vst3_link = {}
if env.get("is_msvc", False):
    vst3_flags = ["/EHsc"]
else:
    vst3_flags = ["-fexceptions", "-Wno-deprecated-declarations"]
    if env.get("use_llvm", False) or env["platform"] == "macos":
        # Clang (and Apple's): the SDK's headers set #pragma pack on purpose, because the VST3
        # ABI fixes struct layouts, and clang warns about every header that does it.
        vst3_flags += ["-Wno-pragma-pack"]
if env["platform"] == "windows":
    # module_win32.cpp: SHGetKnownFolderPath (shell32) and CoTaskMemFree (ole32).
    vst3_link = {"LIBS": ["ole32", "shell32"]}
    if not env.get("is_msvc", False):
        vst3_link["LIBS"] += ["uuid"]
elif env["platform"] == "macos":
    # module_mac.mm is Objective-C++ that requires ARC too (build_extension adds -fobjc-arc and
    # Cocoa for every library on macOS).
    vst3_link = {"LINKFLAGS": ["-framework", "CoreFoundation"]}
else:
    vst3_link = {"LIBS": ["dl"]}



# Android: CLAP only. The VST 3 SDK does not support Android, and there are no VST 3 plugins for it.
# vst3_host gets an empty extension instead (src/vst3_android/stub.c says why).
if env["platform"] == "android":
    stub_env = env.Clone()
    stub_env.Append(CPPPATH=["#thirdparty/godot-cpp/gdextension"])
    stub = stub_env.SharedLibrary(
        "demo/addons/vst3_host/bin/libvst3_host{}{}".format(env["suffix"], env["SHLIBSUFFIX"]),
        source=[stub_env.SharedObject("build/vst3_host/{}.{}.{}/stub".format(env["platform"], env["target"], env["arch"]), "src/vst3_android/stub.c")],
    )
    Default(stub)
    Return()

library_vst3 = build_extension(
    "vst3_host",
    ["src/vst3"],
    ["#thirdparty/vst3sdk"],
    extra_sources=vst3_sdk_sources(),
    extra_defines=vst3_env_defines,
    extra_flags=vst3_flags,
    extra_link=vst3_link,
    exceptions=True,
    docs_prefix="Vst3",
)
