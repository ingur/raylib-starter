#pragma once
// Generic Luau <-> C++ marshalling.
//
// Every conversion rule lives here exactly once. tools/bindgen.py emits
// registration lists and type traits, never unmarshalling code: a bound raylib
// function is `bind::Wrapper<decltype(&Fn), &Fn>::Call`, and the compiler grows
// the argument handling from the signature. Adding a C type means adding one
// Conv specialisation, not editing 600 call sites.
//
// Luau is built in its default error mode, so luaL_error and friends throw a
// C++ exception that the VM catches at the call boundary. Destructors run.
// Do not switch the build to LUA_USE_LONGJMP without auditing every adapter.

#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <tuple>
#include <type_traits>
#include <utility>

#include "lua.h"
#include "lualib.h"

namespace bind {

// ---------------------------------------------------------------- conversions

// Single point of truth for one C type. Check() reads argument `narg`, Push()
// pushes exactly one value. Specialise this, never Wrapper.
template <typename T, typename Enable = void>
struct Conv;

// A double -> integral cast is undefined for NaN, infinities and anything past
// the target range, and unsigned types wrap silently on negatives. Script
// numbers reach every generated binding, so bound the value first. Fractions
// truncate toward zero, which is what raylib's int parameters have always done.
//
// max() + 1.0 lands exactly on 2^bits for every two's complement width: the
// addition is either exact or already rounded there, so the half open compare
// is tight even for 64 bit types where max() itself is not representable.
template <typename T>
inline T CheckIntegral(lua_State *L, int narg) {
    constexpr double kLow = static_cast<double>(std::numeric_limits<T>::min());
    constexpr double kHigh = static_cast<double>(std::numeric_limits<T>::max()) + 1.0;
    const double value = std::trunc(luaL_checknumber(L, narg));
    if (!(value >= kLow && value < kHigh))  // negated so NaN lands here too
        luaL_argerror(L, narg, "integer out of range");
    return static_cast<T>(value);
}

// Integers and enums. Values go out as doubles, which hold every 32 bit
// integer exactly, so the full unsigned range survives a round trip.
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
    // raylib hands back NULL for out of range queries, nil is the honest mapping
    static void Push(lua_State *L, const char *value) {
        if (value != nullptr)
            lua_pushstring(L, value);
        else
            lua_pushnil(L);
    }
};

// ------------------------------------------------------------ tagged userdata

// Specialised by the generated bindings, once per raylib struct that is not
// mapped onto a native Luau type. Tags start at 1; tag 0 belongs to plain
// lua_newuserdata.
template <typename T>
struct UdTraits {
    static constexpr bool kBound = false;
};

template <typename T>
inline T *CheckUd(lua_State *L, int narg) {
    void *p = lua_touserdatatagged(L, narg, UdTraits<T>::kTag);
    if (p == nullptr)
        luaL_typeerror(L, narg, UdTraits<T>::kName);
    return static_cast<T *>(p);
}

template <typename T>
inline T *NewUd(lua_State *L) {
    return static_cast<T *>(lua_newuserdatataggedwithmetatable(L, sizeof(T), UdTraits<T>::kTag));
}

// By value: raylib structs are small and copied on every call anyway.
template <typename T>
struct Conv<T, std::enable_if_t<UdTraits<T>::kBound>> {
    static T Check(lua_State *L, int narg) { return *CheckUd<T>(L, narg); }
    static void Push(lua_State *L, const T &value) { *NewUd<T>(L) = value; }
};

// By pointer: raylib's in place mutators (ImageResize, UpdateCamera, ...) get
// the address of the userdata payload, so the script sees the write. This is
// only safe because the pointer never escapes the call.
template <typename T>
struct Conv<T *, std::enable_if_t<UdTraits<T>::kBound>> {
    static T *Check(lua_State *L, int narg) { return CheckUd<T>(L, narg); }
};

// ------------------------------------------------------------ field accessors

// Field<&Image::width> turns a member pointer into a get/set pair. Both take
// the raw payload so a single pair of metamethods can serve every type.
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
    void (*set)(lua_State *, void *, int);  // null on read only types
};

struct TypeInfo {
    const char *name;
    int tag;
    const FieldDef *fields;
    int fieldCount;
};

// Fields are emitted sorted, so this is a binary search over at most ~16 short
// names. Fast enough that interning key atoms is not worth the machinery yet.
inline const FieldDef *FindField(const TypeInfo &type, const char *key) {
    int lo = 0, hi = type.fieldCount - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int cmp = std::strcmp(key, type.fields[mid].name);
        if (cmp == 0)
            return &type.fields[mid];
        if (cmp < 0)
            hi = mid - 1;
        else
            lo = mid + 1;
    }
    return nullptr;
}

