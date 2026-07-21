const std = @import("std");
const raylib_zig = @import("raylib");

// Rename the binary here.
const name = "game";

// Packed assets file name. The pak is a plain zip.
const assets_pak = "assets.pak";

const pk_defines = [_][2][]const u8{
    .{ "PK_ENABLE_OS", "1" },
    .{ "PK_ENABLE_THREADS", "0" },
    .{ "PK_ENABLE_DETERMINISM", "1" },
    .{ "PK_ENABLE_WATCHDOG", "0" },
    .{ "PK_ENABLE_CUSTOM_SNAME", "0" },
    .{ "PK_ENABLE_MIMALLOC", "0" },
};

// pocketpy relies on implementation defined pointer casts
const cflags = [_][]const u8{ "-std=gnu11", "-fno-sanitize=undefined" };

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
    const pocketpy_dep = b.dependency("pocketpy", .{});
    const miniz_dep = b.dependency("miniz", .{});

    const game_srcs = try findCSources(b, b.pathFromRoot("."), "src");
    const pk_srcs = try findCSources(b, pocketpy_dep.builder.pathFromRoot("."), "src");

    if (!web_only) try native(b, .{
        .target = target,
        .optimize = optimize,
        .ndebug = ndebug,
        .raylib_dep = raylib_dep,
        .pocketpy_dep = pocketpy_dep,
        .miniz_dep = miniz_dep,
        .game_srcs = game_srcs,
        .pk_srcs = pk_srcs,
    });

    try web(b, raylib_dep, pocketpy_dep, miniz_dep, game_srcs, pk_srcs);

    // the raylib package omits its parser output, fetch the pinned one
    const fetch_api = b.addSystemCommand(&.{ "curl", "-fsSL", b.fmt(
        "https://raw.githubusercontent.com/raysan5/raylib/{s}/tools/rlparser/output/raylib_api.json",
        .{raylibTag()},
    ), "-o" });
    const api_dir = b.addWriteFiles();
    _ = api_dir.addCopyFile(
        fetch_api.addOutputFileArg("raylib_api.json"),
        "tools/rlparser/output/raylib_api.json",
    );

    const bindgen = b.addSystemCommand(&.{ "python3", "tools/bindgen.py" });
    bindgen.addDirectoryArg(api_dir.getDirectory());
    bindgen.addDirectoryArg(pocketpy_dep.path(""));
    bindgen.setCwd(b.path(""));
    bindgen.has_side_effects = true;
    b.step("bindgen", "Regenerate the python bindings and type stubs").dependOn(&bindgen.step);

    // clangd reads the generated compile_flags.txt
    const editor_flags = b.addUpdateSourceFiles();
    editor_flags.addBytesToSource(b.fmt("-I{s}\n-I{s}\n-I{s}\n-DASSETS_PAK=\"{s}\"\n", .{
        raylib_dep.builder.pathFromRoot("src"),
        pocketpy_dep.builder.pathFromRoot("include"),
        miniz_dep.builder.pathFromRoot("."),
        assets_pak,
    }), "compile_flags.txt");
    b.getInstallStep().dependOn(&editor_flags.step);
}

const NativeOptions = struct {
    target: std.Build.ResolvedTarget,
    optimize: std.builtin.OptimizeMode,
    ndebug: bool,
    raylib_dep: *std.Build.Dependency,
    pocketpy_dep: *std.Build.Dependency,
    miniz_dep: *std.Build.Dependency,
    game_srcs: []const []const u8,
    pk_srcs: []const []const u8,
};

