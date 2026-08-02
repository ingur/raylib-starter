"""Generate bindings from the raylib and raymath api descriptions.

Usage: bindgen.py <directory holding raylib_api.json and raymath_api.json>

Writes src/bind/raylib_bind.hpp, src/bind/raylib_bind.cpp and types/raylib.d.luau.
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
    "SaveFileText": ("(fileName: string, text: string) -> boolean", "Write a text file, creating parent directories"),
    "SaveFileData": ("(fileName: string, data: buffer) -> boolean", "Write a binary file, creating parent directories"),
    "GetFileModTime": ("(fileName: string) -> number", "Get the modification time of a file in the virtual filesystem"),
    "FileExists": ("(fileName: string) -> boolean", "Check if a file exists, loose files and the pak"),
    "DirectoryExists": ("(dirPath: string) -> boolean", "Check if a directory exists, loose files and the pak"),
    "GetFileLength": ("(fileName: string) -> number", "Get the byte length of a file in the virtual filesystem"),
    "LoadDirectoryFiles": ("(dirPath: string, recursive: boolean?) -> {string}", "List files in a directory, loose files and the pak"),
    "LoadDroppedFiles": ("() -> {string}", "Load the paths of the files dropped onto the window"),
    "LoadMusicStream": ("(fileName: string) -> Music", "Load music through the virtual filesystem"),
    "UnloadMusicStream": ("(music: Music) -> ()", "Unload music and release its streaming buffer"),
    "LoadModel": ("(fileName: string) -> Model", "Load a model through the virtual filesystem"),
    "LoadModelAnimations": ("(fileName: string) -> {ModelAnimation}", "Load model animations through the virtual filesystem"),
    "UnloadModelAnimations": ("(animations: {ModelAnimation} | ModelAnimation) -> ()", "Unload one animation or a table of them"),
}

# functions the binding must not expose, each name carries its reason.
# verify these on a raylib bump
DENIED = {
    # the host opens the window before boot and closes it after the loop
    "InitWindow": "the host owns the window",
    "CloseWindow": "the host owns the window",
    "UnloadFileText": "the host returns strings, which Luau collects",
    "UnloadFileData": "the host returns buffers, which Luau collects",
    "SetAutomationEventList": "raylib keeps the pointer past the call",
    # recording dereferences the retained list, so with the setter denied
    # StartAutomationEventRecording crashes at the next EndDrawing
    "LoadAutomationEventList": "useless without SetAutomationEventList",
    "UnloadAutomationEventList": "useless without SetAutomationEventList",
    "ExportAutomationEventList": "useless without SetAutomationEventList",
    "SetAutomationEventBaseFrame": "useless without SetAutomationEventList",
    "StartAutomationEventRecording": "crashes without SetAutomationEventList",
    "StopAutomationEventRecording": "useless without SetAutomationEventList",
    "PlayAutomationEvent": "scripts cannot fill event params, it plays no-ops",
    "LoadSoundAlias": "returns a handle borrowing another handle's allocation",
    "UnloadSoundAlias": "consumes a handle borrowing another handle's allocation",
    "LoadModelFromMesh": "takes ownership of its argument's buffers",
    # rshapes.c returns the internal texShapes, which defaults to rlgl's white 1x1
    # texture at GL id 1, and UnloadTexture unloads any positive id. Unlike
    # rt.texture, whose getter is needed to draw a render target, nothing in a
    # script needs this one, SetShapesTexture is the half that is useful
    "GetShapesTexture": "borrows raylib's internal texture, unloading it breaks the renderer",
    # a FilePathList keeps its names in a char ** no script can reach, so the
    # host returns them as a table and these have nothing left to hand over
    "LoadDirectoryFilesEx": "use LoadDirectoryFiles, it takes a recursive flag",
    "UnloadDirectoryFiles": "the host returns tables, which Luau collects",
    "UnloadDroppedFiles": "the host returns tables, which Luau collects",
    # the report would otherwise describe these from the signature alone, and
    # the signature is misleading in each case
    "ComputeCRC32": "hashes a counted input buffer, not an out parameter",
    "ComputeMD5": "returns a static internal array, no ownership to describe",
    "ComputeSHA1": "returns a static internal array, no ownership to describe",
    "ComputeSHA256": "returns a static internal array, no ownership to describe",
    "UnloadRandomSequence": "no supported function yields a random sequence",
}

# the one handle-typed field a script can read. drawing a render target needs it
# and raylib takes no RenderTexture, so it is a documented borrow
BORROWED_FIELDS = {("RenderTexture", "texture")}

# order must match the Scope enum in src/bind/rl_adapters.hpp
SCOPES = ["Drawing", "TextureMode", "Mode2D", "Mode3D", "ShaderMode", "BlendMode", "ScissorMode", "VrStereoMode"]

# hand written adapters in src/bind/rl_adapters.hpp, the value is (reason, Luau signature)
ADAPTED = {
    "SetShaderValue": (
        "the value size is implied by uniformType",
        "(shader: Shader, locIndex: number, value: number | vector | Vector4 | buffer, uniformType: number) -> ()",
    ),
    "UpdateTexture": (
        "the pixel buffer size is implied by the texture",
        "(texture: Texture, pixels: buffer) -> ()",
    ),
}

# mapped onto native Luau values in src/bind/rl_types.hpp
NATIVE_STRUCTS = {"Vector2": "vector", "Vector3": "vector", "Color": "number"}

SCALARS = {
    "bool", "char", "short", "int", "long", "long long", "float", "double", "unsigned char",
    "unsigned short", "unsigned int", "unsigned long", "unsigned long long", "size_t",
}

# a matching adjacent count name labels the report reason as "array"
COUNT_NAME = re.compile(r"^(count|instances|len|length|.*Count)$")

# raymath helpers, only returned by the *ToFloatV functions the classifier rejects
SKIPPED_STRUCTS = {"float3", "float16"}

# Luau keywords, which cannot be parameter names in the definitions file
KEYWORDS = {
    "and", "break", "continue", "do", "else", "elseif", "end", "export", "false", "for",
    "function", "if", "in", "local", "nil", "not", "or", "repeat", "return", "then",
    "true", "type", "typeof", "until", "while",
}


def load_api(directory: Path) -> dict:
    """raylib.h and raymath.h, parsed by rlparser into one api description."""
    api = read_json(directory / "raylib_api.json")
    merge(api, read_json(directory / "raymath_api.json"))
    return api


def read_json(path: Path) -> dict:
    if not path.is_file():
        sys.exit(f"{path.name} not found at {path}, run ./build.sh bindgen")
    return json.loads(sanitize(path.read_text()))


def merge(api: dict, extra: dict) -> None:
    """Add what the second header brings, keeping raylib.h's copy of anything shared.

    raymath re-declares the vector structs, the Quaternion alias and PI, and
    defines no function raylib.h already has.
    """
    for section, entries in extra.items():
        seen = {entry["name"] for entry in api.get(section, [])}
        api.setdefault(section, []).extend(e for e in entries if e["name"] not in seen)


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
        self.alias = base  # the spelling raylib uses here, Quaternion rather than Vector4
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
        self.structs = {s["name"]: s for s in api["structs"] if s["name"] not in SKIPPED_STRUCTS}
        self.callbacks = {c["name"] for c in api["callbacks"]}
        self.userdata = [name for name in self.structs if name not in NATIVE_STRUCTS]
        self.tag = {name: i + 1 for i, name in enumerate(self.userdata)}  # tag 0 is plain userdata
        self.owns = self._ownership()
        # aliases the definitions file declares, so a signature may name them
        self.exported_aliases = {
            a["name"] for a in api["aliases"] if not a["name"].startswith("*") and self.is_userdata(a["type"])
        }

    def parse(self, raw: str) -> Type:
        return Type(raw, self.aliases)

    def _ownership(self) -> dict:
        """Find structs that transitively own an allocation."""
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
    """-> (plain | unload | scope | adapted | host | unsupported, reason)

    Structural rules decide everything a signature can answer. The tables above
    hold only what it cannot.
    """
    name = fn["name"]
    if name in HOST_FUNCTIONS:
        return "host", ""
    if name in DENIED:
        return "unsupported", DENIED[name]
    if name in ADAPTED:
        return "adapted", ADAPTED[name][0]

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
        nxt = params[i + 1] if i + 1 < len(params) else None
        counted = nxt and api.parse(nxt["type"]).base in SCALARS and COUNT_NAME.match(nxt.get("name", ""))
        if not api.is_userdata(t.base) or t.const or t.ptr > 1:
            return "unsupported", f"{t} {'array' if counted or t.const else 'out parameter'}"
        # a lone writable userdata pointer is an in-place mutator, one adjacent
        # count means it is an array instead and C cannot tell the two apart
        if counted:
            return "unsupported", f"{t} array"

    for scope in SCOPES:
        if name in (f"Begin{scope}", f"End{scope}"):
            return "scope", scope
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
    # signatures read as raylib writes them, the alias is exported as its target
    if t.alias in api.exported_aliases:
        return t.alias
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
            continue
        if api.value_kind(t) is None:
            continue
        if api.owns.get(t.base):
            continue
        if api.is_handle(t.base) and (name, f["name"]) not in BORROWED_FIELDS:
            continue
        out.append((f["name"], lua_type(api, t)))
    return out


def writable(api: Api, struct: str, field: str) -> bool:
    """Return whether a field can be assigned without corrupting a handle.

    A handle's integral fields can describe allocation bounds and stay read only.
    """
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
            out.append((d["name"], describe(d.get("description")), float(eval(expr, {"PI": math.pi}))))
    return out


GENERATED = "// generated by tools/bindgen.py from raylib {version}, do not edit\n"


def emit_header(api: Api, version: str) -> str:
    out = [GENERATED.format(version=version), "#pragma once\n\n"]
    out.append('#include "bind/bind.hpp"\n#include "raylib.h"\n\nnamespace bind {\n\n')
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
    out.append("// one call per VM, creates the raylib global and the userdata metatables\nvoid OpenRaylib(lua_State *L);\n")
    out.append(
        "\n// names the definitions file promises, installed by src/vfs.cpp. the host\n"
        "// checks them after OpenLoaders so a missing one cannot reach a script as nil\n"
        f"inline constexpr const char *kHostFunctions[] = {{\n"
        + "".join(f'    "{n}",\n' for n in sorted(HOST_FUNCTIONS))
        + "};\n"
    )
    return "".join(out)


def emit_source(api: Api, bound: list[tuple[dict, str]], unsupported: list[tuple[str, str]], version: str) -> str:
    out = [
        GENERATED.format(version=version),
        '\n#include "bind/raylib_bind.hpp"\n\n#include "bind/rl_adapters.hpp"\n#include "bind/rl_types.hpp"\n\n',
        # raymath is header only, this keeps its definitions inside this one unit
        '#define RAYMATH_STATIC_INLINE\n#include "raymath.h"\n\n',
    ]
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
        elif kind == "adapted":
            out.append(f'    {{"{name}", &adapt::{name}}},\n')
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
        f"\n// Not bound: {len(unsupported)} of {len(api.raw['functions'])} raylib functions, with the reason for each\n"
        "//\n"
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
                text = f"{text} (borrowed, do not unload it)".lstrip(". ")
            out.append(doc("    ", text))
            access = "" if writable(api, name, field) else "read "
            out.append(f"    {access}{field}: {ltype}\n")
        out.append("end\n\n")

    # only exported aliases enter a definition file's global type namespace
    for a in api.raw["aliases"]:
        if a["name"] in api.exported_aliases:
            out.append(f"export type {a['name']} = {a['type']}\n")
    out.append("\ndeclare DEV: boolean\n\ndeclare raylib: {\n")

    for fn, kind in bound:
        override = ADAPTED[fn["name"]][1] if kind == "adapted" else None
        if override is None:
            params = ", ".join(f"{param_name(p, i)}: {lua_type(api, api.parse(p['type']))}"
                               for i, p in enumerate(fn.get("params", [])))
            override = f"({params}) -> {lua_type(api, api.parse(fn['returnType']))}"
        out.append(doc("    ", describe(fn.get("description"))))
        out.append(f"    {fn['name']}: {override},\n")

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
    for table, entries in (("HOST_FUNCTIONS", HOST_FUNCTIONS), ("DENIED", DENIED), ("ADAPTED", ADAPTED)):
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

    (ROOT / "src" / "bind" / "raylib_bind.hpp").write_text(emit_header(api, version))
    (ROOT / "src" / "bind" / "raylib_bind.cpp").write_text(emit_source(api, bound, unsupported, version))
    defs = emit_defs(api, bound, version)
    host = "".join(doc("    ", d) + f"    {n}: {sig},\n" for n, (sig, d) in sorted(HOST_FUNCTIONS.items()))
    (ROOT / "types" / "raylib.d.luau").write_text(defs.replace("\n    Vector2:", f"\n{host}\n    Vector2:", 1))

    adapted = sum(1 for _, k in bound if k != "plain")
    print(f"raylib {version}: {len(bound)} functions bound ({adapted} adapted), {len(hosted)} host provided,")
    print(f"  {len(api.userdata)} userdata types, {len(unsupported)} unsupported")
    print("  see the report at the bottom of src/bind/raylib_bind.cpp")


if __name__ == "__main__":
    main()
