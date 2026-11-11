#ifndef QT_COMMANDER_VERSION_H
#define QT_COMMANDER_VERSION_H

// The release version is compiled in from the git tag by CMake
// (QTC_VERSION in the top-level CMakeLists.txt). Nothing in the C++ sources
// may hardcode a version: a running process reports exactly the release it
// was built from, which is the only way a client can tell how old an already
// injected runtime is.
#ifndef QT_COMMANDER_VERSION
#define QT_COMMANDER_VERSION "0.0.0+unknown"
#endif

#endif // QT_COMMANDER_VERSION_H
