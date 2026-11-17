#!/usr/bin/env python3
"""
HQRN CI - Negative & Inconsistency Test Suite for Diagnostic Metadata Verifier
Ensures that any corrupted, missing, or mismatched metadata in
`metadata.json`, `diagnostic_metadata.txt`, or `step_summary.md` causes
`verify_diagnostic_metadata.py` to fail cleanly with an explicit, human-readable
diagnostic message and non-zero exit code.
"""

import os
import sys
import json
import shutil
import tempfile
import subprocess

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
VERIFIER_SCRIPT = os.path.join(SCRIPT_DIR, 'verify_diagnostic_metadata.py')

DEFAULT_ENV = {
    'EXP_RUN_ATTEMPT': '1',
    'EXP_TRIGGER_EVENT': 'push',
    'EXP_GIT_REF': 'refs/heads/main',
    'EXP_GIT_REF_NAME': 'main'
}

def create_valid_fixture(target_dir, attempt="1", event="push", git_ref="refs/heads/main", ref_name="main"):
    """Creates a 100% consistent set of diagnostic metadata files."""
    os.makedirs(target_dir, exist_ok=True)

    # 1. metadata.json
    meta = {
        "commit_sha": "abcdef1234567890abcdef1234567890abcdef12",
        "short_sha": "abcdef1",
        "workflow_run_id": "987654321",
        "workflow_run_number": 42,
        "workflow_run_attempt": int(attempt) if attempt.isdigit() else attempt,
        "is_rerun": False,
        "trigger_event": event,
        "git_ref": git_ref,
        "git_ref_name": ref_name,
        "job_name": "x86_64-avx2-fma (gcc-14)",
        "matrix_compiler": "gcc-14",
        "workflow": "HQRN CI & Vector Verification",
        "actor": "developer",
        "timestamp_utc": "2026-11-15T14:19:00Z",
        "timestamp_epoch": 1794752340,
        "artifact_name": "asan-ubsan-failure-test"
    }
    with open(os.path.join(target_dir, 'metadata.json'), 'w', encoding='utf-8') as f:
        json.dump(meta, f, indent=2)

    # 2. diagnostic_metadata.txt
    txt_content = f"""HQRN Continuous Integration - Diagnostic Failure Context
======================================================================
Generated:              2026-11-15T14:19:00Z (Epoch: 1794752340)
Workflow:               HQRN CI & Vector Verification
Workflow Run ID:        987654321 (Run #42)
Workflow Run Attempt:   {attempt}
Trigger Event:          {event}
Git Ref:                {git_ref} (ref_name: {ref_name})
Commit SHA:             abcdef1234567890abcdef1234567890abcdef12
Short SHA:              abcdef1
Actor:                  developer
Job Name:               x86_64-avx2-fma (gcc-14)
Target Matrix/Compiler: gcc-14
Is Re-run (Attempt > 1): false
Diagnostic Artifact:    asan-ubsan-failure-test
======================================================================
"""
    with open(os.path.join(target_dir, 'diagnostic_metadata.txt'), 'w', encoding='utf-8') as f:
        f.write(txt_content)

    # 3. step_summary.md
    summary_content = f"""### ⚠️ Sanitizer-Lauf Fehlgeschlagen: Diagnose-Übersicht
| Parameter | Wert |
|---|---|
| **Commit-SHA** | `abcdef1234567890abcdef1234567890abcdef12` (`abcdef1`) |
| **Workflow Run ID** | [987654321](https://github.com/org/repo/actions/runs/987654321) (Run #42) |
| **Run Attempt (Wiederholung)** | **Versuch #{attempt}** |
| **Trigger-Event** | `{event}` |
| **Git-Ref** | `{git_ref}` (`{ref_name}`) |
| **Jobname** | `x86_64-avx2-fma (gcc-14)` |
| **Compiler / Ziel** | `gcc-14` (x86_64 AVX2/FMA) |
| **Zeitpunkt (UTC)** | `2026-11-15T14:19:00Z` |
| **Artefakt-Paket** | `asan-ubsan-failure-test` |

Die vollständigen Diagnose-Logs wurden als Artefakt hochgeladen.
"""
    with open(os.path.join(target_dir, 'step_summary.md'), 'w', encoding='utf-8') as f:
        f.write(summary_content)

