#pragma once
// adapters for raylib signatures the generic binder cannot expose safely
// add one adapter per signature shape

#include "bind.hpp"
#include "raylib.h"

namespace adapt {

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

}  // namespace adapt

#define BIND_UNLOAD(f) (&::adapt::Unload<decltype(&f), &f>::Call)
#define BIND_SCOPE_BEGIN(f, s) (&::adapt::ScopeBegin<::adapt::Scope::s, decltype(&f), &f>::Call)
#define BIND_SCOPE_END(f, s) (&::adapt::ScopeEnd<::adapt::Scope::s, decltype(&f), &f>::Call)
