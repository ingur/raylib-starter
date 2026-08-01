#pragma once

#include <string>

struct lua_State;

// owns one Luau VM, Reset boots a fresh one and nothing survives except the
// string the host carries across by hand
class Script {
public:
    Script() = default;
    ~Script();
    Script(const Script &) = delete;
    Script &operator=(const Script &) = delete;

    bool Reset(bool devMode);
    void Close();

    // `missing` reports a file that is not there, as opposed to one that failed
    bool RunEntry(const char *path, bool *missing = nullptr);

    bool CallGlobal(const char *name, bool *missing = nullptr);
    bool CallGlobalStr(const char *name, const char *arg, bool *missing = nullptr);

    // leaves the module value on the VM stack, nothing on failure
    bool Require(const char *name, bool *missing = nullptr);

    // result of the previous call
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
