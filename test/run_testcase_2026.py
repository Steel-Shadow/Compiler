#!/usr/bin/env python3
import argparse
import os
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_REPO_URL = "git@github.com:compile-technology-buaa/testcase-2026.git"
DEFAULT_REPO = ROOT / "test" / "vendor" / "testcase-2026"
DEFAULT_WORK = ROOT / "test" / "work" / "testcase-2026"
DEFAULT_COMPILER = ROOT / "build" / "src" / "Compiler"


def default_jobs():
    return min(max((os.cpu_count() or 2) // 2, 1), 8)


def run(cmd, cwd=None, env=None, timeout=None, stdin_path=None):
    stdin = None
    if stdin_path is not None:
        stdin = open(stdin_path, "rb")
    try:
        return subprocess.run(
            cmd,
            cwd=cwd,
            env=env,
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
        if stdin is not None:
            stdin.close()


def text(path):
    if not path.exists():
        return ""
    return path.read_text(encoding="utf-8", errors="replace").replace("\r\n", "\n")


def same_lines(lhs, rhs):
    return lhs.splitlines() == rhs.splitlines()


def same_runtime_output(lhs, rhs):
    return lhs.rstrip("\n") == rhs.rstrip("\n")


def ensure_repo(repo, repo_url, prepare):
    if repo.exists():
        return
    if not prepare:
        raise SystemExit(
            f"Missing testcase repo: {repo}\n"
            f"Run with --prepare to clone {repo_url}, or pass --repo <path>."
        )
    repo.parent.mkdir(parents=True, exist_ok=True)
    result = run(["git", "clone", "--depth", "1", repo_url, str(repo)])
    if result.returncode != 0:
        sys.stderr.write(result.stderr.decode(errors="replace"))
        raise SystemExit(result.returncode)


def ensure_generated(repo, prepare):
    generated = repo / "generated"
    if generated.exists() and list(generated.rglob("testfile.txt")):
        return generated
    if not prepare:
        raise SystemExit(
            f"Missing generated cases under {generated}.\n"
            "Run with --prepare so the harness can execute `make generate`."
        )
    env = os.environ.copy()
    env["ASAN_OPTIONS"] = "detect_leaks=0:" + env.get("ASAN_OPTIONS", "")
    result = run(["make", "generate"], cwd=repo, env=env)
    if result.returncode != 0:
        sys.stderr.write(result.stdout.decode(errors="replace"))
        sys.stderr.write(result.stderr.decode(errors="replace"))
        raise SystemExit(result.returncode)
    return generated


def case_name(case_dir, generated):
    return case_dir.relative_to(generated).as_posix()


def discover_cases(generated, suite):
    all_sources = sorted(path.parent for path in generated.rglob("testfile.txt"))
    correct = []
    errors = []
    for case_dir in all_sources:
        rel = case_name(case_dir, generated)
        if (case_dir / "error.txt").exists():
            errors.append(case_dir)
        elif (case_dir / "ans.txt").exists():
            correct.append(case_dir)

    if suite == "all":
        return correct, errors
    if suite == "2026":
        return [c for c in correct if case_name(c, generated).startswith("2026/")], [
            e for e in errors if case_name(e, generated).startswith("err_2026/")
        ]
    if suite == "regression":
        return [c for c in correct if case_name(c, generated).startswith("regression/")], [
            e for e in errors if case_name(e, generated).startswith("err_regression/")
        ]
    if suite == "correct":
        return correct, []
    if suite == "errors":
        return [], errors
    raise ValueError(f"unknown suite {suite}")


def filter_cases(cases, generated, pattern, limit):
    if pattern:
        cases = [case for case in cases if pattern in case_name(case, generated)]
    if limit is not None:
        cases = cases[:limit]
    return cases


def compiler_outputs(work_dir):
    return {
        "lexer": work_dir / "lexer.txt",
        "error": work_dir / "error.txt",
        "ir": work_dir / "ir.txt",
        "mips": work_dir / "mips.txt",
    }


def run_compiler(compiler, source, out_dir, timeout):
    out_dir.mkdir(parents=True, exist_ok=True)
    outputs = compiler_outputs(out_dir)
    for output in outputs.values():
        try:
            output.unlink()
        except FileNotFoundError:
            pass
    return run(
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


def run_error_case(compiler, case_dir, generated, work_root, timeout):
    name = case_name(case_dir, generated)
    out_dir = work_root / name
    result = run_compiler(compiler, case_dir / "testfile.txt", out_dir, timeout)
    actual = text(out_dir / "error.txt")
    expected = text(case_dir / "error.txt")
    ok = result.returncode == 0 and same_lines(actual, expected)
    detail = ""
    if result.returncode != 0:
        detail = f"compiler exited {result.returncode}: {result.stderr.decode(errors='replace')}"
    elif not same_lines(actual, expected):
        detail = f"error.txt mismatch\nexpected:\n{expected}\nactual:\n{actual}"
    return ok, detail


def run_mars(mars_jar, mips_path, input_path, timeout):
    cmd = ["java", "-jar", str(mars_jar), "nc", str(mips_path)]
    return run(cmd, stdin_path=input_path, timeout=timeout)


def mars_input_path(case_dir, out_dir):
    input_path = case_dir / "in.txt"
    source = text(case_dir / "testfile.txt")
    uses_int_input = re.search(r"\b(get_int|getint)\s*\(", source) is not None
    uses_other_input = re.search(r"\b(get_char|getchar|get_string)\s*\(", source) is not None
    if not uses_int_input or uses_other_input:
        return input_path

    tokens = text(input_path).split()
    if not tokens or any(re.fullmatch(r"[+-]?\d+", token) is None for token in tokens):
        return input_path

    normalized_path = out_dir / "mars_in.txt"
    normalized_path.write_text("\n".join(tokens) + "\n", encoding="utf-8")
    return normalized_path


def run_correct_case(compiler, case_dir, generated, work_root, timeout, mars_jar, mars_timeout):
    name = case_name(case_dir, generated)
    out_dir = work_root / name
    result = run_compiler(compiler, case_dir / "testfile.txt", out_dir, timeout)
    actual_error = text(out_dir / "error.txt")
    if result.returncode != 0:
        return False, f"compiler exited {result.returncode}: {result.stderr.decode(errors='replace')}"
    if actual_error.splitlines():
        return False, f"expected no errors, got:\n{actual_error}"

    if mars_jar is None:
        return True, ""

    mars = run_mars(mars_jar, out_dir / "mips.txt", mars_input_path(case_dir, out_dir), mars_timeout)
    if mars.returncode != 0:
        return False, (
            f"Mars exited {mars.returncode}\n"
            f"stdout:\n{mars.stdout.decode(errors='replace')}\n"
            f"stderr:\n{mars.stderr.decode(errors='replace')}"
        )
    actual = mars.stdout.decode(errors="replace").replace("\r\n", "\n")
    expected = text(case_dir / "ans.txt")
    if not same_runtime_output(actual, expected):
        return False, f"runtime output mismatch\nexpected:\n{expected}\nactual:\n{actual}"
    return True, ""


def run_case(order, kind, index, total, case_dir, args, generated):
    name = case_name(case_dir, generated)
    if kind == "correct":
        ok, detail = run_correct_case(
            args.compiler,
            case_dir,
            generated,
            args.work_dir,
            args.timeout,
            args.mars_jar,
            args.mars_timeout,
        )
    else:
        ok, detail = run_error_case(args.compiler, case_dir, generated, args.work_dir, args.timeout)
    return order, kind, index, total, name, ok, detail


def run_selected_cases(correct, errors, args, generated):
    jobs = []
    order = 0
    for index, case_dir in enumerate(correct, 1):
        jobs.append((order, "correct", index, len(correct), case_dir))
        order += 1
    for index, case_dir in enumerate(errors, 1):
        jobs.append((order, "error", index, len(errors), case_dir))
        order += 1

    failures = []
    if not jobs:
        return failures

    workers = min(args.jobs, len(jobs))
    print(f"Jobs: {workers}")

    if workers == 1:
        for job in jobs:
            result = run_case(*job, args, generated)
            order, kind, index, total, name, ok, detail = result
            print(f"[{kind} {index}/{total}] {'PASS' if ok else 'FAIL'} {name}", flush=True)
            if not ok:
                failures.append((order, name, detail))
        return failures

    with ThreadPoolExecutor(max_workers=workers) as executor:
        futures = {executor.submit(run_case, *job, args, generated): job for job in jobs}
        for future in as_completed(futures):
            job = futures[future]
            try:
                order, kind, index, total, name, ok, detail = future.result()
            except Exception as exc:
                order, kind, index, total, case_dir = job
                name = case_name(case_dir, generated)
                ok = False
                detail = f"test runner failed: {exc!r}"
            print(f"[{kind} {index}/{total}] {'PASS' if ok else 'FAIL'} {name}", flush=True)
            if not ok:
                failures.append((order, name, detail))
    return failures


def main():
    parser = argparse.ArgumentParser(description="Run compiler tests from testcase-2026.")
    parser.add_argument("--repo", type=Path, default=DEFAULT_REPO)
    parser.add_argument("--repo-url", default=DEFAULT_REPO_URL)
    parser.add_argument("--compiler", type=Path, default=DEFAULT_COMPILER)
    parser.add_argument("--work-dir", type=Path, default=DEFAULT_WORK)
    parser.add_argument("--prepare", action="store_true", help="clone and/or generate testcase data")
    parser.add_argument(
        "--suite",
        choices=["all", "2026", "regression", "correct", "errors"],
        default="2026",
    )
    parser.add_argument("--filter", help="only run cases whose generated path contains this text")
    parser.add_argument("--limit", type=int, help="limit number of correct and error cases separately")
    parser.add_argument("--timeout", type=float, default=10.0)
    parser.add_argument("--mars-timeout", type=float, default=15.0)
    parser.add_argument("--mars-jar", type=Path)
    parser.add_argument(
        "-j",
        "--jobs",
        type=int,
        default=default_jobs(),
        help="number of test cases to run in parallel",
    )
    args = parser.parse_args()

    if args.jobs < 1:
        raise SystemExit("--jobs must be at least 1")
    if not args.compiler.exists():
        raise SystemExit(f"Compiler not found: {args.compiler}. Build it first with `cmake --build build`.")
    if args.mars_jar is not None and not args.mars_jar.exists():
        raise SystemExit(f"Mars jar not found: {args.mars_jar}")

    ensure_repo(args.repo, args.repo_url, args.prepare)
    generated = ensure_generated(args.repo, args.prepare)
    correct, errors = discover_cases(generated, args.suite)
    correct = filter_cases(correct, generated, args.filter, args.limit)
    errors = filter_cases(errors, generated, args.filter, args.limit)

    args.work_dir.mkdir(parents=True, exist_ok=True)

    print(f"Compiler: {args.compiler}")
    print(f"Generated cases: {generated}")
    print(f"Suite: {args.suite}")
    print(f"Correct cases: {len(correct)}")
    print(f"Error cases: {len(errors)}")
    if args.mars_jar is None and correct:
        print("Mars: not provided; correct cases will check compilation/error.txt only")
    elif args.mars_jar is not None:
        print(f"Mars: {args.mars_jar}")

    failures = run_selected_cases(correct, errors, args, generated)

    if failures:
        failures.sort(key=lambda failure: failure[0])
        print("\nFailures:")
        for _, name, detail in failures[:20]:
            print(f"\n--- {name} ---")
            print(detail)
        if len(failures) > 20:
            print(f"\n... {len(failures) - 20} more failure(s)")
        return 1

    print("\nAll selected cases passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
