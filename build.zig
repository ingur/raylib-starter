const std = @import("std");
const raylib_zig = @import("raylib");

const name = "game";

const assets_pak = "assets.pak";

// raylib and miniz use C. The host and Luau use C++17.
// disable FP contraction for consistent results across targets
const cflags = [_][]const u8{ "-std=gnu11", "-ffp-contract=off" };
const cxxflags = [_][]const u8{ "-std=c++17", "-ffp-contract=off" };

// match Luau.VM's upstream -fno-math-errno flag
const vm_cxxflags = cxxflags ++ [_][]const u8{"-fno-math-errno"};

// miniz 3.1.2 has no amalgamated source file
const miniz_srcs = [_][]const u8{ "miniz.c", "miniz_zip.c", "miniz_tinfl.c", "miniz_tdef.c" };

// Luau has no Zig build, so compile the embedded libraries from source
// Ast, Bytecode, and CodeGen link Luau.Common in 0.732
const luau_src_dirs = [_][]const u8{ "Common/src", "Ast/src", "Bytecode/src", "Compiler/src" };
const luau_includes = [_][]const u8{
    "Common/include",
    "Ast/include",
    "Bytecode/include",
    "Compiler/include",
    "VM/include",
    "VM/src",
};

// the jit is x64/A64 only, the web build compiles none of it
const luau_codegen_includes = [_][]const u8{ "CodeGen/include", "CodeGen/src" };

pub fn build(b: *std.Build) !void {
    // `zig build web -Dweb` skips the native graph and its system libraries
    const web_only = b.option(bool, "web", "configure the web target only") orelse false;

    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});
    const ndebug = optimize != .Debug;

    const raylib_dep = if (web_only) b.dependency("raylib", .{}) else b.dependency("raylib", .{
        .target = target,
        .optimize = optimize,
        .linux_display_backend = raylib_zig.LinuxDisplayBackend.Both,
    });
    const miniz_dep = b.dependency("miniz", .{});
    const luau_dep = b.dependency("luau", .{});

    const luau_root = luau_dep.builder.pathFromRoot(".");
    const game_srcs = try findCSources(b, b.pathFromRoot("."), &.{"src"});
    const luau_srcs = try findCSources(b, luau_root, &luau_src_dirs);
    const luau_vm_srcs = try findCSources(b, luau_root, &.{"VM/src"});
    const luau_codegen_srcs = try findCSources(b, luau_root, &.{"CodeGen/src"});

    if (!web_only) try native(b, .{
        .target = target,
        .optimize = optimize,
        .ndebug = ndebug,
        .raylib_dep = raylib_dep,
        .miniz_dep = miniz_dep,
        .luau_dep = luau_dep,
        .game_srcs = game_srcs,
        .luau_srcs = luau_srcs,
        .luau_vm_srcs = luau_vm_srcs,
        .luau_codegen_srcs = luau_codegen_srcs,
    });

    try web(b, .{
        .raylib_dep = raylib_dep,
        .miniz_dep = miniz_dep,
        .luau_dep = luau_dep,
        .game_srcs = game_srcs,
        .luau_srcs = luau_srcs,
        .luau_vm_srcs = luau_vm_srcs,
    });

    // the raylib Zig package omits raylib_api.json, so fetch it from the pinned tag
    const fetch_api = b.addSystemCommand(&.{ "curl", "-fsSL", b.fmt(
        "https://raw.githubusercontent.com/raysan5/raylib/{s}/tools/rlparser/output/raylib_api.json",
        .{raylibTag()},
    ), "-o" });
    const api_dir = b.addWriteFiles();
    _ = api_dir.addCopyFile(fetch_api.addOutputFileArg("raylib_api.json"), "raylib_api.json");

    // upstream publishes no raymath_api.json, so build raylib's own parser and
    // run it over the pinned header
    const fetch_parser = b.addSystemCommand(&.{ "curl", "-fsSL", b.fmt(
        "https://raw.githubusercontent.com/raysan5/raylib/{s}/tools/rlparser/rlparser.c",
        .{raylibTag()},
    ), "-o" });
    const parser_src = fetch_parser.addOutputFileArg("rlparser.c");

    const build_parser = b.addSystemCommand(&.{ b.graph.zig_exe, "cc", "-o" });
    const parser_exe = build_parser.addOutputFileArg("rlparser");
    build_parser.addFileArg(parser_src);

    const parse_raymath = std.Build.Step.Run.create(b, "run rlparser");
    parse_raymath.addFileArg(parser_exe);
    parse_raymath.addArg("-i");
    parse_raymath.addFileArg(raylib_dep.path("src/raymath.h"));
    parse_raymath.addArg("-o");
    const raymath_api = parse_raymath.addOutputFileArg("raymath_api.json");
    parse_raymath.addArgs(&.{ "-f", "JSON", "-d", "RMAPI" });
    _ = api_dir.addCopyFile(raymath_api, "raymath_api.json");

    const bindgen = b.addSystemCommand(&.{ "python3", "tools/bindgen.py" });
    bindgen.addDirectoryArg(api_dir.getDirectory());
    bindgen.setCwd(b.path(""));
    bindgen.has_side_effects = true;
    b.step("bindgen", "Regenerate the raylib bindings and Luau type definitions")
        .dependOn(&bindgen.step);

    var flags: []const u8 = b.fmt("-xc++\n-std=c++17\n-Isrc\n-I{s}\n-I{s}\n", .{
        raylib_dep.builder.pathFromRoot("src"),
        miniz_dep.builder.pathFromRoot("."),
    });
    for (luau_includes ++ luau_codegen_includes) |inc|
        flags = b.fmt("{s}-I{s}\n", .{ flags, luau_dep.builder.pathFromRoot(inc) });
    flags = b.fmt("{s}-DASSETS_PAK=\"{s}\"\n-DGAME_CODEGEN=1\n", .{ flags, assets_pak });

    const editor_flags = b.addUpdateSourceFiles();
    editor_flags.addBytesToSource(flags, "compile_flags.txt");
    b.getInstallStep().dependOn(&editor_flags.step);
}

