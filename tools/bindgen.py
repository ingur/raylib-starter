"""Regenerate the raylib bindings from the pinned raylib_api.json.

Usage: bindgen.py <dir containing raylib_api.json | path to raylib_api.json>

Writes src/raylib_bind.cpp and types/raylib.d.lua.

The generator classifies every raylib signature instead of blanket binding it:

  plain        every argument and the return value have a safe generic mapping,
               so the binding is `bind::Wrapper<decltype(&Fn), &Fn>::Call` and
               the C++ template machinery in src/bind.hpp writes the marshalling.
  unload       hands owned memory back, so it goes through the adapter in
               src/rl_adapters.hpp that invalidates the userdata afterwards.
  unsupported  nothing is emitted and the symbol is listed, with a reason, in the

Nothing in between is silently guessed at. The previous pocketpy generator cast
script integers straight to raw pointers, which let a script write to arbitrary
memory; the classifier exists so that cannot happen again. The unsupported list
shrinks as adapters are written in src/rl_adapters.hpp, never by loosening the
rules here.
"""

import json
import math
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# Symbols whose C signature looks safe but whose documented behaviour is not. This
# is the one place semantics that raylib_api.json cannot express are written down;
# re-read it on a raylib bump. Everything else is decided by the classifier below.
HAND_CLASSIFIED = {
    # The host serves these through the vfs so packed assets work, see src/vfs.cpp.
    "LoadFileText": "host provides this through the vfs",
    "LoadFileData": "host provides this through the vfs",
    "UnloadFileText": "host provides this through the vfs",
    "UnloadFileData": "host provides this through the vfs",
    # rcore.c:495,3698 stashes the pointer in a static and dereferences it on later
    # frames, so a Luau GC pass or a hot reload would leave raylib holding garbage.
    "SetAutomationEventList": "raylib keeps the pointer past the call",
    # raudio.c: the alias shares the source's sample data, and nothing in the type
    # tells the two apart, so either unloader frees memory the other still uses.
    "LoadSoundAlias": "returns a handle borrowing another handle's allocation",
    "UnloadSoundAlias": "consumes a handle borrowing another handle's allocation",
    # rmodels.c:1146 shallow copies the mesh into the model, so the Mesh userdata and
    # the Model both own the same buffers. Needs an adapter that invalidates the input.
    "LoadModelFromMesh": "takes ownership of its argument's buffers",
}

# Mapped onto native Luau values rather than userdata, see src/rl_types.hpp.
NATIVE_STRUCTS = {"Vector2": "vector", "Vector3": "vector", "Color": "number"}

SCALARS = {
    "bool",
    "char",
    "short",
    "int",
    "long",
    "long long",
    "float",
    "double",
    "unsigned char",
    "unsigned short",
    "unsigned int",
    "unsigned long",
    "unsigned long long",
    "size_t",
}

# A trailing element count turns a pointer parameter into an array, which needs an
# adapter. Deliberately narrow: `ImageBlurGaussian(Image *image, int blurSize)` is a
# single image plus an unrelated size, not an array of images.
COUNT_NAME = re.compile(r"^(count|instances|len|length|.*Count)$")

# Luau keywords. A C parameter called `end` or `type` would make the whole
# definitions file fail to parse, and luau-lsp then silently drops every type.
LUAU_KEYWORDS = {
    "and", "break", "continue", "do", "else", "elseif", "end", "export", "false",
    "for", "function", "if", "in", "local", "nil", "not", "or", "repeat", "return",
    "then", "true", "type", "typeof", "until", "while",
}


def param_name(param: dict, index: int) -> str:
    name = param.get("name") or f"a{index}"
    return f"{name}_" if name in LUAU_KEYWORDS else name


# --------------------------------------------------------------------- loading


def load_api(arg: Path) -> dict:
    path = arg if arg.is_file() else arg / "raylib_api.json"
    if not path.exists():
        sys.exit(f"raylib_api.json not found at {path}")
    return json.loads(sanitize(path.read_text()))


def sanitize(text: str) -> str:
    """rlparser emits unescaped quotes inside description strings."""

    def escape(m: re.Match) -> str:
        return m.group(1) + m.group(2).replace('\\"', '"').replace('"', '\\"') + m.group(3)

    return re.sub(r'^(\s*"description": ")(.*)(",?)$', escape, text, flags=re.M)


