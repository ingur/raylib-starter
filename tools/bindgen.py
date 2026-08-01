"""Generate bindings from raylib_api.json.

Usage: bindgen.py <raylib directory | raylib_api.json>

Writes src/raylib_bind.hpp, src/raylib_bind.cpp and types/raylib.d.luau.
"""

import json
import math
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# implemented in src/vfs.cpp because raylib's own version cannot reach the vfs
# the value is the Luau signature for the definitions file
HOST_FUNCTIONS = {
    "LoadFileText": ("(fileName: string) -> string", "Read a text file through the virtual filesystem"),
    "LoadFileData": ("(fileName: string) -> buffer", "Read a binary file through the virtual filesystem"),
    "LoadMusicStream": ("(fileName: string) -> Music", "Load music through the virtual filesystem"),
    "UnloadMusicStream": ("(music: Music) -> ()", "Unload music and release its streaming buffer"),
}

# API semantics that signatures cannot express. Verify each entry on a raylib bump.
HAND_CLASSIFIED = {
    "UnloadFileText": "the host returns strings, which Luau collects",
    "UnloadFileData": "the host returns buffers, which Luau collects",
    # rcore.c retains this pointer and dereferences it in later frames
    "SetAutomationEventList": "raylib keeps the pointer past the call",
    # Sound aliases own their AudioBuffer but borrow the source sample data
    "LoadSoundAlias": "returns a handle borrowing another handle's allocation",
    "UnloadSoundAlias": "consumes a handle borrowing another handle's allocation",
    # LoadModelFromMesh shallow-copies Mesh, so both values own its buffers
    "LoadModelFromMesh": "takes ownership of its argument's buffers",
    # rmodels.c writes maps[mapType] with no bounds check at all
    "SetMaterialTexture": "unchecked index writes out of bounds",
    # rmodels.c checks only upper bounds, so a negative index writes before the array
    "SetModelMeshMaterial": "unchecked index writes out of bounds",
    # rmodels.c wraps only frames above the count, so a negative frame reads out of bounds
    "UpdateModelAnimation": "unchecked frame reads out of bounds",
    "UpdateModelAnimationEx": "unchecked frame reads out of bounds",
    # rshapes.c returns the internal texShapes, which defaults to rlgl's white 1x1
    # texture at GL id 1, and UnloadTexture unloads any positive id. Unlike
    # rt.texture, whose getter is needed to draw a render target, nothing in a
    # script needs this one, SetShapesTexture is the half that is useful
    "GetShapesTexture": "borrows raylib's internal texture, unloading it breaks the renderer",
}

# The only handle-typed field a script can read. Drawing a render target needs
# target.texture and raylib offers no function that takes a RenderTexture, so
# this one field is a documented borrow: safe to read and pass, undefined to
# unload, exactly as in C raylib.
BORROWED_FIELDS = {("RenderTexture", "texture")}

# raylib scopes the host unwinds when a script errors mid frame. Order is the
# generated Scope enum order
SCOPES = ["Drawing", "TextureMode", "Mode2D", "Mode3D", "ShaderMode", "BlendMode", "ScissorMode", "VrStereoMode"]

# C cannot say whether a writable struct pointer is one object or an array, so
# accepting one is a reviewed decision per function. Anything absent from this set
# is reported instead of bound. On a bump, new pointer functions appear in the
# report until they are audited and added here
IN_PLACE_MUTATORS = {
    "GenMeshTangents", "GenTextureMipmaps", "ImageAlphaClear", "ImageAlphaCrop", "ImageAlphaMask",
    "ImageAlphaPremultiply", "ImageBlurGaussian", "ImageClearBackground", "ImageColorBrightness",
    "ImageColorContrast", "ImageColorGrayscale", "ImageColorInvert", "ImageColorReplace",
    "ImageColorTint", "ImageCrop", "ImageDither", "ImageDraw", "ImageDrawCircle",
    "ImageDrawCircleLines", "ImageDrawCircleLinesV", "ImageDrawCircleV", "ImageDrawLine",
    "ImageDrawLineEx", "ImageDrawLineV", "ImageDrawPixel", "ImageDrawPixelV", "ImageDrawRectangle",
    "ImageDrawRectangleLines", "ImageDrawRectangleRec", "ImageDrawRectangleV", "ImageDrawText",
    "ImageDrawTextEx", "ImageDrawTriangle", "ImageDrawTriangleEx", "ImageDrawTriangleLines",
    "ImageFlipHorizontal", "ImageFlipVertical", "ImageFormat", "ImageMipmaps", "ImageResize",
    "ImageResizeCanvas", "ImageResizeNN", "ImageRotate", "ImageRotateCCW", "ImageRotateCW",
    "ImageToPOT", "UpdateCamera", "UpdateCameraPro", "UploadMesh", "WaveCrop", "WaveFormat",
}

