# Compiler Test Module

This directory contains the local test harness for the BUAA 2026 compiler
testcase repository.

The testcase repository itself is not committed here. The harness can clone it
into `test/vendor/testcase-2026` and use its generated cases:

```bash
python3 test/run_testcase_2026.py --prepare --suite 2026
```

Cases are run in parallel by default, up to 8 workers. Override that with
`-j` when you want a different level of concurrency:

```bash
python3 test/run_testcase_2026.py --suite all -j 4
python3 test/run_testcase_2026.py --suite all -j 1
```

Generated test data and compiler outputs are written under ignored directories:

- `test/vendor/`
- `test/work/`

Useful commands:

```bash
# Build compiler first
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Clone/generate testcase-2026, then run 2026 correct + error frontend checks
python3 test/run_testcase_2026.py --prepare --suite 2026

# Run only error cases
python3 test/run_testcase_2026.py --suite errors

# Run a small subset while developing
python3 test/run_testcase_2026.py --suite all --limit 20
```

Correct cases currently check that the front end succeeds and `error.txt` is
empty. Error cases compare `error.txt`. Runtime comparison is disabled until the
new middle end and backend are rebuilt.

`make generate` in `testcase-2026` is executed with
`ASAN_OPTIONS=detect_leaks=0` so it works in ptrace/sandboxed environments where
LeakSanitizer cannot run.