class Type:
    """One parsed C type from the api description."""

    def __init__(self, raw: str, aliases: dict):
        text = " ".join(raw.split())
        self.array = None
        m = re.match(r"^(.*?)\[(\d+)\]$", text)
        if m:
            text, self.array = m.group(1).strip(), int(m.group(2))
        self.const = text.startswith("const ")
        if self.const:
            text = text[6:]
        self.ptr = text.count("*")
        base = text.replace("*", "").strip()
        # rlparser spells `typedef T *Alias` as an alias literally named "*Alias"
        while base in aliases:
            base, extra = aliases[base]
            self.ptr += extra
        self.base = base

    def __str__(self) -> str:
        return f"{'const ' if self.const else ''}{self.base}{' ' + '*' * self.ptr if self.ptr else ''}"


def build_aliases(api: dict) -> dict:
    """name -> (target base, extra pointer depth)"""
    out = {}
    for a in api["aliases"]:
        name, target = a["name"], a["type"]
        if name.startswith("*"):
            out[name[1:]] = (target, 1)
        else:
            out[name] = (target, 0)
    return out


# ---------------------------------------------------------------- classification


class Api:
    def __init__(self, api: dict):
        self.raw = api
        self.aliases = build_aliases(api)
        self.structs = {s["name"]: s for s in api["structs"]}
        self.callbacks = {c["name"] for c in api["callbacks"]}
        # every struct that is not one of the three native mappings becomes userdata
        self.userdata = [name for name in self.structs if name not in NATIVE_STRUCTS]
        self.tag = {name: i + 1 for i, name in enumerate(self.userdata)}  # tag 0 is plain userdata
        self.owns_pointer = self._resolve_ownership()

    def parse(self, raw: str) -> Type:
        return Type(raw, self.aliases)

    def _resolve_ownership(self) -> dict:
        """A struct owns memory if it, or any struct it embeds, has a pointer field.

        Integer fields of such a struct describe the extent of that allocation
        (Image.width, Wave.frameCount, FilePathList.count), so letting a script
        write them would make raylib read out of bounds. Those fields are exposed
        read only. Derived from the description, never hardcoded.
        """
        owns = {name: any(self.parse(f["type"]).ptr > 0 for f in s["fields"]) for name, s in self.structs.items()}
        changed = True
        while changed:
            changed = False
            for name, s in self.structs.items():
                if owns[name]:
                    continue
                for f in s["fields"]:
                    t = self.parse(f["type"])
                    if t.ptr == 0 and owns.get(t.base):
                        owns[name] = changed = True
                        break
        return owns

    def is_userdata(self, base: str) -> bool:
        return base in self.structs and base not in NATIVE_STRUCTS

    def value_kind(self, t: Type) -> str | None:
        """Category of a by-value type, or None when it needs an adapter."""
        if t.array is not None or t.ptr:
            return None
        if t.base in SCALARS:
            return "scalar"
        if t.base in NATIVE_STRUCTS:
            return "native"
        if self.is_userdata(t.base):
            return "userdata"
        return None


def classify(api: Api, fn: dict) -> tuple[str, str]:
    """-> ("plain" | "unload", "") or ("unsupported", reason)"""
    if fn["name"] in HAND_CLASSIFIED:
        return "unsupported", HAND_CLASSIFIED[fn["name"]]

    params = fn.get("params", [])
    for p in params:
        if p["type"] == "...":
            return "unsupported", "variadic, use string.format"

    ret = api.parse(fn["returnType"])
    if ret.base != "void" or ret.ptr:
        if ret.ptr:
            if ret.base == "char" and ret.const:
                pass  # const char * is a plain string return
            else:
                return "unsupported", f"returns {ret}, ownership is not described by the api"
        elif api.value_kind(ret) is None:
            return "unsupported", f"cannot return {ret}"

    for i, p in enumerate(params):
        t = api.parse(p["type"])
        if t.base in api.callbacks:
            return "unsupported", f"takes the C callback {t.base}"
        if t.ptr == 0:
            if api.value_kind(t) is None:
                return "unsupported", f"cannot pass {t} by value"
            continue
        if t.ptr == 1 and t.base == "char" and t.const:
            continue  # plain string argument
        if t.base in ("void", "char"):
            return "unsupported", f"raw {t} buffer parameter"
        nxt = params[i + 1] if i + 1 < len(params) else None
        counted = nxt is not None and api.parse(nxt["type"]).base in SCALARS and COUNT_NAME.match(nxt.get("name", ""))
        if not api.is_userdata(t.base) or t.const or counted or t.ptr > 1:
            what = "array" if counted else "out parameter" if not t.const else "array"
            return "unsupported", f"{t} {what}"
        # a writable pointer to one userdata struct is raylib's in place mutator
        # idiom; the address of the payload never escapes the call

    # An unloader hands its argument's owned memory back. Binding it generically
    # would let a script unload the same handle twice and reach a double free, so
    # it goes through the adapter that invalidates the userdata instead.
    if fn["name"].startswith("Unload") and len(params) == 1:
        t = api.parse(params[0]["type"])
        if t.ptr == 0 and api.is_userdata(t.base) and ret.base == "void" and not ret.ptr:
            return "unload", ""

    return "plain", ""