const NativeOptions = struct {
    target: std.Build.ResolvedTarget,
    optimize: std.builtin.OptimizeMode,
    ndebug: bool,
    raylib_dep: *std.Build.Dependency,
    miniz_dep: *std.Build.Dependency,
    luau_dep: *std.Build.Dependency,
    game_srcs: []const []const u8,
    luau_srcs: []const []const u8,
    luau_vm_srcs: []const []const u8,
    luau_codegen_srcs: []const []const u8,
};

fn native(b: *std.Build, opts: NativeOptions) !void {
    const raylib = opts.raylib_dep.artifact("raylib");
    const is_linux = opts.target.result.os.tag == .linux;
    const is_windows = opts.target.result.os.tag == .windows;

    // work around Zig issue 20476. It can add resolved system .so files to
    // raylib's static archive, so relink them on the executable
    var sys_libs: std.array_list.Managed(std.Build.Module.SystemLib) = .init(b.allocator);
    var lib_dirs: std.array_list.Managed([]const u8) = .init(b.allocator);
    if (is_linux) {
        const pkg_config = b.graph.environ_map.get("PKG_CONFIG") orelse "pkg-config";
        // include host library paths when targeting versioned glibc
        const out = b.run(&.{
            "env",
            "PKG_CONFIG_ALLOW_SYSTEM_CFLAGS=1",
            "PKG_CONFIG_ALLOW_SYSTEM_LIBS=1",
            pkg_config,
            "--cflags",
            "--libs-only-L",
            "x11",
            "xext",
            "xrandr",
            "xinerama",
            "xcursor",
            "xi",
            "xrender",
            "xfixes",
            "gl",
            "wayland-client",
            "wayland-cursor",
            "wayland-egl",
            "xkbcommon",
        });
        var it = std.mem.tokenizeAny(u8, out, " \n\r\t");
        while (it.next()) |tok| {
            if (std.mem.startsWith(u8, tok, "-I")) {
                // keep Zig's libc headers ahead of host headers
                raylib.root_module.addAfterIncludePath(.{ .cwd_relative = b.dupe(tok[2..]) });
            } else if (std.mem.startsWith(u8, tok, "-L")) {
                try lib_dirs.append(b.dupe(tok[2..]));
            }
        }
        var kept: std.array_list.Managed(std.Build.Module.LinkObject) = .init(b.allocator);
        for (raylib.root_module.link_objects.items) |lo| switch (lo) {
            .system_lib => |sl| try sys_libs.append(sl),
            else => try kept.append(lo),
        };
        raylib.root_module.link_objects.clearRetainingCapacity();
        try raylib.root_module.link_objects.appendSlice(b.allocator, kept.items);
    }

    const luau_mod = b.createModule(.{
        .target = opts.target,
        .optimize = opts.optimize,
        .link_libc = true,
        .link_libcpp = true,
    });
    for (luau_includes ++ luau_codegen_includes) |inc|
        luau_mod.addIncludePath(opts.luau_dep.path(inc));
    const luau_path = opts.luau_dep.path("");
    luau_mod.addCSourceFiles(.{ .root = luau_path, .files = opts.luau_srcs, .flags = &cxxflags });
    luau_mod.addCSourceFiles(.{ .root = luau_path, .files = opts.luau_vm_srcs, .flags = &vm_cxxflags });
    luau_mod.addCSourceFiles(.{ .root = luau_path, .files = opts.luau_codegen_srcs, .flags = &cxxflags });
    // without this Luau keeps every LUAU_ASSERT, including the liveness check on
    // each value copy in the interpreter
    if (opts.ndebug) luau_mod.addCMacro("NDEBUG", "1");
    const luau = b.addLibrary(.{
        .name = "luau",
        .linkage = .static,
        .root_module = luau_mod,
    });

    const exe_mod = b.createModule(.{
        .target = opts.target,
        .optimize = opts.optimize,
        .link_libc = true,
        .link_libcpp = true,
        .strip = opts.ndebug,
    });
    exe_mod.addCSourceFiles(.{ .files = opts.game_srcs, .flags = &cxxflags });
    exe_mod.addCSourceFiles(.{ .root = opts.miniz_dep.path(""), .files = &miniz_srcs, .flags = &cflags });
    exe_mod.addIncludePath(opts.miniz_dep.path(""));
    exe_mod.addIncludePath(b.path("src"));
    for (luau_includes ++ luau_codegen_includes) |inc|
        exe_mod.addIncludePath(opts.luau_dep.path(inc));
    exe_mod.addIncludePath(opts.raylib_dep.path("src"));
    exe_mod.addCMacro("ASSETS_PAK", "\"" ++ assets_pak ++ "\"");
    exe_mod.addCMacro("GAME_CODEGEN", "1");
    if (opts.ndebug) exe_mod.addCMacro("NDEBUG", "1");
    exe_mod.linkLibrary(raylib);
    exe_mod.linkLibrary(luau);
    for (lib_dirs.items) |d| exe_mod.addLibraryPath(.{ .cwd_relative = d });
    for (sys_libs.items) |sl| exe_mod.linkSystemLibrary(sl.name, .{
        .needed = sl.needed,
        .weak = sl.weak,
        .use_pkg_config = .no,
        .preferred_link_mode = sl.preferred_link_mode,
        .search_strategy = sl.search_strategy,
    });

    const exe = b.addExecutable(.{ .name = name, .root_module = exe_mod });
    if (is_linux) {
        // let host X11 and Wayland libraries resolve their runtime dependencies
        exe.linker_allow_shlib_undefined = true;
    }
    if (is_windows and opts.ndebug) exe.subsystem = .Windows;
    b.installArtifact(exe);

    const pak = pakStep(b, "zig-out/bin");
    b.getInstallStep().dependOn(&pak.step);

    // release builds resolve the pak next to the executable, run from there
    const run_cmd = b.addSystemCommand(&.{"zig-out/bin/" ++ name});
    run_cmd.setCwd(b.path(""));
    run_cmd.step.dependOn(b.getInstallStep());
    b.step("run", "Run the game").dependOn(&run_cmd.step);
}

