const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const json_mod = b.addModule("json_hotpath", .{
        .root_source_file = b.path("src/json_hotpath.c"),
        .target = target,
        .optimize = optimize,
        .link_libc = true,
    });

    const frame_mod = b.addModule("brpc_frame", .{
        .root_source_file = b.path("src/brpc_frame.c"),
        .target = target,
        .optimize = optimize,
    });

    const stream_mod = b.addModule("brpc_stream", .{
        .root_source_file = b.path("src/brpc_stream.c"),
        .target = target,
        .optimize = optimize,
    });

    const channel_mod = b.addModule("brpc_channel", .{
        .root_source_file = b.path("src/brpc_channel.c"),
        .target = target,
        .optimize = optimize,
        .link_libc = true,
    });

    const prof_mod = b.addModule("brpc_prof", .{
        .root_source_file = b.path("src/brpc_prof.c"),
        .target = target,
        .optimize = optimize,
    });

    const main_exe = b.addExecutable(.{
        .name = "brpc_demo",
        .root_module = b.createModule(.{
            .root_source_file = b.path("examples/main.c"),
            .target = target,
            .optimize = optimize,
            .link_libc = true,
            .imports = &.{
                .{ .name = "json_hotpath", .module = json_mod },
                .{ .name = "brpc_frame", .module = frame_mod },
                .{ .name = "brpc_stream", .module = stream_mod },
                .{ .name = "brpc_channel", .module = channel_mod },
                .{ .name = "brpc_prof", .module = prof_mod },
            },
        }),
    });
    b.installArtifact(main_exe);

    const run_demo = b.addRunArtifact(main_exe);
    run_demo.step.dependOn(b.getInstallStep());
    const run_step = b.step("run", "Run demo");
    run_step.dependOn(&run_demo.step);

    const test_exe = b.addExecutable(.{
        .name = "test_brpc",
        .root_module = b.createModule(.{
            .root_source_file = b.path("tests/test_brpc.c"),
            .target = target,
            .optimize = optimize,
            .link_libc = true,
            .imports = &.{
                .{ .name = "json_hotpath", .module = json_mod },
                .{ .name = "brpc_frame", .module = frame_mod },
                .{ .name = "brpc_stream", .module = stream_mod },
                .{ .name = "brpc_channel", .module = channel_mod },
                .{ .name = "brpc_prof", .module = prof_mod },
            },
        }),
    });

    const run_test = b.addRunArtifact(test_exe);
    run_test.step.dependOn(b.getInstallStep());
    const test_step = b.step("test", "Run tests");
    test_step.dependOn(&run_test.step);
}
