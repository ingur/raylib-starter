#pragma once
// adapters for what the binding itself introduces, in three kinds:
// the death of a handle, the recovery of an open scope, and a size that only
// exists as a pointer in C
// raylib owns its own parameter contracts, a call that is wrong in C is wrong
// here for the same reason

#include "bind/rl_types.hpp"
#include "raylib.h"

#include <cstring>

namespace adapt {

// ------------------------------------------------------------ ownership
// the unload adapter zeroes and releases its userdata, later use errors and a
// repeated unload is a no-op
// fields whose structs own allocations have no getter or setter, so field reads
// cannot create a second owning userdata
// copies of GL name fields are borrows, they are safe to read and pass,
// unloading them is undefined, as in C raylib
template <typename Signature, Signature fn>
struct Unload;

template <typename T, void (*fn)(T)>
struct Unload<void (*)(T), fn> {
    static int Call(lua_State *L) {
        using Type = std::remove_cv_t<T>;
        if (bind::IsReleased(L, 1, bind::UdTraits<Type>::kName))
            return 0;
        Type *handle = bind::CheckUd<Type>(L, 1);
        fn(*handle);
        *handle = T{};
        lua_setuserdatatag(L, 1, bind::kReleasedTag);
        return 0;
    }
};

// ----------------------------------------------------------------- scope
// raylib's Begin/End pairs leave global render state open. A script that raises
// between them would otherwise draw the error screen into a render target, under
// its shader, or with its projection, and a script that forgets an End would
// corrupt the matrix stack every frame with no diagnostic
enum class Scope { Drawing, TextureMode, Mode2D, Mode3D, ShaderMode, BlendMode, ScissorMode, VrStereoMode };

inline const char *ScopeName(Scope scope) {
    switch (scope) {
        case Scope::Drawing: return "Drawing";
        case Scope::TextureMode: return "TextureMode";
        case Scope::Mode2D: return "Mode2D";
        case Scope::Mode3D: return "Mode3D";
        case Scope::ShaderMode: return "ShaderMode";
        case Scope::BlendMode: return "BlendMode";
        case Scope::ScissorMode: return "ScissorMode";
        case Scope::VrStereoMode: return "VrStereoMode";
    }
    return "?";
}

inline void CloseScope(Scope scope) {
    switch (scope) {
        case Scope::Drawing: EndDrawing(); break;
        case Scope::TextureMode: EndTextureMode(); break;
        case Scope::Mode2D: EndMode2D(); break;
        case Scope::Mode3D: EndMode3D(); break;
        case Scope::ShaderMode: EndShaderMode(); break;
        case Scope::BlendMode: EndBlendMode(); break;
        case Scope::ScissorMode: EndScissorMode(); break;
        case Scope::VrStereoMode: EndVrStereoMode(); break;
    }
}

// fixed capacity, the error path must not allocate
inline constexpr int kMaxScopeDepth = 16;

inline Scope g_openScopes[kMaxScopeDepth];
inline int g_openScopeCount = 0;

inline int ScopeDepth() {
    return g_openScopeCount;
}

inline const char *InnermostScopeName() {
    return g_openScopeCount > 0 ? ScopeName(g_openScopes[g_openScopeCount - 1]) : "none";
}

inline void UnwindScopes(int depth) {
    while (g_openScopeCount > depth)
        CloseScope(g_openScopes[--g_openScopeCount]);
}

// raylib's End calls restore defaults rather than the previous value, so
// reopening a scope that is already open is always a bug
inline void CheckCanOpen(lua_State *L, Scope scope) {
    if (g_openScopeCount == kMaxScopeDepth)
        luaL_error(L, "more than %d raylib scopes are open", kMaxScopeDepth);
    for (int i = 0; i < g_openScopeCount; ++i)
        if (g_openScopes[i] == scope)
            luaL_error(L, "Begin%s while %s is already open", ScopeName(scope), ScopeName(scope));
}

inline void CheckIsOpen(lua_State *L, Scope scope) {
    if (g_openScopeCount == 0)
        luaL_error(L, "End%s without Begin%s", ScopeName(scope), ScopeName(scope));
    const Scope top = g_openScopes[g_openScopeCount - 1];
    if (top != scope)
        luaL_error(L, "End%s while %s is the innermost open scope", ScopeName(scope), ScopeName(top));
}

// validate before raylib runs, record after it succeeds, so a raised argument
// error never leaves the stack describing a scope that was not opened
template <Scope scope, typename Signature, Signature fn>
struct ScopeBegin {
    static int Call(lua_State *L) {
        CheckCanOpen(L, scope);
        const int results = bind::Wrapper<Signature, fn>::Call(L);
        g_openScopes[g_openScopeCount++] = scope;
        return results;
    }
};

template <Scope scope, typename Signature, Signature fn>
struct ScopeEnd {
    static int Call(lua_State *L) {
        CheckIsOpen(L, scope);
        const int results = bind::Wrapper<Signature, fn>::Call(L);
        --g_openScopeCount;
        return results;
    }
};

// ------------------------------------------------------- representation
// a script holds no pointer, so where raylib takes a void * whose size lives
// in another argument, the adapter is the only place that size can be rebuilt

// rlgl casts the value pointer to float, int or unsigned int, so the scratch
// has to satisfy all three alignments
union UniformValue {
    float f[4];
    int i[4];
    unsigned int u[4];
};

inline void ReadUniformBuffer(lua_State *L, int narg, UniformValue *out, std::size_t bytes) {
    std::size_t len = 0;
    const void *data = luaL_checkbuffer(L, narg, &len);
    if (len != bytes)
        luaL_argerror(L, narg, TextFormat("this uniform needs a buffer of %d bytes", (int)bytes));
    std::memcpy(out, data, bytes);
}

inline int SetShaderValue(lua_State *L) {
    const Shader shader = bind::Conv<Shader>::Check(L, 1);
    const int locIndex = bind::CheckIntegral<int>(L, 2);
    const int uniformType = bind::CheckIntegral<int>(L, 4);

    UniformValue value{};
    switch (uniformType) {
        case SHADER_UNIFORM_FLOAT:
            value.f[0] = static_cast<float>(luaL_checknumber(L, 3));
            break;
        case SHADER_UNIFORM_VEC2:
        case SHADER_UNIFORM_VEC3: {
            const float *v = luaL_checkvector(L, 3);
            value.f[0] = v[0];
            value.f[1] = v[1];
            value.f[2] = uniformType == SHADER_UNIFORM_VEC3 ? v[2] : 0.0f;
            break;
        }
        case SHADER_UNIFORM_VEC4: {
            const Vector4 v = bind::Conv<Vector4>::Check(L, 3);
            value.f[0] = v.x;
            value.f[1] = v.y;
            value.f[2] = v.z;
            value.f[3] = v.w;
            break;
        }
        case SHADER_UNIFORM_INT:
        case SHADER_UNIFORM_SAMPLER2D:
            value.i[0] = bind::CheckIntegral<int>(L, 3);
            break;
        case SHADER_UNIFORM_UINT:
            value.u[0] = bind::CheckIntegral<unsigned int>(L, 3);
            break;
        case SHADER_UNIFORM_IVEC2:
        case SHADER_UNIFORM_UIVEC2:
            ReadUniformBuffer(L, 3, &value, 8);
            break;
        case SHADER_UNIFORM_IVEC3:
        case SHADER_UNIFORM_UIVEC3:
            ReadUniformBuffer(L, 3, &value, 12);
            break;
        case SHADER_UNIFORM_IVEC4:
        case SHADER_UNIFORM_UIVEC4:
            ReadUniformBuffer(L, 3, &value, 16);
            break;
        default:
            luaL_argerror(L, 4, "unknown shader uniform type");
    }

    ::SetShaderValue(shader, locIndex, &value, uniformType);
    return 0;
}

inline int UpdateTexture(lua_State *L) {
    const Texture texture = bind::Conv<Texture>::Check(L, 1);
    // the buffer has to match what rlgl will read, which only the texture knows
    const int expected = ::GetPixelDataSize(texture.width, texture.height, texture.format);
    if (expected <= 0)
        luaL_argerror(L, 1, "texture has no pixel format");

    std::size_t len = 0;
    const void *pixels = luaL_checkbuffer(L, 2, &len);
    if (len != static_cast<std::size_t>(expected))
        luaL_argerror(L, 2, TextFormat("this texture needs a buffer of %d bytes", expected));

    ::UpdateTexture(texture, pixels);
    return 0;
}

}  // namespace adapt

#define BIND_UNLOAD(f) (&::adapt::Unload<decltype(&f), &f>::Call)
#define BIND_SCOPE_BEGIN(f, s) (&::adapt::ScopeBegin<::adapt::Scope::s, decltype(&f), &f>::Call)
#define BIND_SCOPE_END(f, s) (&::adapt::ScopeEnd<::adapt::Scope::s, decltype(&f), &f>::Call)