# --------------------------------------------------------------------- emitting


def struct_fields(api: Api, name: str) -> list[tuple[str, str]]:
    """Bindable (field, lua type) pairs in declaration order.

    A field whose own type owns an allocation is dropped outright, getter
    included. Reading it would hand back a by value copy of the handle that
    aliases the same pointers, and an alias defeats the unload adapter: zeroing
    one userdata cannot invalidate an independent copy, so `UnloadAudioStream
    (sound.stream)` followed by `UnloadSound(sound)` would free the same
    AudioBuffer twice. Read only is not enough; the value must not escape.
    """
    out = []
    for f in api.structs[name]["fields"]:
        t = api.parse(f["type"])
        if t.ptr or t.array is not None:
            continue  # raw pointers and fixed arrays are never exposed
        if api.owns_pointer.get(t.base):
            continue
        if api.value_kind(t) is None:
            continue
        out.append((f["name"], lua_type(api, t)))
    return out


def writable(api: Api, name: str, field: str) -> bool:
    """Whether a field gets a __newindex setter.

    In a struct that owns an allocation the integers describe that allocation's
    extent (Image.width, Wave.frameCount, FilePathList.count), so writing them
    makes raylib read out of bounds. bool is exempt because a flag cannot
    describe an extent, which is what keeps Music.looping usable.
    """
    t = api.parse(next(f["type"] for f in api.structs[name]["fields"] if f["name"] == field))
    integral = t.base in SCALARS and t.base not in ("float", "double", "bool")
    return not (api.owns_pointer[name] and integral)


def lua_type(api: Api, t: Type) -> str:
    if t.ptr and t.base == "char":
        return "string"
    if t.base == "void" and not t.ptr:
        return "()"
    if t.base == "bool":
        return "boolean"
    if t.base in SCALARS:
        return "number"
    if t.base in NATIVE_STRUCTS:
        return NATIVE_STRUCTS[t.base]
    return t.base  # userdata, possibly through a pointer


def describe(text: str) -> str:
    """Collapse a raylib description into one line safe to paste into a comment."""
    return " ".join((text or "").split())


def field_doc(api: Api, struct: str, field: str) -> str:
    return describe(next(f.get("description", "") for f in api.structs[struct]["fields"] if f["name"] == field))


def doc(indent: str, text: str) -> str:
    """A luau-lsp doc comment, or nothing when raylib had no prose for the symbol."""
    return f"{indent}--- {text}\n" if text else ""


def numeric_constants(api: Api) -> list[tuple[str, str, float]]:
    """(name, description, value) for enums, colours and numeric defines."""
    out = []
    for enum in api.raw["enums"]:
        for v in enum["values"]:
            out.append((v["name"], describe(v.get("description")) or enum["name"], float(v["value"])))
    for d in api.raw["defines"]:
        if d["type"] == "COLOR":
            r, g, b, a = (int(x) for x in re.search(r"\{\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+)\s*\}", d["value"]).groups())
            out.append((d["name"], describe(d.get("description")) or "Color", float((r << 24) | (g << 16) | (b << 8) | a)))
        elif d["type"] in ("INT", "FLOAT"):
            out.append((d["name"], describe(d.get("description")), float(d["value"])))
        elif d["type"] == "FLOAT_MATH":
            expr = re.sub(r"(\d)f\b", r"\1", d["value"])
            out.append((d["name"], describe(d.get("description")), float(eval(expr, {"PI": math.pi}))))  # noqa: S307
    return out


