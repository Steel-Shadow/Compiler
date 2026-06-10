#!/usr/bin/env python3
import argparse
import re
import subprocess
import sys
import time
from decimal import Decimal
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT_DIR = Path(__file__).resolve().parent
DEFAULT_SOURCE = SCRIPT_DIR / "testfile.txt"
DEFAULT_WORK = SCRIPT_DIR
DEFAULT_INPUT = SCRIPT_DIR / "in.txt"
DEFAULT_COMPILER = ROOT / "build" / "src" / "Compiler"
MARS_JAR_PATTERNS = ("Mars*.jar", "mars*.jar", "*Mars*.jar", "*mars*.jar")


def find_default_mars_jar():
    candidates = []
    for directory in (SCRIPT_DIR, SCRIPT_DIR / "vendor", ROOT / "test" / "vendor"):
        for pattern in MARS_JAR_PATTERNS:
            candidates.extend(directory.glob(pattern))
    jars = sorted({path.resolve() for path in candidates if path.is_file()})
    return jars[0] if jars else None


def run(cmd, timeout=None, stdin_path=None, cwd=None):
    stdin = subprocess.DEVNULL
    if stdin_path is not None:
        stdin = open(stdin_path, "rb")
    try:
        return subprocess.run(
            cmd,
            cwd=cwd,
            stdin=stdin,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=timeout,
            check=False,
        )
    except subprocess.TimeoutExpired as exc:
        stdout = exc.stdout or b""
        stderr = exc.stderr or b""
        if isinstance(stdout, str):
            stdout = stdout.encode()
        if isinstance(stderr, str):
            stderr = stderr.encode()
        stderr += f"\ntimeout after {timeout} seconds".encode()
        return subprocess.CompletedProcess(cmd, 124, stdout=stdout, stderr=stderr)
    finally:
        if stdin_path is not None:
            stdin.close()


def text(path):
    if not path.exists():
        return ""
    return path.read_text(encoding="utf-8", errors="replace").replace("\r\n", "\n")


def same_runtime_output(lhs, rhs):
    return lhs.rstrip("\n") == rhs.rstrip("\n")


def parse_final_cycle(stats_path):
    match = None
    for _ in range(10):
        stats = text(stats_path)
        match = re.search(r"^Final Cycle:\s*([0-9]+(?:\.[0-9]+)?)\s*$", stats, re.MULTILINE)
        if match:
            break
        time.sleep(0.05)
    if not match:
        return None
    return Decimal(match.group(1))


def format_cycle(value):
    if value == value.to_integral_value():
        return str(value.to_integral_value())
    return format(value.normalize(), "f")


def strip_instruction_count_output(output):
    lines = output.splitlines()
    while lines and lines[-1] == "":
        lines = lines[:-1]
    if lines and re.fullmatch(r"\d+", lines[-1]):
        lines = lines[:-1]
        if lines and lines[-1] == "":
            lines = lines[:-1]
    if not lines:
        return ""
    return "\n".join(lines) + "\n"


def compiler_outputs(work_dir):
    return {
        "lexer": work_dir / "lexer.txt",
        "error": work_dir / "error.txt",
        "ir": work_dir / "ir.txt",
        "mips": work_dir / "mips.txt",
    }


def run_compiler(compiler, source, work_dir, timeout):
    work_dir.mkdir(parents=True, exist_ok=True)
    outputs = compiler_outputs(work_dir)
    for output in outputs.values():
        try:
            output.unlink()
        except FileNotFoundError:
            pass
        output.touch()
    result = run(
        [
            str(compiler),
            str(source),
            str(outputs["lexer"]),
            str(outputs["error"]),
            str(outputs["ir"]),
            str(outputs["mips"]),
        ],
        timeout=timeout,
    )
    return result, outputs


def run_mars(mars_jar, mips_path, input_path, timeout, collect_cycles, work_dir):
    stats_path = work_dir / "InstructionStatistics.txt"
    try:
        stats_path.unlink()
    except FileNotFoundError:
        pass

    cmd = ["java", "-jar", str(mars_jar.resolve()), "nc"]
    if collect_cycles:
        cmd.append("ic")
    cmd.append(str(mips_path.resolve()))
    result = run(cmd, stdin_path=input_path, timeout=timeout, cwd=work_dir)
    return result, stats_path


def print_file_hint(name, path):
    print(f"{name}: {path}")