fn native(b: *std.Build, opts: NativeOptions) !void {
    const raylib = opts.raylib_dep.artifact("raylib");
    const is_linux = opts.target.result.os.tag == .linux;
    const is_windows = opts.target.result.os.tag == .windows;

    // zig resolves system libs to paths and packs them into static archives,
    // so raylib's are queried through pkg-config here and linked on the exe
    var sys_libs: std.array_list.Managed([]const u8) = .init(b.allocator);
    var lib_dirs: std.array_list.Managed([]const u8) = .init(b.allocator);
    if (is_linux) {
        const out = b.run(&.{
            "pkg-config",     "--cflags",       "--libs-only-L",
            "x11",            "xext",           "xrandr",
            "xinerama",       "xcursor",        "xi",
            "xrender",        "xfixes",         "gl",
            "wayland-client", "wayland-cursor", "wayland-egl",
            "xkbcommon",
        });
        var it = std.mem.tokenizeAny(u8, out, " \n\r\t");
        while (it.next()) |tok| {
            if (std.mem.startsWith(u8, tok, "-I")) {
                raylib.root_module.addIncludePath(.{ .cwd_relative = b.dupe(tok[2..]) });
            } else if (std.mem.startsWith(u8, tok, "-L")) {
                try lib_dirs.append(b.dupe(tok[2..]));
            }
        }
        var kept: std.array_list.Managed(std.Build.Module.LinkObject) = .init(b.allocator);
        for (raylib.root_module.link_objects.items) |lo| switch (lo) {
            .system_lib => |sl| try sys_libs.append(sl.name),
            else => try kept.append(lo),
        };
        raylib.root_module.link_objects.clearRetainingCapacity();
        try raylib.root_module.link_objects.appendSlice(b.allocator, kept.items);
    }

    const pk_mod = b.createModule(.{
        .target = opts.target,
        .optimize = opts.optimize,
        .link_libc = true,
    });
    pk_mod.addIncludePath(opts.pocketpy_dep.path("include"));
    for (pk_defines) |d| pk_mod.addCMacro(d[0], d[1]);
    if (opts.ndebug) pk_mod.addCMacro("NDEBUG", "");
    pk_mod.addCSourceFiles(.{
        .root = opts.pocketpy_dep.path(""),
        .files = opts.pk_srcs,
        .flags = &cflags,
    });
    if (is_windows) {
        // upstream includes <WinSock2.h>, zig's bundled mingw headers are lowercase
        const shim = b.addWriteFiles();
        pk_mod.addIncludePath(shim.add("WinSock2.h", "#include <winsock2.h>\n").dirname());
        pk_mod.linkSystemLibrary("ws2_32", .{});
    }
    const pocketpy = b.addLibrary(.{
        .name = "pocketpy",
        .linkage = .static,
        .root_module = pk_mod,
    });

    const exe_mod = b.createModule(.{
        .target = opts.target,
        .optimize = opts.optimize,
        .link_libc = true,
        .strip = opts.ndebug,
    });
    exe_mod.addCSourceFiles(.{ .files = opts.game_srcs, .flags = &cflags });
    exe_mod.addCSourceFile(.{ .file = opts.miniz_dep.path("miniz.c"), .flags = &cflags });
    exe_mod.addIncludePath(opts.miniz_dep.path(""));
    exe_mod.addIncludePath(opts.pocketpy_dep.path("include"));
    exe_mod.addIncludePath(opts.raylib_dep.path("src"));
    exe_mod.addCMacro("ASSETS_PAK", "\"" ++ assets_pak ++ "\"");
    if (opts.ndebug) exe_mod.addCMacro("NDEBUG", "");
    exe_mod.linkLibrary(raylib);
    exe_mod.linkLibrary(pocketpy);
    for (lib_dirs.items) |d| exe_mod.addLibraryPath(.{ .cwd_relative = d });
    for (sys_libs.items) |l| exe_mod.linkSystemLibrary(l, .{ .use_pkg_config = .no });

    const exe = b.addExecutable(.{ .name = name, .root_module = exe_mod });
    if (is_linux) {
        // host X11 and wayland libs resolve their own dependencies at runtime
        exe.linker_allow_shlib_undefined = true;
    }
    if (is_windows and opts.ndebug) exe.subsystem = .Windows;
    b.installArtifact(exe);

    // pack assets/ and game/ into a zip beside the binary
    const pak = pakStep(b, "zig-out/bin");
    b.getInstallStep().dependOn(&pak.step);

    // release builds resolve the pak next to the executable, run from there
    const run_cmd = b.addSystemCommand(&.{"zig-out/bin/" ++ name});
    run_cmd.setCwd(b.path(""));
    run_cmd.step.dependOn(b.getInstallStep());
    b.step("run", "Run the game").dependOn(&run_cmd.step);
}