HEADER = """// Generated by tools/bindgen.py from raylib {version}. Do not edit.
//
// Rerun ./build.sh bindgen after bumping raylib in build.zig.zon. The report of
// everything that is deliberately not bound is at the bottom of this file.

#include "raylib_bind.hpp"

#include "bind.hpp"
#include "rl_adapters.hpp"
#include "rl_types.hpp"

"""


def emit_cpp(api: Api, bound: list[tuple[dict, str]], unsupported: list[tuple[str, str]], version: str) -> str:
    out = [HEADER.format(version=version)]

    out.append("namespace bind {\n\n// userdata tags, 1 based because tag 0 belongs to plain lua_newuserdata\n")
    for name in api.userdata:
        out.append(
            f"template <> struct UdTraits<{name}> {{\n"
            f"    static constexpr bool kBound = true;\n"
            f"    static constexpr int kTag = {api.tag[name]};\n"
            f'    static constexpr const char *kName = "{name}";\n'
            f"}};\n"
        )
    out.append("\n}  // namespace bind\n\nnamespace {\n\nusing bind::Field;\nusing bind::FieldDef;\nusing bind::TypeInfo;\n")

    types = []
    for name in api.userdata:
        fields = struct_fields(api, name)
        entries = []
        for field, _ in sorted(fields):
            setter = f"&Field<&{name}::{field}>::Set" if writable(api, name, field) else "nullptr"
            entries.append(f'    {{"{field}", &Field<&{name}::{field}>::Get, {setter}}},')
        # a zero length array is not valid C++, an opaque handle carries no fields
        table = f"kFields{name}" if entries else "nullptr"
        if entries:
            out.append(f"\nconst FieldDef kFields{name}[] = {{\n" + "\n".join(entries) + "\n};")
        out.append(f'\nconst TypeInfo kType{name} = {{"{name}", {api.tag[name]}, {table}, {len(fields)}}};\n')
        types.append(name)

    out.append("\nconst TypeInfo *const kTypes[] = {\n")
    out.extend(f"    &kType{n},\n" for n in types)
    out.append("};\n")

    out.append("\nstruct FnDef {\n    const char *name;\n    lua_CFunction fn;\n};\n\nconst FnDef kFunctions[] = {\n")
    out.extend(f'    {{"{fn["name"]}", {"BIND_UNLOAD" if kind == "unload" else "BIND_FN"}({fn["name"]})}},\n' for fn, kind in bound)
    out.append("};\n")

    out.append("\n// constructors take the struct fields in declaration order, missing ones stay zero\nconst FnDef kConstructors[] = {\n")
    out.append('    {"Vector2", bind::CtorVector2},\n')
    out.append('    {"Vector3", bind::CtorVector3},\n')
    out.append('    {"Color", bind::CtorColor},\n')
    for name in api.userdata:
        if api.owns_pointer[name]:
            continue  # a resource handle is not something a script can build
        members = ", ".join(f"&{name}::{f}" for f, _ in struct_fields(api, name) if writable(api, name, f))
        if not members:
            continue
        out.append(f'    {{"{name}", &bind::Ctor<{name}, {members}>::Call}},\n')
    out.append("};\n")

    consts = numeric_constants(api)
    out.append("\nstruct NumDef {\n    const char *name;\n    double value;\n};\n\nconst NumDef kConstants[] = {\n")
    out.extend(f'    {{"{n}", {v!r}}},  // {c}\n' for n, c, v in consts)
    out.append("};\n\n}  // namespace\n")

    out.append("\nconst char *const kRaylibUserdataTypes[] = {\n")
    out.extend(f'    "{n}",\n' for n in api.userdata)
    out.append("    nullptr,\n};\n")

    out.append(
        f"""
void OpenRaylib(lua_State *L) {{
    for (const TypeInfo *type : kTypes)
        bind::RegisterType(L, *type);

    lua_createtable(L, 0, {len(bound) + len(consts)} + {len(api.userdata) + 3});
    for (const FnDef &def : kFunctions) {{
        lua_pushcfunction(L, def.fn, def.name);
        lua_rawsetfield(L, -2, def.name);
    }}
    for (const FnDef &def : kConstructors) {{
        lua_pushcfunction(L, def.fn, def.name);
        lua_rawsetfield(L, -2, def.name);
    }}
    for (const NumDef &def : kConstants) {{
        lua_pushnumber(L, def.value);
        lua_rawsetfield(L, -2, def.name);
    }}
    lua_setglobal(L, "raylib");
}}
"""
    )

    out.append(
        f"\n// ---------------------------------------------------------------------------\n"
        f"// Not bound: {len(unsupported)} of {len(api.raw['functions'])} raylib functions.\n"
        f"//\n"
        f"// Each needs an adapter in src/rl_adapters.hpp that owns the lifetime the C\n"
        f"// signature leaves implicit. Until one exists the function is absent rather\n"
        f"// than exposed as a raw address.\n//\n"
    )
    width = max(len(n) for n, _ in unsupported)
    for name, reason in unsupported:
        out.append(f"//   {name:<{width}}  {reason}\n")

    return "".join(out)