def main():
    parser = argparse.ArgumentParser(description="Compile and run one hand-written SysY test.")
    parser.add_argument("source", nargs="?", type=Path, default=DEFAULT_SOURCE)
    parser.add_argument("--compiler", type=Path, default=DEFAULT_COMPILER)
    parser.add_argument("--work-dir", type=Path, default=DEFAULT_WORK)
    parser.add_argument("-i", "--input", type=Path, help="stdin file for Mars",default=DEFAULT_INPUT)
    parser.add_argument("-e", "--expected", type=Path, help="expected runtime output file")
    parser.add_argument("--mars-jar", type=Path)
    parser.add_argument("--timeout", type=float, default=10.0)
    parser.add_argument("--mars-timeout", type=float, default=15.0)
    parser.add_argument("--no-run", action="store_true", help="only compile, do not run Mars")
    parser.add_argument("--no-cycles", dest="collect_cycles", action="store_false")
    parser.add_argument("--show-ir", action="store_true")
    parser.add_argument("--show-mips", action="store_true")
    parser.set_defaults(collect_cycles=True)
    args = parser.parse_args()

    source = args.source.resolve()
    compiler = args.compiler.resolve()
    work_dir = args.work_dir.resolve()
    input_path = args.input.resolve() if args.input is not None else None
    expected_path = args.expected.resolve() if args.expected is not None else None

    if not source.exists():
        raise SystemExit(f"source not found: {source}")
    if not compiler.exists():
        raise SystemExit(f"compiler not found: {compiler}. Build it first with `cmake --build build`.")
    if input_path is not None and not input_path.exists():
        raise SystemExit(f"input file not found: {input_path}")
    if expected_path is not None and not expected_path.exists():
        raise SystemExit(f"expected output file not found: {expected_path}")

    mars_jar = args.mars_jar.resolve() if args.mars_jar is not None else find_default_mars_jar()
    if args.mars_jar is not None and not mars_jar.exists():
        raise SystemExit(f"Mars jar not found: {mars_jar}")

    print(f"Source: {source}")
    print(f"Compiler: {compiler}")
    print(f"Work dir: {work_dir}")

    compile_result, outputs = run_compiler(compiler, source, work_dir, args.timeout)
    for name, path in outputs.items():
        print_file_hint(name, path)

    if compile_result.returncode != 0:
        sys.stderr.write(compile_result.stdout.decode(errors="replace"))
        sys.stderr.write(compile_result.stderr.decode(errors="replace"))
        return compile_result.returncode

    error_text = text(outputs["error"])
    if error_text.splitlines():
        print("\nCompile finished with front-end errors:")
        print(error_text, end="" if error_text.endswith("\n") else "\n")
        return 1

    print("\nCompile OK.")
    if args.show_ir:
        print("\n--- ir.txt ---")
        print(text(outputs["ir"]), end="")
    if args.show_mips:
        print("\n--- mips.txt ---")
        print(text(outputs["mips"]), end="")

    if args.no_run:
        return 0
    if mars_jar is None:
        print("\nMars jar not found; skipped runtime execution.")
        return 0

    print(f"\nMars: {mars_jar}")
    if input_path is not None:
        print(f"Input: {input_path}")

    mars_result, stats_path = run_mars(
        mars_jar,
        outputs["mips"],
        input_path,
        args.mars_timeout,
        args.collect_cycles,
        work_dir,
    )
    stdout = mars_result.stdout.decode(errors="replace").replace("\r\n", "\n")
    stderr = mars_result.stderr.decode(errors="replace").replace("\r\n", "\n")
    if args.collect_cycles:
        stdout = strip_instruction_count_output(stdout)

    if mars_result.returncode != 0:
        print("\nMars failed.")
        if stdout:
            print("\n--- stdout ---")
            print(stdout, end="" if stdout.endswith("\n") else "\n")
        if stderr:
            print("\n--- stderr ---")
            print(stderr, end="" if stderr.endswith("\n") else "\n")
        return mars_result.returncode

    print("\n--- output ---")
    print(stdout, end="" if stdout.endswith("\n") or not stdout else "\n")

    if args.collect_cycles:
        final_cycle = parse_final_cycle(stats_path)
        if final_cycle is None:
            print(f"\nFinal Cycle: missing in {stats_path}")
        else:
            print(f"\nFinal Cycle: {format_cycle(final_cycle)}")

    if expected_path is not None:
        expected = text(expected_path)
        if not same_runtime_output(stdout, expected):
            print("\nOutput mismatch.")
            print("\n--- expected ---")
            print(expected, end="" if expected.endswith("\n") else "\n")
            return 1
        print("\nOutput matches expected.")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
