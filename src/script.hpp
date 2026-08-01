#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

struct lua_State;

// a plain data copy of a Luau value, the only thing that survives a reload
struct ScriptState {
    enum class Kind { Bool, Number, String, Vector, Buffer, Table };

    Kind kind = Kind::Table;
    bool boolean = false;
    double number = 0;
    std::string bytes;  // String and Buffer payloads
    float vec[3] = {};
    std::vector<std::pair<ScriptState, ScriptState>> pairs;  // Table
};

// owns one Luau VM, Reset boots a fresh one and nothing survives except the
// state the host carries across by hand
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
    bool CallGlobalTable(const char *name, const ScriptState &arg, bool *missing = nullptr);

    // leaves the module value on the VM stack, nothing on failure
    bool Require(const char *name, bool *missing = nullptr);

    // result of the previous call
    bool LastResultTruthy() const { return lastTruthy; }
    bool LastResultNil() const { return lastNil; }

    // moves out the snapshot of a table result, empty when there was none
    bool TakeResultState(ScriptState &out);
    // why a table result could not be snapshotted, empty when it could
    const char *StateError() const { return lastStateError.c_str(); }

    const char *Error() const { return lastError; }
    lua_State *State() const { return L; }

private:
    bool NoState();
    void ResetResult();
    void CaptureResult();
    void CaptureError();
    bool Pcall(int nargs, int nresults);

    lua_State *L = nullptr;
    std::string entry;  // chunk name of the entry script, for error text
    std::optional<ScriptState> lastState;
    std::string lastStateError;
    bool lastNil = true;
    bool lastTruthy = false;
    char lastError[2048] = {};
};