# mapped onto native Luau values in src/rl_types.hpp
NATIVE_STRUCTS = {"Vector2": "vector", "Vector3": "vector", "Color": "number"}

SCALARS = {
    "bool", "char", "short", "int", "long", "long long", "float", "double", "unsigned char",
    "unsigned short", "unsigned int", "unsigned long", "unsigned long long", "size_t",
}

# a matching adjacent count name labels the report reason as "array"
COUNT_NAME = re.compile(r"^(count|instances|len|length|.*Count)$")

# Luau keywords, which cannot be parameter names in the definitions file
KEYWORDS = {
    "and", "break", "continue", "do", "else", "elseif", "end", "export", "false", "for",
    "function", "if", "in", "local", "nil", "not", "or", "repeat", "return", "then",
    "true", "type", "typeof", "until", "while",
}


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
        while base in aliases:
            base, extra = aliases[base]
            self.ptr += extra
        self.base = base

    def __str__(self) -> str:
        return f"{'const ' if self.const else ''}{self.base}{' ' + '*' * self.ptr if self.ptr else ''}"


class Api:
    def __init__(self, api: dict):
        self.raw = api
        self.aliases = {}
        for a in api["aliases"]:
            # rlparser names typedef pointer aliases "*Alias"
            if a["name"].startswith("*"):
                self.aliases[a["name"][1:]] = (a["type"], 1)
            else:
                self.aliases[a["name"]] = (a["type"], 0)
        self.structs = {s["name"]: s for s in api["structs"]}
        self.callbacks = {c["name"] for c in api["callbacks"]}
        self.userdata = [name for name in self.structs if name not in NATIVE_STRUCTS]
        self.tag = {name: i + 1 for i, name in enumerate(self.userdata)}  # tag 0 is plain userdata
        self.owns = self._ownership()

    def parse(self, raw: str) -> Type:
        return Type(raw, self.aliases)

    def _ownership(self) -> dict:
        """Find structs that transitively own an allocation.

        Their integral fields can describe allocation bounds and stay read only.
        """
        owns = {n: any(self.parse(f["type"]).ptr > 0 for f in s["fields"]) for n, s in self.structs.items()}
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

    def is_handle(self, name: str) -> bool:
        """A value raylib produces and the script only ever borrows.

        Either it owns an allocation, or it carries a GPU name in an `id` field.
        Handles get no constructor, and assigning their fields could make an
        Unload call free another handle's resource.
        """
        s = self.structs.get(name)
        return bool(s) and (self.owns[name] or any(f["name"] == "id" for f in s["fields"]))

    def value_kind(self, t: Type) -> str | None:
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
    """-> (plain | unload | scope | host | unsupported, reason)"""
    name = fn["name"]
    if name in HOST_FUNCTIONS:
        return "host", ""
    if name in HAND_CLASSIFIED:
        return "unsupported", HAND_CLASSIFIED[name]

    params = fn.get("params", [])
    for p in params:
        if p["type"] == "...":
            return "unsupported", "variadic, use string.format"

    ret = api.parse(fn["returnType"])
    if ret.ptr:
        if not (ret.base == "char" and ret.const):  # const char * returns map to strings
            return "unsupported", f"returns {ret}, ownership is not described by the api"
    elif ret.base != "void" and api.value_kind(ret) is None:
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
            continue  # const char * arguments map to strings
        if t.base in ("void", "char"):
            return "unsupported", f"raw {t} buffer parameter"
        if not api.is_userdata(t.base) or t.const or t.ptr > 1:
            nxt = params[i + 1] if i + 1 < len(params) else None
            counted = nxt and api.parse(nxt["type"]).base in SCALARS and COUNT_NAME.match(nxt.get("name", ""))
            return "unsupported", f"{t} {'array' if counted or t.const else 'out parameter'}"
        # reviewed mutators may borrow one writable userdata pointer for this call
        if name not in IN_PLACE_MUTATORS:
            return "unsupported", f"{t} is not a reviewed in-place mutator"

    for scope in SCOPES:
        if name in (f"Begin{scope}", f"End{scope}"):
            return "scope", scope
    # Unload adapters release userdata so later use errors and repeated unloads are safe
    if name.startswith("Unload") and len(params) == 1:
        t = api.parse(params[0]["type"])
        if t.ptr == 0 and api.is_userdata(t.base) and ret.base == "void" and not ret.ptr:
            return "unload", ""
    return "plain", ""


