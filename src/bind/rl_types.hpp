#pragma once
// raylib types mapped onto native Luau values instead of userdata

#include "bind/bind.hpp"
#include "raylib.h"

namespace bind {

// Luau vectors have three components, so Vector2 sets z to zero
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

inline int CtorVector2(lua_State *L) {
    lua_pushvector(L, static_cast<float>(luaL_optnumber(L, 1, 0.0)), static_cast<float>(luaL_optnumber(L, 2, 0.0)), 0.0f);
    return 1;
}

inline int CtorVector3(lua_State *L) {
    lua_pushvector(L, static_cast<float>(luaL_optnumber(L, 1, 0.0)), static_cast<float>(luaL_optnumber(L, 2, 0.0)),
                   static_cast<float>(luaL_optnumber(L, 3, 0.0)));
    return 1;
}

// components are clamped rather than rejected: color maths in a script drifts
// past the byte range constantly and saturating is the useful answer
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
