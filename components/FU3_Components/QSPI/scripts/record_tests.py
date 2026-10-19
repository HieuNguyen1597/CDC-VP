#!/usr/bin/env python3
"""Record actual QSPI CTest runs; never infer success from existing artifacts."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
from datetime import datetime, timezone


def command(args):
    return subprocess.run(args, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("normal_build", type=Path)
    parser.add_argument("ubsan_build", type=Path)
    parser.add_argument("--output-dir", type=Path,
                        help="Report directory (default: NORMAL_BUILD/test-reports)")
    options = parser.parse_args()
    component = Path(__file__).resolve().parents[1]
    reports = (options.output_dir or options.normal_build / "test-reports").resolve()
    reports.mkdir(parents=True, exist_ok=True)
    expected = {"fu3_qspi_" + s for s in (
        "registers protocol triggers indirect fifo polling timeout dma xip phases "
        "errors reset clock concurrency random components threshold_words spec_timing bus "
        "flash_commands flash_program flash_erase flash_protocol flash_reset flash_image flash_sioo flash_integrity "
        "p0_triggers p0_dlr p0_fifo p0_irq_w1c p0_active_reset p0_bus_errors "
        "p1_polling p1_dma p1_domains").split()}
    expected.update("fu3_qspi_p1_parameters_" + depth for depth in ("1", "3", "4", "7", "16", "32", "4x7", "7x4"))
    summary = {"captured_at_utc": datetime.now(timezone.utc).isoformat(), "runs": []}
    passed = True
    for kind, directory in zip(("debug", "ubsan"), (options.normal_build, options.ubsan_build)):
        build = str(Path(directory).resolve())
        cache = (Path(build) / "CMakeCache.txt").read_text()
        flags = re.search(r"^CMAKE_CXX_FLAGS:STRING=(.*)$", cache, re.M).group(1)
        if kind == "ubsan" and not all(flag in flags for flag in (
                "-fsanitize=undefined", "-fno-sanitize-recover=undefined", "-Werror")):
            raise SystemExit("UBSan build is missing required sanitizer/warning flags")
        listing = command(["ctest", "--test-dir", build, "--show-only=json-v1"])
        if listing.returncode:
            raise SystemExit(listing.stdout)
        registered = {t["name"] for t in json.loads(listing.stdout)["tests"]}
        if registered != expected:
            raise SystemExit(f"Unexpected test set in {build}: {registered ^ expected}")
        args = ["ctest", "--test-dir", build, "-V", "--output-on-failure"]
        result = command(args)
        name = "test-results.txt" if kind == "debug" else "ubsan-results.txt"
        (reports / name).write_text("Command: " + " ".join(args) + "\n\n" + result.stdout, encoding="utf-8")
        records = {}
        for scenario, count, time in re.findall(r"PASS (\w+) assertions=(\d+) time=([^\r\n]+)", result.stdout):
            records["fu3_qspi_" + scenario] = {"status": "passed", "assertions": int(count), "simulation_time": time}
        ok = result.returncode == 0 and set(records) == expected
        passed = passed and ok
        summary["runs"].append({"configuration": kind, "build_directory": build, "cxx_flags": flags,
            "exit_code": result.returncode, "passed": ok, "test_count": len(expected),
            "assertions": sum(r["assertions"] for r in records.values()),
            "tests": records, "transcript": name})
        print(f"{kind}: {'PASS' if ok else 'FAIL'}, {len(records)}/{len(expected)} tests")
    repo = component.parents[2]
    roots = [component / "include", component / "src", component / "tests", component / "scripts",
             component.parent / "bus/include", component.parent / "bus/src"]
    paths = [component / "CMakeLists.txt", component.parent / "bus/CMakeLists.txt"]
    for root in roots:
        paths.extend(p for p in root.rglob("*") if p.suffix in {".h", ".cpp", ".py"})
    summary["source_sha256"] = {p.relative_to(repo).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                                 for p in sorted(paths)}
    summary["toolchain"] = {name: command([name, "--version"]).stdout.splitlines()[0]
                            for name in ("cmake", "ctest", "c++")}
    summary["systemc"] = "2.3.4 (see runtime banner in transcripts)"
    summary["passed"] = passed
    (reports / "test-summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(f"Reports: {reports}")
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