const WebOptions = struct {
    raylib_dep: *std.Build.Dependency,
    miniz_dep: *std.Build.Dependency,
    luau_dep: *std.Build.Dependency,
    game_srcs: []const []const u8,
    luau_srcs: []const []const u8,
    luau_vm_srcs: []const []const u8,
};

// a translation unit and whatever it needs on top of the flags its extension implies
const WebSource = struct {
    path: std.Build.LazyPath,
    extra: []const []const u8 = &.{},
};

fn web(b: *std.Build, opts: WebOptions) !void {
    var srcs: std.array_list.Managed(WebSource) = .init(b.allocator);
    for ([_][]const u8{
        "src/rcore.c", "src/rshapes.c", "src/rtextures.c",
        "src/rtext.c", "src/rmodels.c", "src/raudio.c",
    }) |f| try srcs.append(.{ .path = opts.raylib_dep.path(f) });
    for (miniz_srcs) |f| try srcs.append(.{ .path = opts.miniz_dep.path(f) });
    for (opts.luau_srcs) |f| try srcs.append(.{ .path = opts.luau_dep.path(f) });
    for (opts.luau_vm_srcs) |f| try srcs.append(.{
        .path = opts.luau_dep.path(f),
        .extra = &.{"-fno-math-errno"},
    });
    for (opts.game_srcs) |f| try srcs.append(.{ .path = b.path(f) });

    // Luau throws C++ exceptions, so WebAssembly objects and the link need EH
    const link = b.addSystemCommand(&.{ "emcc", "-O3", "-fwasm-exceptions" });
    link.setName("emcc link");
    link.setCwd(b.path(""));
    link.has_side_effects = true;

    // per-source objects, cached by zig so rebuilds only touch changed files
    for (srcs.items) |src| {
        const cc = b.addSystemCommand(&.{ "emcc", "-c", "-O3", "-fwasm-exceptions" });
        const obj = objectName(b, src.path);
        cc.setName(b.fmt("emcc {s}", .{obj}));
        cc.addArgs(if (isCpp(src.path)) &cxxflags else &cflags);
        cc.addArgs(src.extra);
        if (src.path == .dependency) cc.addArg("-w");
        cc.addArgs(&.{ "-DNDEBUG", "-DPLATFORM_WEB", "-DGRAPHICS_API_OPENGL_ES3" });
        cc.addArg("-DASSETS_PAK=\"" ++ assets_pak ++ "\"");
        cc.addPrefixedDirectoryArg("-I", opts.raylib_dep.path("src"));
        cc.addPrefixedDirectoryArg("-I", opts.miniz_dep.path(""));
        for (luau_includes) |inc| cc.addPrefixedDirectoryArg("-I", opts.luau_dep.path(inc));
        cc.addPrefixedDirectoryArg("-I", b.path("src"));
        cc.addFileArg(src.path);
        cc.addArgs(&.{ "-MD", "-MF" });
        _ = cc.addDepFileOutputArg(b.fmt("{s}.d", .{obj}));
        cc.addArg("-o");
        link.addFileArg(cc.addOutputFileArg(b.fmt("{s}.o", .{obj})));
    }

    const pak = pakStep(b, "zig-out/web");
    link.addArgs(&.{
        "-sUSE_GLFW=3",
        "-sEXPORTED_RUNTIME_METHODS=ccall",
        "-sALLOW_MEMORY_GROWTH=1",
        // Emscripten defaults to WebGL 1. raylib's ES3 backend needs WebGL 2.
        "-sMIN_WEBGL_VERSION=2",
        "-sMAX_WEBGL_VERSION=2",
        "-lidbfs.js",
    });
    link.addArg("--shell-file");
    link.addFileArg(b.path("src/web/shell.html"));
    link.addArg("--pre-js");
    link.addFileArg(b.path("src/web/save.js"));
    link.addArgs(&.{ "--preload-file", "zig-out/web/" ++ assets_pak ++ "@/" ++ assets_pak });
    link.addArgs(&.{ "-o", "zig-out/web/index.html" });
    link.step.dependOn(&pak.step);
    b.step("web", "Build the web target").dependOn(&link.step);
}

