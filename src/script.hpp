#pragma once

#include <string>

struct lua_State;

// Owns one Luau VM. Reset() throws the old state away and boots a fresh one,
// which is the whole hot reload story: nothing survives a reload except the
// string the host carries across by hand.
class Script {
public:
    Script() = default;
    ~Script();
    Script(const Script &) = delete;
    Script &operator=(const Script &) = delete;

    bool Reset(bool devMode);
    void Close();

    // Compiles and runs a vfs script as the entry chunk. `missing` reports a
    // file that is not there, as opposed to one that failed.
    bool RunEntry(const char *path, bool *missing = nullptr);

    // Calls a global function. `missing` reports an undefined global.
    bool CallGlobal(const char *name, bool *missing = nullptr);
    bool CallGlobalStr(const char *name, const char *arg, bool *missing = nullptr);

    // Runs a module through the host require and leaves its value on the VM
    // stack for the caller to read via State(). Pops nothing on failure.
    bool Require(const char *name, bool *missing = nullptr);

    // Result of the previous call. Truthy drives update() -> quit, the string
    // is the reload state; LastResultString() is null unless it really was one.
    bool LastResultTruthy() const { return lastTruthy; }
    bool LastResultNil() const { return lastNil; }
    const char *LastResultString() const { return lastIsString ? lastString.c_str() : nullptr; }

    const char *Error() const { return lastError; }
    lua_State *State() const { return L; }

private:
    bool NoState();
    void ResetResult();
    void CaptureResult();
    void CaptureError();
    bool Pcall(int nargs, int nresults);

    lua_State *L = nullptr;
    std::string entry;       // chunk name of the entry script, for error text
    std::string lastString;
    bool lastIsString = false;
    bool lastNil = true;
    bool lastTruthy = false;
    char lastError[2048] = {};
};