def emit_defs(api: Api, bound: list[tuple[dict, str]], version: str) -> str:
    out = [
        f"--!nonstrict\n"
        f"-- Generated by tools/bindgen.py from raylib {version}. Do not edit.\n"
        f"-- Editor setup lives in .vscode/settings.json and .nvim.lua.\n\n"
    ]
    # `declare class X ... end` was retired in Luau 0.7xx; the current spelling of an
    # opaque host type is `declare extern type X with ... end` (Ast/src/Parser.cpp,
    # LuauDisallowExternClassInTypeDefinitions).
    for name in api.userdata:
        out.append(doc("", describe(api.structs[name].get("description"))))
        out.append(f"declare extern type {name} with\n")
        for field, ltype in struct_fields(api, name):
            out.append(doc("    ", field_doc(api, name, field)))
            out.append(f"    {field}: {ltype}\n")
        out.append("end\n\n")

    # a bare `type` in a definitions file is file local; only `export type` reaches
    # the global namespace where user code can name it
    for a in api.raw["aliases"]:
        if not a["name"].startswith("*") and api.is_userdata(a["type"]):
            out.append(f"export type {a['name']} = {a['type']}\n")
    out.append("\ndeclare DEV: boolean\n\ndeclare raylib: {\n")

    for fn, _ in bound:
        params = ", ".join(
            f"{param_name(p, i)}: {lua_type(api, api.parse(p['type']))}" for i, p in enumerate(fn.get("params", []))
        )
        ret = api.parse(fn["returnType"])
        out.append(doc("    ", describe(fn.get("description"))))
        out.append(f"    {fn['name']}: ({params}) -> {lua_type(api, ret)},\n")

    out.append("\n    Vector2: (x: number?, y: number?) -> vector,\n")
    out.append("    Vector3: (x: number?, y: number?, z: number?) -> vector,\n")
    out.append("    Color: (r: number?, g: number?, b: number?, a: number?) -> number,\n")
    for name in api.userdata:
        if api.owns_pointer[name]:
            continue
        fields = [(f, t) for f, t in struct_fields(api, name) if writable(api, name, f)]
        if not fields:
            continue
        args = ", ".join(f"{f}: {t}?" for f, t in fields)
        out.append(f"    {name}: ({args}) -> {name},\n")

    out.append("\n")
    for name, description, _ in numeric_constants(api):
        out.append(doc("    ", description))
        out.append(f"    {name}: number,\n")
    out.append("}\n")
    return "".join(out)


def main() -> None:
    if len(sys.argv) != 2:
        sys.exit(__doc__.strip())
    raw = load_api(Path(sys.argv[1]))
    api = Api(raw)
    version = next((d["value"] for d in raw["defines"] if d["name"] == "RAYLIB_VERSION"), "?").strip('"')

    bound, unsupported = [], []
    for fn in raw["functions"]:
        kind, reason = classify(api, fn)
        if kind == "unsupported":
            unsupported.append((fn["name"], reason))
        else:
            bound.append((fn, kind))

    (ROOT / "src").mkdir(exist_ok=True)
    (ROOT / "types").mkdir(exist_ok=True)
    (ROOT / "src" / "raylib_bind.cpp").write_text(emit_cpp(api, bound, unsupported, version))
    (ROOT / "types" / "raylib.d.lua").write_text(emit_defs(api, bound, version))

    adapted = sum(1 for _, kind in bound if kind != "plain")
    print(f"raylib {version}: bound {len(bound)}/{len(raw['functions'])} functions ({adapted} through adapters),")
    print(f"  {len(api.userdata)} userdata types, {len(unsupported)} unsupported")
    print("  see the report at the bottom of src/raylib_bind.cpp")


if __name__ == "__main__":
    main()