fn pakStep(b: *std.Build, dir: []const u8) *std.Build.Step.Run {
    const mkdir = b.addSystemCommand(&.{ "mkdir", "-p", dir });
    mkdir.setCwd(b.path(""));
    // -FS removes entries deleted from the source directories
    const pak = b.addSystemCommand(&.{ "zip", "-qrFS" });
    pak.addArg(b.fmt("{s}/{s}", .{ dir, assets_pak }));
    pak.addArgs(&.{ "assets", "game" });
    pak.setCwd(b.path(""));
    pak.has_side_effects = true;
    pak.step.dependOn(&mkdir.step);
    return pak;
}

fn findCSources(b: *std.Build, root: []const u8, subs: []const []const u8) ![]const []const u8 {
    var list: std.array_list.Managed([]const u8) = .init(b.allocator);
    for (subs) |sub| try walkCSources(b, root, sub, &list);
    std.mem.sort([]const u8, list.items, {}, struct {
        fn lt(_: void, lhs: []const u8, rhs: []const u8) bool {
            return std.mem.lessThan(u8, lhs, rhs);
        }
    }.lt);
    return list.items;
}

fn walkCSources(b: *std.Build, root: []const u8, sub: []const u8, list: *std.array_list.Managed([]const u8)) !void {
    var dir = try b.build_root.handle.openDir(b.graph.io, b.pathJoin(&.{ root, sub }), .{ .iterate = true });
    defer dir.close(b.graph.io);
    var it = dir.iterate();
    while (try it.next(b.graph.io)) |entry| {
        const child = b.pathJoin(&.{ sub, entry.name });
        switch (entry.kind) {
            .directory => try walkCSources(b, root, child, list),
            .file => if (std.mem.endsWith(u8, entry.name, ".c") or
                std.mem.endsWith(u8, entry.name, ".cpp")) try list.append(child),
            else => {},
        }
    }
}

fn subPath(src: std.Build.LazyPath) []const u8 {
    return switch (src) {
        .src_path => |p| p.sub_path,
        .dependency => |p| p.sub_path,
        else => unreachable,
    };
}

fn isCpp(src: std.Build.LazyPath) bool {
    return std.mem.endsWith(u8, subPath(src), ".cpp");
}

fn objectName(b: *std.Build, src: std.Build.LazyPath) []const u8 {
    const rel = subPath(src);
    const obj = b.dupe(rel[0..std.mem.lastIndexOfScalar(u8, rel, '.').?]);
    std.mem.replaceScalar(u8, obj, '/', '_');
    return obj;
}

fn raylibTag() []const u8 {
    const zon = @embedFile("build.zig.zon");
    const marker = "raysan5/raylib/archive/refs/tags/";
    const start = std.mem.indexOf(u8, zon, marker).? + marker.len;
    return zon[start..std.mem.indexOfPos(u8, zon, start, ".tar.gz").?];
}
