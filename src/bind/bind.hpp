#pragma once
// add a Conv specialization to bind a C++ type

#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "lua.h"
#include "lualib.h"

#if LUA_USE_LONGJMP
#error "bindings and adapters rely on C++ exceptions for error unwinding"
#endif

namespace bind {

// unloaded userdata is retagged here so later use fails its own tag check
inline constexpr int kReleasedTag = LUA_UTAG_LIMIT - 1;

template <typename T, typename Enable = void>
struct Conv;

// out of range floating point to integer casts are undefined
// kHigh is exclusive because max() may not be representable as double
template <typename T>
inline T CheckIntegral(lua_State *L, int narg) {
    constexpr double kLow = static_cast<double>(std::numeric_limits<T>::min());
    constexpr double kHigh = static_cast<double>(std::numeric_limits<T>::max()) + 1.0;
    const double value = std::trunc(luaL_checknumber(L, narg));
    if (!(value >= kLow && value < kHigh))  // also catches NaN
        luaL_argerror(L, narg, "integer out of range");
    return static_cast<T>(value);
}

template <typename T>
struct Conv<T, std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>>> {
    static T Check(lua_State *L, int narg) { return CheckIntegral<T>(L, narg); }
    static void Push(lua_State *L, T value) { lua_pushnumber(L, static_cast<double>(value)); }
};

template <typename T>
struct Conv<T, std::enable_if_t<std::is_floating_point_v<T>>> {
    static T Check(lua_State *L, int narg) { return static_cast<T>(luaL_checknumber(L, narg)); }
    static void Push(lua_State *L, T value) { lua_pushnumber(L, static_cast<double>(value)); }
};

template <>
struct Conv<bool> {
    static bool Check(lua_State *L, int narg) { return luaL_checkboolean(L, narg) != 0; }
    static void Push(lua_State *L, bool value) { lua_pushboolean(L, value); }
};

template <>
struct Conv<const char *> {
    static const char *Check(lua_State *L, int narg) { return luaL_checkstring(L, narg); }
    static void Push(lua_State *L, const char *value) {
        if (value != nullptr)
            lua_pushstring(L, value);
        else
            lua_pushnil(L);
    }
};

// generated once for each userdata type, tag 0 is reserved for plain userdata
template <typename T>
struct UdTraits {
    static constexpr bool kBound = false;
};

// runs only while an error is already being raised, so it can afford a lookup
inline bool IsReleased(lua_State *L, int narg, const char *name) {
    if (lua_userdatatag(L, narg) != kReleasedTag || !lua_getmetatable(L, narg))
        return false;
    lua_getfield(L, LUA_REGISTRYINDEX, name);
    const bool same = lua_rawequal(L, -1, -2) != 0;
    lua_pop(L, 2);
    return same;
}

// a released userdata keeps its metatable, so identity still names the type
inline void BadArgument(lua_State *L, int narg, const char *name) {
    if (IsReleased(L, narg, name))
        luaL_error(L, "%s has been unloaded", name);
    luaL_typeerror(L, narg, name);
}

template <typename T>
inline T *CheckUd(lua_State *L, int narg) {
    void *payload = lua_touserdatatagged(L, narg, UdTraits<T>::kTag);
    if (payload == nullptr)
        BadArgument(L, narg, UdTraits<T>::kName);
    return static_cast<T *>(payload);
}

template <typename T>
inline T *NewUd(lua_State *L, const T &value) {
    static_assert(std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T>,
                  "userdata payloads must be trivial, RegisterType installs no destructor");
    void *memory = lua_newuserdatataggedwithmetatable(L, sizeof(T), UdTraits<T>::kTag);
    return ::new (memory) T(value);
}

template <typename T>
struct Conv<T, std::enable_if_t<UdTraits<T>::kBound>> {
    static T Check(lua_State *L, int narg) { return *CheckUd<T>(L, narg); }
    static void Push(lua_State *L, const T &value) { NewUd<T>(L, value); }
};

// points into the userdata, the pointer must not escape the call
template <typename T>
struct Conv<T *, std::enable_if_t<UdTraits<T>::kBound>> {
    static T *Check(lua_State *L, int narg) { return CheckUd<T>(L, narg); }
};

template <auto Member>
struct Field;

template <typename S, typename F, F S::*M>
struct Field<M> {
    static void Get(lua_State *L, const void *obj) { Conv<F>::Push(L, static_cast<const S *>(obj)->*M); }
    static void Set(lua_State *L, void *obj, int narg) { static_cast<S *>(obj)->*M = Conv<F>::Check(L, narg); }
};

struct FieldDef {
    const char *name;
    void (*get)(lua_State *, const void *);
    void (*set)(lua_State *, void *, int);  // nullptr for read only fields
};

struct TypeInfo {
    const char *name;
    int tag;
    const FieldDef *fields;
    int fieldCount;
};

// generated fields are sorted for binary search
inline const FieldDef *FindField(const TypeInfo &type, const char *key) {
    int lo = 0, hi = type.fieldCount - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        const int cmp = std::strcmp(key, type.fields[mid].name);
        if (cmp == 0)
            return &type.fields[mid];
        if (cmp < 0)
            hi = mid - 1;
        else
            lo = mid + 1;
    }
    return nullptr;
}