def run_verifier(target_dir, env_overrides=None):
    """Executes verify_diagnostic_metadata.py in a subprocess with custom env."""
    run_env = os.environ.copy()
    run_env.update(DEFAULT_ENV)
    run_env['DIAGNOSTICS_DIR'] = target_dir
    if env_overrides:
        for k, v in env_overrides.items():
            if v is None:
                run_env.pop(k, None)
            else:
                run_env[k] = str(v)

    res = subprocess.run(
        [sys.executable, VERIFIER_SCRIPT],
        env=run_env,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True
    )
    return res.returncode, res.stdout, res.stderr

def run_tests():
    tests = []

    def test(name):
        def decorator(fn):
            tests.append((name, fn))
            return fn
        return decorator

    @test("1. Baseline consistent metadata passes with exit code 0")
    def _(tmp):
        create_valid_fixture(tmp)
        code, out, _ = run_verifier(tmp)
        assert code == 0, f"Expected 0, got {code}. Output:\n{out}"
        assert "[SUCCESS] All diagnostic metadata representations are 100% consistent!" in out

    @test("2. metadata.json: workflow_run_attempt mismatch triggers clear error")
    def _(tmp):
        create_valid_fixture(tmp)
        p = os.path.join(tmp, 'metadata.json')
        with open(p, 'r') as f:
            d = json.load(f)
        d['workflow_run_attempt'] = "2"
        with open(p, 'w') as f:
            json.dump(d, f)

        code, out, _ = run_verifier(tmp)
        assert code == 1, f"Expected 1, got {code}"
        assert 'workflow_run_attempt mismatch (got "2", expected "1")' in out

    @test("3. metadata.json: trigger_event mismatch triggers clear error")
    def _(tmp):
        create_valid_fixture(tmp)
        p = os.path.join(tmp, 'metadata.json')
        with open(p, 'r') as f:
            d = json.load(f)
        d['trigger_event'] = "pull_request"
        with open(p, 'w') as f:
            json.dump(d, f)

        code, out, _ = run_verifier(tmp)
        assert code == 1, f"Expected 1, got {code}"
        assert 'trigger_event mismatch (got "pull_request", expected "push")' in out

    @test("4. metadata.json: git_ref mismatch triggers clear error")
    def _(tmp):
        create_valid_fixture(tmp)
        p = os.path.join(tmp, 'metadata.json')
        with open(p, 'r') as f:
            d = json.load(f)
        d['git_ref'] = "refs/pull/42/merge"
        with open(p, 'w') as f:
            json.dump(d, f)

        code, out, _ = run_verifier(tmp)
        assert code == 1, f"Expected 1, got {code}"
        assert 'git_ref mismatch (got "refs/pull/42/merge", expected "refs/heads/main")' in out

    @test("5. metadata.json: missing key triggers clear error")
    def _(tmp):
        create_valid_fixture(tmp)
        p = os.path.join(tmp, 'metadata.json')
        with open(p, 'r') as f:
            d = json.load(f)
        del d['trigger_event']
        with open(p, 'w') as f:
            json.dump(d, f)

        code, out, _ = run_verifier(tmp)
        assert code == 1, f"Expected 1, got {code}"
        assert 'key "trigger_event" is missing or empty' in out

    @test("6. metadata.json: corrupted JSON syntax triggers parse error")
    def _(tmp):
        create_valid_fixture(tmp)
        p = os.path.join(tmp, 'metadata.json')
        with open(p, 'w') as f:
            f.write("{ invalid json content: true ")

        code, out, _ = run_verifier(tmp)
        assert code == 1, f"Expected 1, got {code}"
        assert 'metadata.json parse error:' in out

    @test("7. diagnostic_metadata.txt: Workflow Run Attempt mismatch triggers clear error")
    def _(tmp):
        create_valid_fixture(tmp)
        p = os.path.join(tmp, 'diagnostic_metadata.txt')
        with open(p, 'r') as f:
            txt = f.read()
        txt = txt.replace("Workflow Run Attempt:   1", "Workflow Run Attempt:   3")
        with open(p, 'w') as f:
            f.write(txt)

        code, out, _ = run_verifier(tmp)
        assert code == 1, f"Expected 1, got {code}"
        assert 'Workflow Run Attempt mismatch (got "3", expected "1")' in out

    @test("8. diagnostic_metadata.txt: missing Trigger Event header triggers clear error")
    def _(tmp):
        create_valid_fixture(tmp)
        p = os.path.join(tmp, 'diagnostic_metadata.txt')
        with open(p, 'r') as f:
            lines = [l for l in f.readlines() if not l.startswith("Trigger Event:")]
        with open(p, 'w') as f:
            f.writelines(lines)

        code, out, _ = run_verifier(tmp)
        assert code == 1, f"Expected 1, got {code}"
        assert '"Trigger Event: <name>" header not found' in out

    @test("9. diagnostic_metadata.txt: Git Ref mismatch triggers clear error")
    def _(tmp):
        create_valid_fixture(tmp)
        p = os.path.join(tmp, 'diagnostic_metadata.txt')
        with open(p, 'r') as f:
            txt = f.read()
        txt = txt.replace("refs/heads/main", "refs/tags/v1.0.0")
        with open(p, 'w') as f:
            f.write(txt)

        code, out, _ = run_verifier(tmp)
        assert code == 1, f"Expected 1, got {code}"
        assert 'Git Ref mismatch (got "refs/tags/v1.0.0", expected "refs/heads/main")' in out

    @test("10. step_summary.md: Run Attempt mismatch triggers clear error")
    def _(tmp):
        create_valid_fixture(tmp)
        p = os.path.join(tmp, 'step_summary.md')
        with open(p, 'r') as f:
            txt = f.read()
        txt = txt.replace("**Versuch #1**", "**Versuch #4**")
        with open(p, 'w') as f:
            f.write(txt)

        code, out, _ = run_verifier(tmp)
        assert code == 1, f"Expected 1, got {code}"
        assert 'Run Attempt mismatch in summary (got "4", expected "1")' in out

    @test("11. step_summary.md: Trigger-Event mismatch triggers clear error")
    def _(tmp):
        create_valid_fixture(tmp)
        p = os.path.join(tmp, 'step_summary.md')
        with open(p, 'r') as f:
            txt = f.read()
        txt = txt.replace("`push`", "`workflow_dispatch`")
        with open(p, 'w') as f:
            f.write(txt)

        code, out, _ = run_verifier(tmp)
        assert code == 1, f"Expected 1, got {code}"
        assert 'Trigger-Event mismatch in summary (got "workflow_dispatch", expected "push")' in out

    @test("12. step_summary.md: Git-Ref mismatch triggers clear error")
    def _(tmp):
        create_valid_fixture(tmp)
        p = os.path.join(tmp, 'step_summary.md')
        with open(p, 'r') as f:
            txt = f.read()
        txt = txt.replace("`refs/heads/main`", "`refs/heads/experimental`")
        with open(p, 'w') as f:
            f.write(txt)

        code, out, _ = run_verifier(tmp)
        assert code == 1, f"Expected 1, got {code}"
        assert 'Git-Ref mismatch in summary (got "refs/heads/experimental", expected "refs/heads/main")' in out

    @test("13. Missing file: missing metadata.json triggers explicit missing file error")
    def _(tmp):
        create_valid_fixture(tmp)
        os.remove(os.path.join(tmp, 'metadata.json'))

        code, out, _ = run_verifier(tmp)
        assert code == 1, f"Expected 1, got {code}"
        assert 'Missing file:' in out and 'metadata.json' in out

    @test("14. Missing file: missing step_summary.md triggers explicit missing file error")
    def _(tmp):
        create_valid_fixture(tmp)
        os.remove(os.path.join(tmp, 'step_summary.md'))

        code, out, _ = run_verifier(tmp)
        assert code == 1, f"Expected 1, got {code}"
        assert 'Missing file:' in out and 'step_summary.md' in out

    @test("15. Missing expected environment variables triggers exit code 2")
    def _(tmp):
        create_valid_fixture(tmp)
        code, out, _ = run_verifier(tmp, env_overrides={'EXP_RUN_ATTEMPT': None})
        assert code == 2, f"Expected 2, got {code}"
        assert 'Missing one or more expected environment variables' in out

    passed = 0
    failed = 0
    print("======================================================================")
    print("HQRN CI - NEGATIVE INCONSISTENCY TEST SUITE FOR DIAGNOSTIC VERIFIER")
    print("======================================================================")

    for name, fn in tests:
        tmp_dir = tempfile.mkdtemp(prefix="hqrn_diag_test_")
        try:
            fn(tmp_dir)
            print(f"[PASS] {name}")
            passed += 1
        except AssertionError as ae:
            print(f"[FAIL] {name}\n       -> {ae}")
            failed += 1
        except Exception as e:
            print(f"[ERROR] {name}\n        -> Unexpected exception: {e}")
            failed += 1
        finally:
            shutil.rmtree(tmp_dir, ignore_errors=True)

    print("----------------------------------------------------------------------")
    print(f"Summary: {passed} passed, {failed} failed out of {len(tests)} test cases.")
    print("======================================================================")

    if failed > 0:
        sys.exit(1)
    sys.exit(0)

if __name__ == '__main__':
    run_tests()
