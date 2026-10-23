// Deliberately incomplete, socket-free plugin for the loader's failure gates.
#include <string>
#ifdef __clang__
#pragma clang diagnostic ignored "-Wreturn-type-c-linkage"
#endif
#ifdef _WIN32
#define EXPORT extern "C" __declspec(dllexport)
#else
#define EXPORT extern "C" __attribute__((visibility("default")))
#endif
#ifndef PORCA_FAKE_VERSION
#define PORCA_FAKE_VERSION "02.08.04.00"
#endif
EXPORT std::string bambu_network_get_version() { return PORCA_FAKE_VERSION; }