fn web(
    b: *std.Build,
    raylib_dep: *std.Build.Dependency,
    pocketpy_dep: *std.Build.Dependency,
    miniz_dep: *std.Build.Dependency,
    game_srcs: []const []const u8,
    pk_srcs: []const []const u8,
) !void {
    var srcs: std.array_list.Managed(std.Build.LazyPath) = .init(b.allocator);
    for ([_][]const u8{
        "src/rcore.c", "src/rshapes.c", "src/rtextures.c",
        "src/rtext.c", "src/rmodels.c", "src/raudio.c",
    }) |f| try srcs.append(raylib_dep.path(f));
    for (pk_srcs) |f| try srcs.append(pocketpy_dep.path(f));
    try srcs.append(miniz_dep.path("miniz.c"));
    for (game_srcs) |f| try srcs.append(b.path(f));

    const link = b.addSystemCommand(&.{ "emcc", "-Os" });
    link.setName("emcc link");
    link.setCwd(b.path(""));
    link.has_side_effects = true;

    // per-source objects, cached by zig so rebuilds only touch changed files
    for (srcs.items) |src| {
        const cc = b.addSystemCommand(&.{ "emcc", "-c", "-Os" });
        const obj = objectName(b, src);
        cc.setName(b.fmt("emcc {s}", .{obj}));
        cc.addArgs(&cflags);
        if (src == .dependency) cc.addArg("-w");
        cc.addArgs(&.{ "-DNDEBUG", "-DPLATFORM_WEB", "-DGRAPHICS_API_OPENGL_ES2" });
        for (pk_defines) |d| cc.addArg(b.fmt("-D{s}={s}", .{ d[0], d[1] }));
        cc.addArg("-DASSETS_PAK=\"" ++ assets_pak ++ "\"");
        cc.addPrefixedDirectoryArg("-I", raylib_dep.path("src"));
        cc.addPrefixedDirectoryArg("-I", pocketpy_dep.path("include"));
        cc.addPrefixedDirectoryArg("-I", miniz_dep.path(""));
        cc.addFileArg(src);
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
        "-lidbfs.js",
    });
    link.addArg("--pre-js");
    link.addFileArg(b.path("src/web_save.js"));
    link.addArgs(&.{ "--preload-file", "zig-out/web/" ++ assets_pak ++ "@/" ++ assets_pak });
    link.addArgs(&.{ "-o", "zig-out/web/" ++ name ++ ".html" });
    link.step.dependOn(&pak.step);
    b.step("web", "Build the web target").dependOn(&link.step);
}

// zip -FS syncs the archive, entries for deleted files are dropped
fn pakStep(b: *std.Build, dir: []const u8) *std.Build.Step.Run {
    const mkdir = b.addSystemCommand(&.{ "mkdir", "-p", dir });
    mkdir.setCwd(b.path(""));
    const pak = b.addSystemCommand(&.{ "zip", "-qrFS" });
    pak.addArg(b.fmt("{s}/{s}", .{ dir, assets_pak }));
    pak.addArgs(&.{ "assets", "game" });
    pak.setCwd(b.path(""));
    pak.has_side_effects = true;
    pak.step.dependOn(&mkdir.step);
    return pak;
}

fn findCSources(b: *std.Build, root: []const u8, sub: []const u8) ![]const []const u8 {
    var list: std.array_list.Managed([]const u8) = .init(b.allocator);
    try walkCSources(b, root, sub, &list);
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
            .file => if (std.mem.endsWith(u8, entry.name, ".c")) try list.append(child),
            else => {},
        }
    }
}

fn objectName(b: *std.Build, src: std.Build.LazyPath) []const u8 {
    const rel = switch (src) {
        .src_path => |p| p.sub_path,
        .dependency => |p| p.sub_path,
        else => unreachable,
    };
    const obj = b.dupe(rel[0 .. rel.len - 2]);
    std.mem.replaceScalar(u8, obj, '/', '_');
    return obj;
}

fn raylibTag() []const u8 {
    const zon = @embedFile("build.zig.zon");
    const marker = "raysan5/raylib/archive/refs/tags/";
    const start = std.mem.indexOf(u8, zon, marker).? + marker.len;
    return zon[start..std.mem.indexOfPos(u8, zon, start, ".tar.gz").?];
}