def describe(text: str | None) -> str:
    return " ".join((text or "").split())


def doc(indent: str, text: str) -> str:
    return f"{indent}--- {text}\n" if text else ""


def field_doc(api: Api, struct: str, field: str) -> str:
    return describe(next(f.get("description", "") for f in api.structs[struct]["fields"] if f["name"] == field))


def lua_type(api: Api, t: Type) -> str:
    if t.ptr and t.base == "char":
        return "string"
    if t.base == "void" and not t.ptr:
        return "()"
    if t.base == "bool":
        return "boolean"
    if t.base in SCALARS:
        return "number"
    return NATIVE_STRUCTS.get(t.base, t.base)


def param_name(param: dict, index: int) -> str:
    name = param.get("name") or f"a{index}"
    return f"{name}_" if name in KEYWORDS else name


def struct_fields(api: Api, name: str) -> list[tuple[str, str]]:
    """Return bindable fields in declaration order.

    A field is dropped, getter included, when its type owns an allocation or is a
    resource handle. Reading either hands back a second userdata sharing the same
    resource, and the unload adapter can only release the userdata it is given, so
    UnloadTexture(GetFontDefault().texture) would delete the default atlas even
    though UnloadFont guards the font itself.

    BORROWED_FIELDS lists the one field raylib gives no other way to reach.
    """
    out = []
    for f in api.structs[name]["fields"]:
        t = api.parse(f["type"])
        if t.ptr or t.array is not None:
            continue  # raw pointers and fixed arrays are not exposed
        if api.value_kind(t) is None:
            continue
        if api.owns.get(t.base):
            continue
        if api.is_handle(t.base) and (name, f["name"]) not in BORROWED_FIELDS:
            continue
        out.append((f["name"], lua_type(api, t)))
    return out


def writable(api: Api, struct: str, field: str) -> bool:
    """Return whether a field can be assigned without corrupting a handle."""
    t = api.parse(next(f["type"] for f in api.structs[struct]["fields"] if f["name"] == field))
    if api.is_handle(t.base):
        return False
    integral = t.base in SCALARS and t.base not in ("float", "double", "bool")
    return not (api.is_handle(struct) and integral)


def ctor_fields(api: Api, name: str) -> list[tuple[str, str]]:
    if api.is_handle(name):
        return []  # resource handles have no script constructor
    return [(f, t) for f, t in struct_fields(api, name) if writable(api, name, f)]


def constants(api: Api) -> list[tuple[str, str, float]]:
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


GENERATED = "// generated by tools/bindgen.py from raylib {version}, do not edit\n"