// The TypeInfo rides along as upvalue 1 of the metamethod closure.
inline const TypeInfo &UpvalueType(lua_State *L) {
    return *static_cast<const TypeInfo *>(lua_tolightuserdata(L, lua_upvalueindex(1)));
}

inline int IndexField(lua_State *L) {
    const TypeInfo &type = UpvalueType(L);
    const char *key = luaL_checkstring(L, 2);
    const FieldDef *field = FindField(type, key);
    if (field == nullptr)
        luaL_error(L, "%s has no field '%s'", type.name, key);
    field->get(L, lua_touserdatatagged(L, 1, type.tag));
    return 1;
}

inline int NewIndexField(lua_State *L) {
    const TypeInfo &type = UpvalueType(L);
    const char *key = luaL_checkstring(L, 2);
    const FieldDef *field = FindField(type, key);
    if (field == nullptr)
        luaL_error(L, "%s has no field '%s'", type.name, key);
    if (field->set == nullptr)
        luaL_error(L, "%s.%s is read only", type.name, key);
    field->set(L, lua_touserdatatagged(L, 1, type.tag), 3);
    return 0;
}

inline int ToStringField(lua_State *L) {
    const TypeInfo &type = UpvalueType(L);
    lua_pushfstring(L, "%s: %p", type.name, lua_touserdatatagged(L, 1, type.tag));
    return 1;
}

// Installs the shared metatable for one tag. Luau refuses to reassign a tag's
// metatable, so this runs once per lua_State.
//
// luaL_newmetatable rather than lua_createtable because lua_setuserdatametatable
// stores into a slot the collector only visits when a cycle starts, and it runs
// no write barrier. Registering while a cycle is already in progress would leave
// the metatable unmarked and it would be swept out from under the userdata. The
// registry entry luaL_newmetatable leaves behind keeps it reachable regardless;
// this is the same pairing Luau's own tagged userdata tests use.
inline void RegisterType(lua_State *L, const TypeInfo &type) {
    luaL_newmetatable(L, type.name);
    lua_pushlightuserdata(L, const_cast<TypeInfo *>(&type));
    lua_pushcclosure(L, IndexField, "__index", 1);
    lua_setfield(L, -2, "__index");
    lua_pushlightuserdata(L, const_cast<TypeInfo *>(&type));
    lua_pushcclosure(L, NewIndexField, "__newindex", 1);
    lua_setfield(L, -2, "__newindex");
    lua_pushlightuserdata(L, const_cast<TypeInfo *>(&type));
    lua_pushcclosure(L, ToStringField, "__tostring", 1);
    lua_setfield(L, -2, "__tostring");
    lua_pushstring(L, type.name);  // makes typeof(texture) read "Texture"
    lua_setfield(L, -2, "__type");
    lua_setreadonly(L, -1, true);
    lua_setuserdatametatable(L, type.tag);
}

// ---------------------------------------------------------------- constructors

// Ctor<Rectangle, &Rectangle::x, ...>::Call builds a zeroed struct and fills
// the listed fields from the arguments in order. Trailing arguments may be
// omitted and stay zero.
template <typename S, auto... Members>
struct Ctor {
    static int Call(lua_State *L) {
        S value{};
        int narg = 0;
        (AssignOpt<Members>(L, &value, ++narg), ...);
        *NewUd<S>(L) = value;
        return 1;
    }

private:
    template <auto M>
    static void AssignOpt(lua_State *L, void *obj, int narg) {
        if (!lua_isnoneornil(L, narg))
            Field<M>::Set(L, obj, narg);
    }
};

// ------------------------------------------------------------- call wrapper

template <typename Signature, Signature fn>
struct Wrapper;

template <typename R, typename... A, R (*fn)(A...)>
struct Wrapper<R (*)(A...), fn> {
    static int Call(lua_State *L) { return Invoke(L, std::index_sequence_for<A...>{}); }

private:
    template <std::size_t... I>
    static int Invoke(lua_State *L, std::index_sequence<I...>) {
        // braced init pins left to right evaluation; a plain call expression
        // leaves argument order unspecified and reports type errors out of order
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

// raylib declares its functions extern "C"; clang, gcc and msvc all ignore
// language linkage when matching the function pointer template parameter.
#define BIND_FN(f) (&::bind::Wrapper<decltype(&f), &f>::Call)

