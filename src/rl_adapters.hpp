#pragma once
// Pattern adapters for raylib signatures the generic wrapper cannot bind safely.
//
// One adapter per signature *shape*, never one per function. tools/bindgen.py
// matches the shape and emits the instantiation; anything it cannot match stays
// out of the module and lands in the report at the bottom of raylib_bind.cpp.

#include "bind.hpp"

namespace adapt {

// The Unload*(T handle) family.
//
// raylib's unloaders take the handle by value and free what it points at, so
// every copy of that handle is left dangling: UnloadSound(s) twice reaches
// RL_FREE on the same AudioBuffer. Taking the userdata by pointer and zeroing
// the payload afterwards makes the userdata itself the single owner. A second
// unload, or any use after unloading, then hits raylib's NULL guards and does
// nothing instead of touching freed memory.
//
// Handles cannot be aliased into a second userdata: a struct field whose type
// owns an allocation is generated without a setter, so there is no way to copy
// Sound.stream back out. Copies obtained from raylib itself (LoadSoundAlias)
// are separate allocations and raylib documents their unload order.
template <typename Signature, Signature fn>
struct Unload;

template <typename T, void (*fn)(T)>
struct Unload<void (*)(T), fn> {
    static int Call(lua_State *L) {
        T *handle = bind::CheckUd<std::remove_cv_t<T>>(L, 1);
        fn(*handle);
        *handle = T{};
        return 0;
    }
};

}  // namespace adapt

#define BIND_UNLOAD(f) (&::adapt::Unload<decltype(&f), &f>::Call)