inline const TypeInfo &UpvalueType(lua_State *L) {
    return *static_cast<const TypeInfo *>(lua_tolightuserdata(L, lua_upvalueindex(1)));
}

// scripts can reach these through getmetatable() with any value as self
inline void *CheckSelf(lua_State *L, const TypeInfo &type) {
    void *self = lua_touserdatatagged(L, 1, type.tag);
    if (self == nullptr)
        BadArgument(L, 1, type.name);
    return self;
}

inline int IndexField(lua_State *L) {
    const TypeInfo &type = UpvalueType(L);
    void *self = CheckSelf(L, type);
    const char *key = luaL_checkstring(L, 2);
    const FieldDef *field = FindField(type, key);
    if (field == nullptr)
        luaL_error(L, "%s has no field '%s'", type.name, key);
    field->get(L, self);
    return 1;
}

inline int NewIndexField(lua_State *L) {
    const TypeInfo &type = UpvalueType(L);
    void *self = CheckSelf(L, type);
    const char *key = luaL_checkstring(L, 2);
    const FieldDef *field = FindField(type, key);
    if (field == nullptr)
        luaL_error(L, "%s has no field '%s'", type.name, key);
    if (field->set == nullptr)
        luaL_error(L, "%s.%s is read only", type.name, key);
    field->set(L, self, 3);
    return 0;
}

inline int ToStringField(lua_State *L) {
    const TypeInfo &type = UpvalueType(L);
    lua_pushfstring(L, "%s: %p", type.name, lua_touserdatatagged(L, 1, type.tag));
    return 1;
}

// without LuauUdataMetatablePinned, Luau does not root tag metatables,
// luaL_newmetatable keeps them in the registry
inline void RegisterType(lua_State *L, const TypeInfo &type) {
    // Luau only rejects a reassigned tag through an assert, which release drops
    lua_getuserdatametatable(L, type.tag);
    const bool taken = !lua_isnil(L, -1);
    lua_pop(L, 1);
    if (taken)
        luaL_error(L, "userdata tag %d is already registered", type.tag);
    if (luaL_newmetatable(L, type.name) == 0)
        luaL_error(L, "duplicate userdata type name '%s'", type.name);

    for (const auto &entry : {std::pair{"__index", IndexField}, std::pair{"__newindex", NewIndexField},
                              std::pair{"__tostring", ToStringField}}) {
        lua_pushlightuserdata(L, const_cast<TypeInfo *>(&type));
        lua_pushcclosure(L, entry.second, entry.first, 1);
        lua_setfield(L, -2, entry.first);
    }
    lua_pushstring(L, type.name);  // makes typeof(texture) read "Texture"
    lua_setfield(L, -2, "__type");
    lua_setreadonly(L, -1, true);
    lua_setuserdatametatable(L, type.tag);
}

// assigns optional fields in declaration order
template <typename S, auto... Members>
struct Ctor {
    static int Call(lua_State *L) {
        S value{};
        int narg = 0;
        (AssignOpt<Members>(L, &value, ++narg), ...);
        NewUd<S>(L, value);
        return 1;
    }

private:
    template <auto M>
    static void AssignOpt(lua_State *L, void *obj, int narg) {
        if (!lua_isnoneornil(L, narg))
            Field<M>::Set(L, obj, narg);
    }
};

template <typename Signature, Signature fn>
struct Wrapper;

template <typename R, typename... A, R (*fn)(A...)>
struct Wrapper<R (*)(A...), fn> {
    static int Call(lua_State *L) { return Invoke(L, std::index_sequence_for<A...>{}); }

private:
    template <std::size_t... I>
    static int Invoke(lua_State *L, std::index_sequence<I...>) {
        // braced initialization preserves left to right argument checks
        [[maybe_unused]] std::tuple<A...> args{Conv<std::remove_cv_t<A>>::Check(L, int(I) + 1)...};
        if constexpr (std::is_void_v<R>) {
            fn(std::get<I>(args)...);
            return 0;
        } else {
            Conv<std::remove_cv_t<R>>::Push(L, fn(std::get<I>(args)...));
            return 1;
        }
    }
};

}  // namespace bind

// supported compilers ignore C language linkage in this template match
#define BIND_FN(f) (&::bind::Wrapper<decltype(&f), &f>::Call)
