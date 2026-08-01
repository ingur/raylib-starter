#pragma once
// Entry point of the generated raylib module. src/raylib_bind.cpp is written by
// tools/bindgen.py; rerun ./build.sh bindgen after bumping raylib.

struct lua_State;

// Creates the global `raylib` table with every bound function, enum constant,
// colour and struct constructor, and installs the tagged userdata metatables.
// Call once per lua_State, after the standard libraries and before any script.
void OpenRaylib(lua_State *L);

// Struct names exposed as tagged userdata, in tag order, NULL terminated.
// Feeds lua_CompileOptions::userdataTypes so the compiler can reason about them.
extern const char *const kRaylibUserdataTypes[];
