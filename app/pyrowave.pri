# =============================================================================
# PyroWave codec
#
# Built out of tree, exactly like the host does it (Apollo/cmake/dependencies/
# pyrowave.cmake). It is not vendored: the codec's own build pulls in Granite and
# ~20 submodules, and re-vendoring that would put a second source of truth for the
# same ABI in this repository.
#
# The C API lives in the codec's `pyrowave-shared` target. Its `pyrowave` static
# library is the C++ core and exports no pyrowave_* symbols at all, so linking that
# one produces "unresolved external symbol" on every entry point. This links
# `pyrowave-shared.lib` and copies the DLL next to Moonlight.exe.
#
# Set PYROWAVE_ROOT in the environment (or pass it to qmake) to point at a checkout:
#   set PYROWAVE_ROOT=C:\path\to\PyroWave
# Without it the decoder is simply not built, and the client keeps working for every
# other codec.
# =============================================================================

isEmpty(PYROWAVE_ROOT) {
    PYROWAVE_ROOT = $$(PYROWAVE_ROOT)
}

isEmpty(PYROWAVE_ROOT) {
    # Sibling checkouts of the workspace, nearest first. qmake's for() takes a
    # variadic list, not a comma separated one, so the candidates are built as a
    # list first.
    PYROWAVE_CANDIDATES = \
        $$PWD/../PyroWave \
        $$PWD/../../PyroWave \
        $$PWD/../../../PyroWave \
        $$PWD/../../../../PyroWave

    for (candidate, PYROWAVE_CANDIDATES) {
        exists($$candidate/pyrowave.h) {
            PYROWAVE_ROOT = $$candidate
        }
    }
}

!exists($$PYROWAVE_ROOT/pyrowave.h) {
    # Not an error. The client must build and work without the codec; only the
    # PyroWave stream becomes unavailable, and that is a far better failure mode than
    # a build that cannot proceed at all.
    pyrowave_enabled =
    message("PyroWave: no checkout found (set PYROWAVE_ROOT), decoder disabled")
    return()
}

# MSVC build directory. Moonlight-Qt is a qmake project and only builds with MSVC,
# so the ucrt64 build of the codec is not usable here.
PYROWAVE_BUILD_DIR = $$PYROWAVE_ROOT/build-msvc

!exists($$PYROWAVE_BUILD_DIR/pyrowave-shared.lib) {
    message("PyroWave: $$PYROWAVE_BUILD_DIR/pyrowave-shared.lib is missing. Build it first: cmake -S <root> -B build-msvc -DBUILD_TESTS=OFF -DBUILD_SHARED_LIBS=ON")
    pyrowave_enabled =
    return()
}

message("PyroWave: using $$PYROWAVE_ROOT (build $$PYROWAVE_BUILD_DIR)")
pyrowave_enabled = true

PYROWAVE_DLL = $$PYROWAVE_BUILD_DIR/libpyrowave-shared-0.dll

INCLUDEPATH += \
    $$PYROWAVE_ROOT \
    $$PYROWAVE_ROOT/eval-results \
    $$PYROWAVE_ROOT/Granite/third_party/khronos/vulkan-headers/include

# Full path rather than -L/-l. qmake translates -L into a /LIBPATH for msvc, but the
# linker then still searches by name and, for a name that is not a known system
# library, does not reliably find it. A full path is unambiguous.
LIBS += "$$PYROWAVE_BUILD_DIR/pyrowave-shared.lib"

# The codec DLL must sit next to the executable or the client fails to start with
# 0xC0000135 and no message. The project keeps runtime DLLs in release/ or debug/
# depending on the configuration (same layout the LIBS paths above assume), and
# OUT_PWD is one level above that, so the configuration has to be resolved rather
# than hardcoded.
CONFIG(debug, debug|release) {
    PYROWAVE_DESTDIR = $$OUT_PWD/debug
} else {
    PYROWAVE_DESTDIR = $$OUT_PWD/release
}

# No mkdir and no `||` fallback: the destination directory is created by the link
# step that runs immediately before this, and both a chained `&` and a trailing `||`
# get mangled when qmake hands the line to a shell. A plain copy either works or
# prints copy's own error, which is clearer than a suppressed one.
# qmix leaves forward slashes in paths it builds itself, and cmd's copy rejects a
# mixed path like "C:\a\b/root/file" with "the syntax of the command is incorrect"
# (exit 1) even though nothing else is wrong with it. Normalizing once, here, is
# cheaper than discovering it as a mysterious post-link failure.
PYROWAVE_ROOT = $$replace(PYROWAVE_ROOT, /, \\)
PYROWAVE_BUILD_DIR = $$replace(PYROWAVE_BUILD_DIR, /, \\)
PYROWAVE_DESTDIR = $$replace(PYROWAVE_DESTDIR, /, \\)
PYROWAVE_DLL = $$replace(PYROWAVE_DLL, /, \\)

# Run through the same cmd nmake already uses rather than nesting a second `cmd /c`,
# and name the destination file explicitly rather than relying on copy's
# "target is a directory" rule, which is what failed here.
win32-msvc*:QMAKE_POST_LINK = \
    cmd /c copy /y \"$$PYROWAVE_DLL\" \"$$PYROWAVE_DESTDIR\\libpyrowave-shared-0.dll\"
