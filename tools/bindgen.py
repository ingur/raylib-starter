"""Regenerate the raylib bindings from pinned raylib and pocketpy checkouts.

Usage: bindgen.py <raylib-src> <pocketpy-src>

Writes src/raylib_module.c, src/rl_colors.c, types/raylib.pyi,
and copies types/vmath.pyi and types/stdc.pyi from pocketpy.
"""

import json
import re
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def load_api(raylib_src: Path) -> dict:
    for rel in ("tools/rlparser/output/raylib_api.json", "parser/output/raylib_api.json"):
        path = raylib_src / rel
        if path.exists():
            api = json.loads(sanitize(path.read_text()))
            resolve_pointer_aliases(api)
            return api
    sys.exit(f"raylib_api.json not found under {raylib_src}")


def sanitize(text: str) -> str:
    def escape(m: re.Match) -> str:
        body = m.group(2).replace('\\"', '"').replace('"', '\\"')
        return m.group(1) + body + m.group(3)

    return re.sub(r'^(\s*"description": ")(.*)(",?)$', escape, text, flags=re.M)


def resolve_pointer_aliases(api: dict) -> None:
    """rlparser encodes `typedef T *Alias` as alias name "*Alias".
    Rewrite uses of such aliases to their pointer type, ffigen maps those to intptr."""
    pointers = {a["name"][1:]: f"{a['type']} *" for a in api["aliases"] if a["name"].startswith("*")}
    api["aliases"] = [a for a in api["aliases"] if not a["name"].startswith("*")]

    def rewrite(type_str: str) -> str:
        for alias, target in pointers.items():
            type_str = re.sub(rf"\b{alias}\b", target, type_str)
        return type_str

    for struct in api["structs"]:
        for field in struct["fields"]:
            field["type"] = rewrite(field["type"])
    for function in api["functions"]:
        function["returnType"] = rewrite(function["returnType"])
        for param in function.get("params", []):
            param["type"] = rewrite(param["type"])


def gen_module(api: dict, pocketpy_src: Path) -> None:
    sys.path.insert(0, str(pocketpy_src / "ffigen"))
    from ffigen.library import Library
    from ffigen.converters import set_vmath_converter

    (ROOT / "src").mkdir(exist_ok=True)
    set_vmath_converter("Vector2", "vec2")
    set_vmath_converter("Vector3", "vec3")
    set_vmath_converter("Color", "color32")
    lib = Library.from_raylib(api)
    lib.build(includes=["raylib.h"], glue_dir=str(ROOT / "src"), stub_dir=str(ROOT / "types"))
    (ROOT / "src" / "raylib.c").replace(ROOT / "src" / "raylib_module.c")


def gen_colors(api: dict) -> None:
    colors = []
    for define in api["defines"]:
        if define["type"] != "COLOR":
            continue
        r, g, b, a = re.search(r"\{ *(\d+), *(\d+), *(\d+), *(\d+) *\}", define["value"]).groups()
        colors.append((define["name"], r, g, b, a))

    lines = [
        "#include \"pocketpy.h\"",
        "",
        "#define ADD_COLOR(name, r, g, b, a) \\",
        "    py_newcolor32(py_emplacedict(mod, py_name(#name)), (c11_color32){{r, g, b, a}})",
        "",
        "void py__add_raylib_colors(void) {",
        "    py_GlobalRef mod = py_getmodule(\"raylib\");",
    ]
    lines += [f"    ADD_COLOR({name}, {r}, {g}, {b}, {a});" for name, r, g, b, a in colors]
    lines += ["}", ""]
    (ROOT / "src" / "rl_colors.c").write_text("\n".join(lines))

    stub = ROOT / "types" / "raylib.pyi"
    entries = "".join(f"{name}: color32\n" for name, *_ in colors)
    stub.write_text(stub.read_text() + "\n# colors\n" + entries)


def copy_stubs(pocketpy_src: Path) -> None:
    for name in ("vmath.pyi", "stdc.pyi", "array2d.pyi", "easing.pyi"):
        shutil.copy(pocketpy_src / "include" / "typings" / name, ROOT / "types" / name)


def main() -> None:
    if len(sys.argv) != 3:
        sys.exit(__doc__.strip())
    raylib_src, pocketpy_src = Path(sys.argv[1]), Path(sys.argv[2])
    api = load_api(raylib_src)
    (ROOT / "types").mkdir(exist_ok=True)
    gen_module(api, pocketpy_src)
    gen_colors(api)
    copy_stubs(pocketpy_src)
    print(f"bound {len(api['functions'])} functions, {len(api['enums'])} enums")


if __name__ == "__main__":
    main()
