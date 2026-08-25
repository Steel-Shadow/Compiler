#!/usr/bin/env python3
"""Local build and submission helper for the SysY compiler."""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent
DEFAULT_BUILD_DIR = ROOT / "build"
DEFAULT_DIST_DIR = ROOT / "dist"
DEFAULT_PACKAGE = DEFAULT_DIST_DIR / "Compiler-submit.zip"

SUBMISSION_FILES = (
    "CMakeLists.txt",
    "config.json",
)

SOURCE_SUFFIXES = {
    ".c",
    ".cc",
    ".cpp",
    ".cxx",
    ".h",
    ".hh",
    ".hpp",
    ".hxx",
}


def run(cmd: list[str], cwd: Path = ROOT) -> None:
    print("+ " + " ".join(cmd))
    subprocess.run(cmd, cwd=cwd, check=True)


def cmake_configure(build_dir: Path, build_type: str) -> None:
    run(
        [
            "cmake",
            "-S",
            str(ROOT),
            "-B",
            str(build_dir),
            f"-DCMAKE_BUILD_TYPE={build_type}",
        ]
    )


def cmake_build(build_dir: Path) -> None:
    run(
        [
            "cmake",
            "--build",
            str(build_dir),
            "--target",
            "Compiler",
            "--parallel",
            str(os.cpu_count() or 2),
        ]
    )


def command_build(args: argparse.Namespace) -> None:
    cmake_configure(args.build_dir, args.build_type)
    cmake_build(args.build_dir)


def iter_submission_paths() -> list[Path]:
    paths: list[Path] = [ROOT / name for name in SUBMISSION_FILES]
    for path in sorted((ROOT / "src").rglob("*")):
        if path.is_file() and (
            path.name == "CMakeLists.txt" or path.suffix in SOURCE_SUFFIXES
        ):
            paths.append(path)
    return paths


def command_package(args: argparse.Namespace) -> None:
    if args.output.exists():
        args.output.unlink()
    args.output.parent.mkdir(parents=True, exist_ok=True)

    missing = [
        str(path.relative_to(ROOT))
        for path in iter_submission_paths()
        if not path.exists()
    ]
    if missing:
        raise SystemExit("missing required submission files: " + ", ".join(missing))

    with zipfile.ZipFile(args.output, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for path in iter_submission_paths():
            archive.write(path, path.relative_to(ROOT).as_posix())

    print(f"wrote {args.output.relative_to(ROOT)}")


def command_verify_package(args: argparse.Namespace) -> None:
    command_package(args)
    with tempfile.TemporaryDirectory(prefix="compiler-submit-") as temp:
        temp_dir = Path(temp)
        with zipfile.ZipFile(args.output) as archive:
            archive.extractall(temp_dir)

        build_dir = temp_dir / "build"
        run(
            [
                "cmake",
                "-S",
                str(temp_dir),
                "-B",
                str(build_dir),
                "-DCMAKE_BUILD_TYPE=Release",
            ],
            cwd=temp_dir,
        )
        run(
            [
                "cmake",
                "--build",
                str(build_dir),
                "--target",
                "Compiler",
                "--parallel",
                str(os.cpu_count() or 2),
            ],
            cwd=temp_dir,
        )
    print("package verification passed")


def command_clean(args: argparse.Namespace) -> None:
    for path in (args.build_dir, args.dist_dir):
        if path.exists():
            print(f"remove {path.relative_to(ROOT)}")
            shutil.rmtree(path)


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Build/package helper for the Compiler project."
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    build = subparsers.add_parser(
        "build", help="configure and build the Compiler executable"
    )
    build.add_argument("--build-dir", type=Path, default=DEFAULT_BUILD_DIR)
    build.add_argument("--build-type", default="Release")
    build.set_defaults(func=command_build)

    package = subparsers.add_parser("package", help="create a submission zip")
    package.add_argument("-o", "--output", type=Path, default=DEFAULT_PACKAGE)
    package.set_defaults(func=command_package)

    verify = subparsers.add_parser(
        "verify-package", help="create and rebuild the submission zip in a temp dir"
    )
    verify.add_argument("-o", "--output", type=Path, default=DEFAULT_PACKAGE)
    verify.set_defaults(func=command_verify_package)

    clean = subparsers.add_parser("clean", help="remove local build/package output")
    clean.add_argument("--build-dir", type=Path, default=DEFAULT_BUILD_DIR)
    clean.add_argument("--dist-dir", type=Path, default=DEFAULT_DIST_DIR)
    clean.set_defaults(func=command_clean)

    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    args.func(args)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