def emit_header(api: Api, version: str) -> str:
    out = [GENERATED.format(version=version), "// run ./build.sh bindgen after a raylib bump\n#pragma once\n\n"]
    out.append('#include "bind.hpp"\n#include "raylib.h"\n\nnamespace bind {\n\n')
    for name in api.userdata:
        out.append(
            f"template <> struct UdTraits<{name}> {{\n"
            f"    static constexpr bool kBound = true;\n"
            f"    static constexpr int kTag = {api.tag[name]};\n"
            f'    static constexpr const char *kName = "{name}";\n'
            f"}};\n"
        )
    out.append(f"\ninline constexpr int kRaylibTagCount = {len(api.userdata)};\n")
    out.append("static_assert(kRaylibTagCount < kReleasedTag, \"raylib tags collide with the released tag\");\n")
    out.append("\n}  // namespace bind\n\n")
    out.append("// Creates the raylib global and userdata metatables for this VM.\nvoid OpenRaylib(lua_State *L);\n")
    return "".join(out)


def emit_source(api: Api, bound: list[tuple[dict, str]], unsupported: list[tuple[str, str]], version: str) -> str:
    out = [GENERATED.format(version=version), '\n#include "raylib_bind.hpp"\n\n#include "rl_adapters.hpp"\n#include "rl_types.hpp"\n\n']
    out.append("namespace {\n\nusing bind::Field;\nusing bind::FieldDef;\nusing bind::TypeInfo;\n")

    for name in api.userdata:
        fields = struct_fields(api, name)
        rows = [
            f'    {{"{f}", &Field<&{name}::{f}>::Get, '
            f'{f"&Field<&{name}::{f}>::Set" if writable(api, name, f) else "nullptr"}}},'
            for f, _ in sorted(fields)
        ]
        table = f"kFields{name}" if rows else "nullptr"
        if rows:
            out.append(f"\nconst FieldDef kFields{name}[] = {{\n" + "\n".join(rows) + "\n};")
        out.append(f'\nconst TypeInfo kType{name} = {{"{name}", {api.tag[name]}, {table}, {len(fields)}}};\n')

    out.append("\nconst TypeInfo *const kTypes[] = {\n")
    out.extend(f"    &kType{n},\n" for n in api.userdata)
    out.append("};\n")

    out.append("\nstruct FnDef {\n    const char *name;\n    lua_CFunction fn;\n};\n\nconst FnDef kFunctions[] = {\n")
    for fn, kind in bound:
        name = fn["name"]
        if kind == "scope":
            scope = next(s for s in SCOPES if name in (f"Begin{s}", f"End{s}"))
            macro = "BIND_SCOPE_BEGIN" if name.startswith("Begin") else "BIND_SCOPE_END"
            out.append(f'    {{"{name}", {macro}({name}, {scope})}},\n')
        else:
            out.append(f'    {{"{name}", {"BIND_UNLOAD" if kind == "unload" else "BIND_FN"}({name})}},\n')
    out.append("};\n")

    out.append("\nconst FnDef kConstructors[] = {\n")
    out.append('    {"Vector2", bind::CtorVector2},\n    {"Vector3", bind::CtorVector3},\n    {"Color", bind::CtorColor},\n')
    for name in api.userdata:
        fields = ctor_fields(api, name)
        if fields:
            members = ", ".join(f"&{name}::{f}" for f, _ in fields)
            out.append(f'    {{"{name}", &bind::Ctor<{name}, {members}>::Call}},\n')
    out.append("};\n")

    consts = constants(api)
    out.append("\nstruct NumDef {\n    const char *name;\n    double value;\n};\n\nconst NumDef kConstants[] = {\n")
    out.extend(f'    {{"{n}", {v!r}}},\n' for n, _, v in consts)
    out.append("};\n\n}  // namespace\n")

    out.append(
        f"""
void OpenRaylib(lua_State *L) {{
    for (const TypeInfo *type : kTypes)
        bind::RegisterType(L, *type);

    lua_createtable(L, 0, {len(bound) + len(consts) + len(api.userdata) + 3});
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
        f"\n// Not bound: {len(unsupported)} of {len(api.raw['functions'])} raylib functions.\n"
        f"// Each needs an adapter that owns the lifetime the C signature leaves implicit.\n//\n"
    )
    width = max(len(n) for n, _ in unsupported)
    out.extend(f"//   {n:<{width}}  {r}\n" for n, r in unsupported)
    return "".join(out)


def emit_defs(api: Api, bound: list[tuple[dict, str]], version: str) -> str:
    out = ["--!nonstrict\n", GENERATED.format(version=version).replace("//", "--"), "\n"]
    # Luau 0.732 uses `declare extern type` for host userdata
    for name in api.userdata:
        out.append(doc("", describe(api.structs[name].get("description"))))
        out.append(f"declare extern type {name} with\n")
        for field, ltype in struct_fields(api, name):
            text = field_doc(api, name, field)
            if (name, field) in BORROWED_FIELDS:
                text = f"{text}. Borrowed, do not unload it separately".lstrip(". ")
            out.append(doc("    ", text))
            access = "" if writable(api, name, field) else "read "
            out.append(f"    {access}{field}: {ltype}\n")
        out.append("end\n\n")

    # only exported aliases enter a definition file's global type namespace
    for a in api.raw["aliases"]:
        if not a["name"].startswith("*") and api.is_userdata(a["type"]):
            out.append(f"export type {a['name']} = {a['type']}\n")
    out.append("\ndeclare DEV: boolean\n\ndeclare raylib: {\n")

    for fn, _ in bound:
        params = ", ".join(f"{param_name(p, i)}: {lua_type(api, api.parse(p['type']))}"
                           for i, p in enumerate(fn.get("params", [])))
        out.append(doc("    ", describe(fn.get("description"))))
        out.append(f"    {fn['name']}: ({params}) -> {lua_type(api, api.parse(fn['returnType']))},\n")

    out.append("\n    Vector2: (x: number?, y: number?) -> vector,\n")
    out.append("    Vector3: (x: number?, y: number?, z: number?) -> vector,\n")
    out.append("    Color: (r: number?, g: number?, b: number?, a: number?) -> number,\n")
    for name in api.userdata:
        fields = ctor_fields(api, name)
        if fields:
            out.append(f"    {name}: ({', '.join(f'{f}: {t}?' for f, t in fields)}) -> {name},\n")

    out.append("\n")
    for name, description, _ in constants(api):
        out.append(doc("    ", description))
        out.append(f"    {name}: number,\n")
    out.append("}\n")
    return "".join(out)


def check_tables(names: set[str]) -> None:
    """Fail on a table entry raylib no longer has, so a rename cannot go unnoticed."""
    for table, entries in (("HOST_FUNCTIONS", HOST_FUNCTIONS), ("HAND_CLASSIFIED", HAND_CLASSIFIED),
                           ("IN_PLACE_MUTATORS", IN_PLACE_MUTATORS)):
        for stale in sorted(set(entries) - names):
            sys.exit(f"{table} names {stale}, which this raylib version does not define")


def main() -> None:
    if len(sys.argv) != 2:
        sys.exit(__doc__.strip())
    raw = load_api(Path(sys.argv[1]))
    api = Api(raw)
    version = next((d["value"] for d in raw["defines"] if d["name"] == "RAYLIB_VERSION"), "?").strip('"')
    check_tables({f["name"] for f in raw["functions"]})

    bound, unsupported, hosted = [], [], []
    for fn in raw["functions"]:
        kind, reason = classify(api, fn)
        if kind == "unsupported":
            unsupported.append((fn["name"], reason))
        elif kind == "host":
            hosted.append(fn)
        else:
            bound.append((fn, kind))

    (ROOT / "src" / "raylib_bind.hpp").write_text(emit_header(api, version))
    (ROOT / "src" / "raylib_bind.cpp").write_text(emit_source(api, bound, unsupported, version))
    defs = emit_defs(api, bound, version)
    host = "".join(doc("    ", d) + f"    {n}: {sig},\n" for n, (sig, d) in sorted(HOST_FUNCTIONS.items()))
    (ROOT / "types" / "raylib.d.luau").write_text(defs.replace("\n    Vector2:", f"\n{host}\n    Vector2:", 1))

    adapted = sum(1 for _, k in bound if k != "plain")
    print(f"raylib {version}: {len(bound)} functions bound ({adapted} adapted), {len(hosted)} host provided,")
    print(f"  {len(api.userdata)} userdata types, {len(unsupported)} unsupported")
    print("  see the report at the bottom of src/raylib_bind.cpp")


if __name__ == "__main__":
    main()
