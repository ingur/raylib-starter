#pragma once
// raylib's three special-cased types.
//
// Everything else raylib declares is a tagged userdata handled generically by
// bind.hpp; these three earn a bespoke representation because they are the ones
// a game touches thousands of times a frame and neither of them should allocate:
//
//   Vector2, Vector3  ->  the native Luau `vector`, which lives inline in a
//                         TValue. Vector2 uses x and y and ignores z.
//   Color             ->  a packed 0xRRGGBBAA number, matching ColorToInt.
//                         Color is four bytes; putting it in a float vector
//                         would be both lossy and larger.

#include "bind.hpp"
#include "raylib.h"

namespace bind {

// LUA_VECTOR_SIZE is 3, so the third lane is free and Vector2 simply zeroes it.
template <>
struct Conv<Vector2> {
    static Vector2 Check(lua_State *L, int narg) {
        const float *v = luaL_checkvector(L, narg);
        return Vector2{v[0], v[1]};
    }
    static void Push(lua_State *L, Vector2 value) { lua_pushvector(L, value.x, value.y, 0.0f); }
};

template <>
struct Conv<Vector3> {
    static Vector3 Check(lua_State *L, int narg) {
        const float *v = luaL_checkvector(L, narg);
        return Vector3{v[0], v[1], v[2]};
    }
    static void Push(lua_State *L, Vector3 value) { lua_pushvector(L, value.x, value.y, value.z); }
};

inline unsigned int PackColor(Color c) {
    return (static_cast<unsigned int>(c.r) << 24) | (static_cast<unsigned int>(c.g) << 16) |
           (static_cast<unsigned int>(c.b) << 8) | static_cast<unsigned int>(c.a);
}

inline Color UnpackColor(unsigned int v) {
    return Color{static_cast<unsigned char>(v >> 24), static_cast<unsigned char>(v >> 16),
                 static_cast<unsigned char>(v >> 8), static_cast<unsigned char>(v)};
}

template <>
struct Conv<Color> {
    static Color Check(lua_State *L, int narg) { return UnpackColor(CheckIntegral<unsigned int>(L, narg)); }
    static void Push(lua_State *L, Color value) { lua_pushnumber(L, static_cast<double>(PackColor(value))); }
};

// ------------------------------------------------------------- constructors

inline int CtorVector2(lua_State *L) {
    lua_pushvector(L, static_cast<float>(luaL_optnumber(L, 1, 0.0)), static_cast<float>(luaL_optnumber(L, 2, 0.0)), 0.0f);
    return 1;
}

inline int CtorVector3(lua_State *L) {
    lua_pushvector(L, static_cast<float>(luaL_optnumber(L, 1, 0.0)), static_cast<float>(luaL_optnumber(L, 2, 0.0)),
                   static_cast<float>(luaL_optnumber(L, 3, 0.0)));
    return 1;
}

// Components are clamped rather than rejected: colour maths in a script drifts
// past the byte range constantly and saturating is the useful answer.
inline int CtorColor(lua_State *L) {
    auto channel = [L](int narg, double fallback) -> unsigned int {
        double v = luaL_optnumber(L, narg, fallback);
        if (!(v > 0.0))
            return 0u;  // catches NaN
        if (v > 255.0)
            return 255u;
        return static_cast<unsigned int>(v);
    };
    unsigned int packed = (channel(1, 0.0) << 24) | (channel(2, 0.0) << 16) | (channel(3, 0.0) << 8) | channel(4, 255.0);
    lua_pushnumber(L, static_cast<double>(packed));
    return 1;
}

}  // namespace bind
