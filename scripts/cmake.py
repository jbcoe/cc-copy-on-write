#!/usr/bin/env python3
import argparse
import subprocess


def main():
    parser = argparse.ArgumentParser(description="CMake helper script")
    parser.add_argument(
        "mode",
        nargs="?",
        default="test",
        choices=["build", "test", "benchmark", "b", "t", "bm"],
        help="Target mode: build (b), test (t), benchmark (bm) (default: test)",
    )
    group = parser.add_mutually_exclusive_group()
    group.add_argument(
        "--debug",
        action="store_const",
        dest="build_type",
        const="Debug",
        help="Use Debug build type",
    )
    group.add_argument(
        "--release",
        action="store_const",
        dest="build_type",
        const="Release",
        help="Use Release build type (default)",
    )
    parser.add_argument(
        "--manual-vtable",
        action="store_true",
        help="Set XYZ_PROTOCOL_GENERATE_MANUAL_VTABLE=ON",
    )
    parser.add_argument("--asan", action="store_true", help="Enable Address Sanitizer")
    parser.add_argument(
        "--ubsan", action="store_true", help="Enable Undefined Behaviour Sanitizer"
    )
    parser.add_argument("--tsan", action="store_true", help="Enable Thread Sanitizer")
    parser.add_argument("--msan", action="store_true", help="Enable Memory Sanitizer")
    parser.add_argument("-B", "--build-dir", help="Build directory")
    parser.add_argument(
        "--clean", action="store_true", help="Fresh configuration and clean-first build"
    )
    parser.add_argument(
        "-v", "--verbose", action="store_true", help="Enable verbose logging"
    )

    args, extra = parser.parse_known_args()

    build_type = args.build_type if args.build_type else "Release"

    mode_map = {
        "b": "build",
        "t": "test",
        "bm": "benchmark",
        "build": "build",
        "test": "test",
        "benchmark": "benchmark",
    }
    mode = mode_map[args.mode]

    build_dir = args.build_dir if args.build_dir else f"build/{build_type}"

    def log(msg):
        if args.verbose:
            print(msg)

    # Configure step
    configure_args = [
        "cmake",
        "-S", ".",
        "-B", build_dir,
        f"-DCMAKE_BUILD_TYPE={build_type}",
        f"-DXYZ_PROTOCOL_GENERATE_MANUAL_VTABLE={'ON' if args.manual_vtable else 'OFF'}",
        f"-DENABLE_ASAN={'ON' if args.asan else 'OFF'}",
        f"-DENABLE_UBSAN={'ON' if args.ubsan else 'OFF'}",
        f"-DENABLE_TSAN={'ON' if args.tsan else 'OFF'}",
        f"-DENABLE_MSAN={'ON' if args.msan else 'OFF'}",
    ]
    if args.clean:
        configure_args.append("--fresh")

    configure_args.extend(extra)

    log(f"Running: {' '.join(configure_args)}")
    subprocess.check_call(configure_args)

    # Build step
    build_args = ["cmake", "--build", build_dir, "--config", build_type]
    if args.clean:
        build_args.append("--clean-first")

    log(f"Running: {' '.join(build_args)}")
    subprocess.check_call(build_args)

    # Test step
    if mode == "test":
        test_args = ["ctest", "--output-on-failure", "--test-dir", build_dir, "-C", build_type]

        log(f"Running: {' '.join(test_args)}")
        subprocess.check_call(test_args)

    # Benchmark step
    if mode == "benchmark":
        benchmark_cmd = ["cmake", "--build", build_dir, "--config", build_type, "--target", "run_benchmark"]

        log(f"Running: {' '.join(benchmark_cmd)}")
        subprocess.check_call(benchmark_cmd)


if __name__ == "__main__":
    main()
