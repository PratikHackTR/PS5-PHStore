/* Forced into every PS5 translation unit by tools/build.ps1.
 * Upstream checks PS5_BUILD or __Prospero__, while Clang emits __PROSPERO__.
 * Never allow a console artifact to silently select upstream host mocks. */
#ifndef PHSTORE_BUILD_GUARD_H
#define PHSTORE_BUILD_GUARD_H
#if !defined(__PROSPERO__)
#error "PH Store console build requires the PS5 compiler target"
#endif
#if !defined(PS5_BUILD) || PS5_BUILD != 1
#error "PS5_BUILD=1 is required: upstream installer host simulation must not ship"
#endif
#endif
