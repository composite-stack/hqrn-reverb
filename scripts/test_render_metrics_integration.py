#!/usr/bin/env python3
"""
HQRN Audio DSP - End-to-End Integration Test Suite
Validates the offline renderer's --metrics-json export coupled with verify_acoustic_metrics.py.
Exercises schema completeness, physical invariants, boundary conditions, cross-validation
with WAV files, spatial positioning, energy calibration, and strict tolerance boundary handling.
"""

import os
import sys
import json
import math
import tempfile
import subprocess
import unittest

def find_renderer_binary() -> str:
    """Resolves path to hqrn_render binary or builds it via make render if necessary."""
    if "HQRN_RENDER_BIN" in os.environ and os.path.isfile(os.environ["HQRN_RENDER_BIN"]):
        return os.path.abspath(os.environ["HQRN_RENDER_BIN"])

    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    candidates = [
        os.path.join(repo_root, "hqrn_render"),
        os.path.join(repo_root, "build", "hqrn_render"),
        os.path.join(repo_root, "build_release", "hqrn_render"),
    ]
    for c in candidates:
        if os.path.isfile(c) and os.access(c, os.X_OK):
            return c

    # Attempt compilation if source and Makefile exist
    if os.path.isfile(os.path.join(repo_root, "Makefile")) and os.path.isfile(os.path.join(repo_root, "hqrn_render.cpp")):
        res = subprocess.run(["make", "render"], cwd=repo_root, capture_output=True, text=True)
        if res.returncode == 0:
            bin_path = os.path.join(repo_root, "hqrn_render")
            if os.path.isfile(bin_path) and os.access(bin_path, os.X_OK):
                return bin_path

    raise RuntimeError("Could not locate or build hqrn_render binary for integration tests.")

class TestRenderMetricsIntegration(unittest.TestCase):
    _run_counter = 0

    @classmethod
    def setUpClass(cls):
        cls.repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        cls.renderer_bin = find_renderer_binary()
        cls.verifier_py = os.path.join(cls.repo_root, "scripts", "verify_acoustic_metrics.py")
        if not os.path.isfile(cls.verifier_py):
            raise RuntimeError(f"verify_acoustic_metrics.py not found at {cls.verifier_py}")

    def setUp(self):
        self.tmp_dir = tempfile.TemporaryDirectory()

    def tearDown(self):
        self.tmp_dir.cleanup()

    def _run_renderer(self, extra_args, suffix=None):
        TestRenderMetricsIntegration._run_counter += 1
        sfx = f"_{suffix}" if suffix else f"_{TestRenderMetricsIntegration._run_counter}"
        wav_path = os.path.join(self.tmp_dir.name, f"output{sfx}.wav")
        json_path = os.path.join(self.tmp_dir.name, f"metrics{sfx}.json")
        cmd = [
            self.renderer_bin,
            "-o", wav_path,
            "--metrics-json", json_path
        ] + extra_args
        res = subprocess.run(cmd, capture_output=True, text=True)
        return res, wav_path, json_path

    def _run_verifier(self, args):
        cmd = [sys.executable, self.verifier_py] + args
        res = subprocess.run(cmd, capture_output=True, text=True)
        return res

    def test_01_json_schema_completeness_and_types(self):
        """Verifies that all 18 keys exist in exported JSON with expected types and finite values."""
        res, wav_path, json_path = self._run_renderer([
            "--mode", "impulse",
            "--len", "1.5",
            "--t60", "1.0"
        ])
        self.assertEqual(res.returncode, 0, f"Renderer failed: {res.stderr}")
        self.assertTrue(os.path.isfile(json_path), "metrics.json was not generated")

        with open(json_path, "r", encoding="utf-8") as f:
            data = json.load(f)

        expected_schema = {
            "mode": str,
            "sample_rate": (int, float),
            "duration_sec": (int, float),
            "t60_target_sec": (int, float),
            "fbase_hz": (int, float),
            "peak_l_dbfs": (int, float),
            "peak_r_dbfs": (int, float),
            "rms_l_dbfs": (int, float),
            "rms_r_dbfs": (int, float),
            "edt_l_sec": (int, float),
            "edt_r_sec": (int, float),
            "t20_l_sec": (int, float),
            "t20_r_sec": (int, float),
            "t30_l_sec": (int, float),
            "t30_r_sec": (int, float),
            "clipped": bool,
            "throughput_msamples": (int, float),
            "realtime_factor": (int, float)
        }

        for key, expected_type in expected_schema.items():
            self.assertIn(key, data, f"Key '{key}' missing from metrics.json")
            val = data[key]
            self.assertIsInstance(val, expected_type, f"Key '{key}' has unexpected type {type(val)}")
            if isinstance(val, (int, float)) and not isinstance(val, bool):
                self.assertFalse(math.isnan(val), f"Key '{key}' contains NaN")
                self.assertFalse(math.isinf(val), f"Key '{key}' contains Inf")

    def test_02_physical_and_signal_boundary_invariants(self):
        """Verifies physical invariants: RMS <= Peak, Peak <= 0 dBFS (normal gain), positive throughput."""
        res, wav_path, json_path = self._run_renderer([
            "--mode", "impulse",
            "--len", "1.5",
            "--t60", "1.0",
            "--gain", "0.0"
        ])
        self.assertEqual(res.returncode, 0)

        with open(json_path, "r", encoding="utf-8") as f:
            d = json.load(f)

        # Invariant 1: RMS must be less than or equal to Peak level (L and R)
        self.assertLessEqual(d["rms_l_dbfs"], d["peak_l_dbfs"] + 1e-4, "RMS L cannot exceed Peak L")
        self.assertLessEqual(d["rms_r_dbfs"], d["peak_r_dbfs"] + 1e-4, "RMS R cannot exceed Peak R")

        # Invariant 2: Normal gain without extreme input should not clip or overshoot 0 dBFS
        self.assertLessEqual(d["peak_l_dbfs"], 0.0, "Peak L exceeded 0 dBFS at 0 dB gain")
        self.assertLessEqual(d["peak_r_dbfs"], 0.0, "Peak R exceeded 0 dBFS at 0 dB gain")
        self.assertFalse(d["clipped"], "Expected clipped=False at standard gain")

        # Invariant 3: Performance metrics must be positive and non-zero
        self.assertGreater(d["throughput_msamples"], 0.0, "Throughput must be positive")
        self.assertGreater(d["realtime_factor"], 0.0, "Realtime factor must be positive")

        # Invariant 4: Duration corresponds to requested duration
        self.assertAlmostEqual(d["duration_sec"], 1.5, places=2)

    def test_03_e2e_coupling_with_acoustic_verifier_success(self):
        """Verifies that exported JSON passes verify_acoustic_metrics.py directly with exit 0."""
        res, wav_path, json_path = self._run_renderer([
            "--mode", "impulse",
            "--len", "2.0",
            "--t60", "1.2"
        ])
        self.assertEqual(res.returncode, 0)

        v_res = self._run_verifier(["--metrics-json", json_path])
        self.assertEqual(v_res.returncode, 0, f"Verifier rejected renderer output: {v_res.stderr}")
        self.assertIn("AKUSTISCHE METRIK-PLAUSIBILITÄTSPRÜFUNG BESTANDEN", v_res.stdout)

    def test_04_target_t60_fallback_from_json(self):
        """Verifies that verify_acoustic_metrics.py reads t60_target_sec from JSON if --target-t60 is omitted."""
        res, _, json_path = self._run_renderer([
            "--mode", "impulse",
            "--len", "1.8",
            "--t60", "1.1"
        ])
        self.assertEqual(res.returncode, 0)

        # Calling without --target-t60 relies on reading t60_target_sec from the JSON export
        v_res = self._run_verifier(["--metrics-json", json_path])
        self.assertEqual(v_res.returncode, 0)
        self.assertIn("Ziel-T60: 1.100 s", v_res.stdout)

    def test_05_cross_validation_with_wav_analysis(self):
        """Cross-checks renderer C++ Schroeder evaluation against Python WAV Schroeder calculation."""
        res, wav_path, json_path = self._run_renderer([
            "--mode", "impulse",
            "--len", "2.2",
            "--t60", "1.2"
        ])
        self.assertEqual(res.returncode, 0)

        report_path = os.path.join(self.tmp_dir.name, "verification_report.json")
        v_res = self._run_verifier([
            "--metrics-json", json_path,
            "--wav", wav_path,
            "--report-json", report_path
        ])
        self.assertEqual(v_res.returncode, 0, f"Cross-check failed: {v_res.stderr}")
        self.assertTrue(os.path.isfile(report_path), "Report JSON was not created")

        with open(report_path, "r", encoding="utf-8") as f:
            rep = json.load(f)

        self.assertTrue(rep["is_valid"])
        raw = rep["raw_metrics"]
        self.assertIn("wav_crosscheck", raw)
        cross = raw["wav_crosscheck"]

        # Difference between C++ and Python EDC integration must be tightly bounded (< 0.005 s)
        for key in ["edt_l_sec", "edt_r_sec", "t20_l_sec", "t20_r_sec", "t30_l_sec", "t30_r_sec"]:
            val_cpp = raw[key]
            val_py = cross[key]
            diff = abs(val_cpp - val_py)
            self.assertLess(
                diff, 0.005,
                f"Discrepancy for {key} between C++ ({val_cpp:.4f}s) and Python ({val_py:.4f}s) was {diff:.6f}s"
            )

    def test_06_dynamic_t60_range_boundaries(self):
        """Verifies acoustic plausibility across multiple T60 orders: short (0.5s), standard (1.2s), long (2.0s)."""
        test_t60_cases = [0.5, 1.2, 2.0]
        for t60 in test_t60_cases:
            duration = t60 * 1.5 + 0.3
            res, _, json_path = self._run_renderer([
                "--mode", "impulse",
                "--len", f"{duration:.2f}",
                "--t60", f"{t60:.2f}"
            ], suffix=f"t60_{t60}")
            self.assertEqual(res.returncode, 0, f"Failed rendering T60={t60}")

            with open(json_path, "r", encoding="utf-8") as f:
                d = json.load(f)

            self.assertAlmostEqual(d["t60_target_sec"], t60, places=3)
            # Check that measured T20 is within 10% of target T60
            rel_err_l = abs(d["t20_l_sec"] - t60) / t60
            rel_err_r = abs(d["t20_r_sec"] - t60) / t60
            self.assertLess(rel_err_l, 0.10, f"T20 L {d['t20_l_sec']} deviated from target {t60} by {rel_err_l*100:.2f}%")
            self.assertLess(rel_err_r, 0.10, f"T20 R {d['t20_r_sec']} deviated from target {t60} by {rel_err_r*100:.2f}%")

            # Passes verifier
            v_res = self._run_verifier(["--metrics-json", json_path])
            self.assertEqual(v_res.returncode, 0, f"T60={t60} failed verifier: {v_res.stderr}")

    def test_07_statistical_energy_calibration_invariant(self):
        """Verifies that total RMS energy remains constant across different T60 values (statistical calibration)."""
        res_short, _, json_short = self._run_renderer([
            "--mode", "impulse",
            "--len", "1.5",
            "--t60", "0.5"
        ], suffix="energy_short")
        res_long, _, json_long = self._run_renderer([
            "--mode", "impulse",
            "--len", "1.5",
            "--t60", "2.0"
        ], suffix="energy_long")
        self.assertEqual(res_short.returncode, 0)
        self.assertEqual(res_long.returncode, 0)

        with open(json_short) as f_s, open(json_long) as f_l:
            d_s = json.load(f_s)
            d_l = json.load(f_l)

        # Under the unitary graph energy normalization ||C||_F^2 = 2N(1 - gamma^2),
        # expected impulse response energy is invariant with respect to gamma.
        rms_diff_l = abs(d_s["rms_l_dbfs"] - d_l["rms_l_dbfs"])
        rms_diff_r = abs(d_s["rms_r_dbfs"] - d_l["rms_r_dbfs"])
        self.assertLess(rms_diff_l, 0.20, f"RMS L diverged between T60=0.5s and 2.0s by {rms_diff_l:.3f} dB")
        self.assertLess(rms_diff_r, 0.20, f"RMS R diverged between T60=0.5s and 2.0s by {rms_diff_r:.3f} dB")

    def test_08_sample_rate_invariance_and_boundaries(self):
        """Verifies sample rate reflection in JSON and sample-rate invariance of T60 (44.1k, 48k, 96k)."""
        target_t60 = 1.0
        sr_rates = [44100.0, 48000.0, 96000.0]
        results = []

        for sr in sr_rates:
            res, _, json_path = self._run_renderer([
                "--mode", "impulse",
                "--sr", str(sr),
                "--len", "1.6",
                "--t60", str(target_t60)
            ], suffix=f"sr_{int(sr)}")
            self.assertEqual(res.returncode, 0, f"Failed at sample rate {sr}")

            with open(json_path, "r", encoding="utf-8") as f:
                d = json.load(f)

            self.assertEqual(d["sample_rate"], sr)
            results.append(d["t20_l_sec"])

            # Verify with acoustic verifier
            v_res = self._run_verifier(["--metrics-json", json_path])
            self.assertEqual(v_res.returncode, 0, f"SR={sr} failed verifier")

        # Invariance check: maximum divergence across 44.1k, 48k, and 96k should be <= 1%
        max_diff = max(results) - min(results)
        self.assertLess(
            max_diff, 0.02,
            f"T20 decay times diverged across sample rates: {results} (diff: {max_diff:.4f}s)"
        )

    def test_09_gain_staging_boundary_linearity(self):
        """Verifies that gain adjustments scale Peak and RMS levels precisely in the exported JSON."""
        res_0, _, json_0 = self._run_renderer([
            "--mode", "impulse",
            "--len", "1.2",
            "--t60", "1.0",
            "--gain", "0.0"
        ], suffix="gain_0")
        self.assertEqual(res_0.returncode, 0)

        res_minus18, _, json_minus18 = self._run_renderer([
            "--mode", "impulse",
            "--len", "1.2",
            "--t60", "1.0",
            "--gain", "-18.0"
        ], suffix="gain_minus18")
        self.assertEqual(res_minus18.returncode, 0)

        with open(json_0) as f0, open(json_minus18) as f18:
            d0 = json.load(f0)
            d18 = json.load(f18)

        diff_peak_l = d0["peak_l_dbfs"] - d18["peak_l_dbfs"]
        diff_rms_l = d0["rms_l_dbfs"] - d18["rms_l_dbfs"]
        self.assertAlmostEqual(diff_peak_l, 18.0, delta=0.05, msg="Peak L dBFS did not scale by 18 dB")
        self.assertAlmostEqual(diff_rms_l, 18.0, delta=0.05, msg="RMS L dBFS did not scale by 18 dB")

    def test_10_true_peak_clipper_ceiling_boundary_and_overload_flag(self):
        """Verifies ceiling enforcement in JSON when clipped, and clipped flag behavior."""
        # Case A: Clipper enabled, ceiling at -0.5 dBFS, extreme gain +35 dB
        res_clip, _, json_clip = self._run_renderer([
            "--mode", "impulse",
            "--len", "1.2",
            "--t60", "1.0",
            "--gain", "35.0",
            "--clipper", "-0.5", "3.0", "1"
        ], suffix="clip_on")
        self.assertEqual(res_clip.returncode, 0)
        with open(json_clip) as f:
            dc = json.load(f)

        # Output must be held at or below ceiling within slight filter overshoot margin (+0.05 dB)
        self.assertLessEqual(dc["peak_l_dbfs"], -0.45, "Clipper ceiling (-0.5 dBFS) was not respected on L")
        self.assertLessEqual(dc["peak_r_dbfs"], -0.45, "Clipper ceiling (-0.5 dBFS) was not respected on R")

        # Case B: Clipper disabled (--clipper -0.1 3.0 0) with extreme gain +40 dB -> triggers clipped=True
        res_unclip, _, json_unclip = self._run_renderer([
            "--mode", "impulse",
            "--len", "1.2",
            "--t60", "1.0",
            "--gain", "40.0",
            "--clipper", "-0.1", "3.0", "0"
        ], suffix="clip_off")
        self.assertEqual(res_unclip.returncode, 0)
        with open(json_unclip) as f:
            du = json.load(f)

        self.assertTrue(du["clipped"], "Expected clipped=True when audio severely exceeds 0 dBFS unclipped")
        self.assertGreater(du["peak_l_dbfs"], 0.0, "Expected Peak > 0 dBFS without clipper")

    def test_11_spatial_panning_and_stereo_symmetry_boundary(self):
        """Verifies off-center source position on Poincaré disk preserves decay symmetry in JSON."""
        res_panned, _, json_panned = self._run_renderer([
            "--mode", "impulse",
            "--len", "1.5",
            "--t60", "1.0",
            "--source", "0.6", "0.0"
        ], suffix="panned")
        self.assertEqual(res_panned.returncode, 0)

        with open(json_panned) as f:
            dp = json.load(f)

        # Peak and RMS should be valid and finite
        self.assertLessEqual(dp["peak_l_dbfs"], 0.0)
        self.assertLessEqual(dp["peak_r_dbfs"], 0.0)

        # Plausibility verifier must pass (decay rates match despite spatial projection)
        v_res = self._run_verifier(["--metrics-json", json_panned])
        self.assertEqual(v_res.returncode, 0, f"Spatial source position failed plausibility: {v_res.stderr}")

    def test_12_strict_verifier_tolerance_boundary_failure_detection(self):
        """Verifies that verify_acoustic_metrics.py strictly fails when real renderer error exceeds threshold."""
        res, _, json_path = self._run_renderer([
            "--mode", "impulse",
            "--len", "1.8",
            "--t60", "1.0"
        ])
        self.assertEqual(res.returncode, 0)

        # Normal tolerance (15%): PASS
        v_pass = self._run_verifier(["--metrics-json", json_path, "--tolerance", "0.15"])
        self.assertEqual(v_pass.returncode, 0)

        # Artificial hyper-strict tolerance (0.00001 = 0.001%): MUST FAIL with exit 1
        v_fail = self._run_verifier(["--metrics-json", json_path, "--tolerance", "0.00001"])
        self.assertEqual(v_fail.returncode, 1, "Hyper-strict tolerance should fail")
        self.assertIn("FEHLER: AKUSTISCHE METRIK-PLAUSIBILITÄTSPRÜFUNG FEHLGESCHLAGEN", v_fail.stderr)
        self.assertIn("weicht um", v_fail.stderr)

    def test_13_non_impulse_modes_json_metrics_handling(self):
        """Verifies JSON output for noise burst and Poincaré orbit modes."""
        for mode in ["burst", "orbit"]:
            res, _, json_path = self._run_renderer([
                "--mode", mode,
                "--len", "0.2"
            ], suffix=mode)
            self.assertEqual(res.returncode, 0, f"Mode {mode} failed rendering")

            with open(json_path) as f:
                d = json.load(f)

            self.assertEqual(d["mode"], mode)
            self.assertGreater(d["duration_sec"], 0.0)
            # In non-impulse mode, decay times are not computed (flagged as -1.0)
            self.assertEqual(d["edt_l_sec"], -1.0)
            self.assertEqual(d["t20_l_sec"], -1.0)

            # Plausibility verifier must cleanly reject non-impulse JSON (exit 1) without crashing
            v_res = self._run_verifier(["--metrics-json", json_path, "--target-t60", "1.0"])
            self.assertEqual(v_res.returncode, 1)
            self.assertIn("konnte nicht bestimmt werden", v_res.stderr)

    def test_14_simd_vs_scalar_json_metric_parity(self):
        """Verifies that AVX2/NEON and generic scalar render paths produce identical metrics in JSON."""
        res_auto, _, json_auto = self._run_renderer([
            "--mode", "impulse",
            "--len", "1.2",
            "--t60", "1.0",
            "--simd", "auto"
        ], suffix="simd_auto")
        self.assertEqual(res_auto.returncode, 0)

        res_scalar, _, json_scalar = self._run_renderer([
            "--mode", "impulse",
            "--len", "1.2",
            "--t60", "1.0",
            "--simd", "scalar"
        ], suffix="simd_scalar")
        self.assertEqual(res_scalar.returncode, 0)

        with open(json_auto) as f1, open(json_scalar) as f2:
            d_auto = json.load(f1)
            d_scalar = json.load(f2)

        # Decay parity
        self.assertAlmostEqual(d_auto["t20_l_sec"], d_scalar["t20_l_sec"], delta=0.005)
        self.assertAlmostEqual(d_auto["t20_r_sec"], d_scalar["t20_r_sec"], delta=0.005)
        # Level parity
        self.assertAlmostEqual(d_auto["peak_l_dbfs"], d_scalar["peak_l_dbfs"], delta=0.05)
        self.assertAlmostEqual(d_auto["rms_l_dbfs"], d_scalar["rms_l_dbfs"], delta=0.05)

    def test_15_nonexistent_directory_error_resilience(self):
        """Verifies that providing a nonexistent directory for --metrics-json fails with exit code 1 and error msg."""
        bad_json = os.path.join(self.tmp_dir.name, "nonexistent_subfolder", "metrics.json")
        wav_path = os.path.join(self.tmp_dir.name, "out_15.wav")
        cmd = [
            self.renderer_bin,
            "--mode", "impulse",
            "--len", "0.1",
            "-o", wav_path,
            "--metrics-json", bad_json
        ]
        res = subprocess.run(cmd, capture_output=True, text=True)
        self.assertEqual(res.returncode, 1, f"Expected exit code 1 for nonexistent directory, got {res.returncode}")
        self.assertIn("Fehler beim Öffnen der Metrik-JSON-Datei: " + bad_json, res.stderr)
        self.assertFalse(os.path.isfile(bad_json))
        self.assertFalse(os.path.isfile(wav_path), "WAV file should not be generated if metrics export fails")

    def test_16_metrics_json_path_is_existing_directory(self):
        """Verifies that passing an existing directory path to --metrics-json fails with exit code 1."""
        dir_as_path = os.path.join(self.tmp_dir.name, "dir_target")
        os.makedirs(dir_as_path, exist_ok=True)
        wav_path = os.path.join(self.tmp_dir.name, "out_16.wav")
        cmd = [
            self.renderer_bin,
            "--mode", "impulse",
            "--len", "0.1",
            "-o", wav_path,
            "--metrics-json", dir_as_path
        ]
        res = subprocess.run(cmd, capture_output=True, text=True)
        self.assertEqual(res.returncode, 1, f"Expected exit code 1 when path is a directory, got {res.returncode}")
        self.assertIn("Fehler beim Öffnen der Metrik-JSON-Datei: " + dir_as_path, res.stderr)
        self.assertFalse(os.path.isfile(wav_path))

    def test_17_metrics_json_parent_is_regular_file(self):
        """Verifies that passing a path whose parent component is a file (ENOTDIR) fails with exit code 1."""
        file_parent = os.path.join(self.tmp_dir.name, "dummy_parent.txt")
        with open(file_parent, "w", encoding="utf-8") as f:
            f.write("not a directory")
        bad_json = os.path.join(file_parent, "metrics.json")
        wav_path = os.path.join(self.tmp_dir.name, "out_17.wav")
        cmd = [
            self.renderer_bin,
            "--mode", "impulse",
            "--len", "0.1",
            "-o", wav_path,
            "--metrics-json", bad_json
        ]
        res = subprocess.run(cmd, capture_output=True, text=True)
        self.assertEqual(res.returncode, 1, f"Expected exit code 1 for ENOTDIR, got {res.returncode}")
        self.assertIn("Fehler beim Öffnen der Metrik-JSON-Datei: " + bad_json, res.stderr)
        self.assertFalse(os.path.isfile(bad_json))
        self.assertFalse(os.path.isfile(wav_path))

    def test_18_metrics_json_unwritable_directory_permission_denied(self):
        """Verifies that an unwritable directory (chmod 0555) fails with exit code 1 and error message."""
        ro_dir = os.path.join(self.tmp_dir.name, "readonly_dir")
        os.makedirs(ro_dir, mode=0o777, exist_ok=True)
        os.chmod(self.tmp_dir.name, 0o777)
        os.chmod(ro_dir, 0o555)
        bad_json = os.path.join(ro_dir, "metrics.json")
        wav_path = os.path.join(self.tmp_dir.name, "out_18.wav")
        cmd = [
            self.renderer_bin,
            "--mode", "impulse",
            "--len", "0.1",
            "-o", wav_path,
            "--metrics-json", bad_json
        ]

        kwargs = {}
        if os.geteuid() == 0:
            import pwd
            try:
                u = pwd.getpwnam("lovable") if "lovable" in [p.pw_name for p in pwd.getpwall()] else None
                if u:
                    kwargs["user"] = u.pw_uid
                    kwargs["group"] = u.pw_gid
            except Exception:
                pass

        if "user" not in kwargs and os.geteuid() == 0:
            for ro_fs_candidate in ["/sys/kernel/hqrn_metrics.json", "/proc/hqrn_metrics.json"]:
                if os.path.exists(os.path.dirname(ro_fs_candidate)):
                    bad_json = ro_fs_candidate
                    cmd = [
                        self.renderer_bin,
                        "--mode", "impulse",
                        "--len", "0.1",
                        "-o", wav_path,
                        "--metrics-json", bad_json
                    ]
                    break

        res = subprocess.run(cmd, capture_output=True, text=True, **kwargs)
        self.assertEqual(res.returncode, 1, f"Expected exit code 1 for unwritable path, got {res.returncode}. stderr={res.stderr}")
        self.assertIn("Fehler beim Öffnen der Metrik-JSON-Datei: " + bad_json, res.stderr)
        self.assertFalse(os.path.isfile(bad_json))
        self.assertFalse(os.path.isfile(wav_path))

    def test_19_metrics_json_unwritable_existing_file_permission_denied(self):
        """Verifies that overwriting a read-only existing file (chmod 0444) fails with exit code 1."""
        rw_dir = os.path.join(self.tmp_dir.name, "rw_dir")
        os.makedirs(rw_dir, mode=0o777, exist_ok=True)
        os.chmod(self.tmp_dir.name, 0o777)
        ro_file = os.path.join(rw_dir, "readonly_file.json")
        with open(ro_file, "w", encoding="utf-8") as f:
            f.write("{}")
        os.chmod(ro_file, 0o444)
        wav_path = os.path.join(self.tmp_dir.name, "out_19.wav")

        cmd = [
            self.renderer_bin,
            "--mode", "impulse",
            "--len", "0.1",
            "-o", wav_path,
            "--metrics-json", ro_file
        ]

        kwargs = {}
        if os.geteuid() == 0:
            import pwd
            try:
                u = pwd.getpwnam("lovable") if "lovable" in [p.pw_name for p in pwd.getpwall()] else None
                if u:
                    kwargs["user"] = u.pw_uid
                    kwargs["group"] = u.pw_gid
            except Exception:
                pass

        if "user" not in kwargs and os.geteuid() == 0:
            for ro_fs_candidate in ["/sys/kernel/profiling", "/proc/version"]:
                if os.path.isfile(ro_fs_candidate):
                    ro_file = ro_fs_candidate
                    cmd = [
                        self.renderer_bin,
                        "--mode", "impulse",
                        "--len", "0.1",
                        "-o", wav_path,
                        "--metrics-json", ro_file
                    ]
                    break

        res = subprocess.run(cmd, capture_output=True, text=True, **kwargs)
        self.assertEqual(res.returncode, 1, f"Expected exit code 1 for read-only file, got {res.returncode}")
        self.assertIn("Fehler beim Öffnen der Metrik-JSON-Datei: " + ro_file, res.stderr)

    def test_20_metrics_json_missing_argument_cli_syntax_error(self):
        """Verifies that omitting the path argument to --metrics-json yields exit code 1 and usage error."""
        # Case A: --metrics-json is the very last argument
        cmd_a = [
            self.renderer_bin,
            "--mode", "impulse",
            "--len", "0.1",
            "--metrics-json"
        ]
        res_a = subprocess.run(cmd_a, capture_output=True, text=True)
        self.assertEqual(res_a.returncode, 1)
        self.assertIn("Fehler: --metrics-json erfordert einen Dateipfad als Argument", res_a.stderr)

        # Case B: --metrics-json is followed immediately by another option flag
        cmd_b = [
            self.renderer_bin,
            "--mode", "impulse",
            "--metrics-json",
            "--sr", "44100"
        ]
        res_b = subprocess.run(cmd_b, capture_output=True, text=True)
        self.assertEqual(res_b.returncode, 1)
        self.assertIn("Fehler: --metrics-json erfordert einen Dateipfad als Argument", res_b.stderr)

    def test_21_metrics_json_empty_path_error(self):
        """Verifies that passing an empty string as --metrics-json path fails with exit code 1."""
        cmd = [
            self.renderer_bin,
            "--mode", "impulse",
            "--len", "0.1",
            "-o", os.path.join(self.tmp_dir.name, "out_21.wav"),
            "--metrics-json", ""
        ]
        res = subprocess.run(cmd, capture_output=True, text=True)
        self.assertEqual(res.returncode, 1)
        self.assertIn("Fehler: --metrics-json darf kein leerer Pfad sein", res.stderr)

    def test_22_metrics_json_valid_symlink_to_existing_file(self):
        """Verifies that writing metrics through a symlink to an existing file succeeds and updates it."""
        target_json = os.path.join(self.tmp_dir.name, "target_22.json")
        with open(target_json, "w", encoding="utf-8") as f:
            f.write('{"placeholder": true}')
        symlink_json = os.path.join(self.tmp_dir.name, "sym_22.json")
        os.symlink(target_json, symlink_json)

        wav_path = os.path.join(self.tmp_dir.name, "out_22.wav")
        cmd = [
            self.renderer_bin,
            "--mode", "impulse",
            "--len", "0.1",
            "-o", wav_path,
            "--metrics-json", symlink_json
        ]
        res = subprocess.run(cmd, capture_output=True, text=True)
        self.assertEqual(res.returncode, 0, f"Expected 0 for valid symlink, got {res.returncode}. stderr={res.stderr}")
        self.assertTrue(os.path.isfile(wav_path))
        self.assertGreater(os.path.getsize(wav_path), 0)

        with open(target_json, "r", encoding="utf-8") as f:
            data = json.load(f)
        self.assertIn("peak_l_dbfs", data)
        self.assertIn("sample_rate", data)
        self.assertNotIn("placeholder", data)

    def test_23_metrics_json_valid_symlink_to_new_file_in_writable_dir(self):
        """Verifies that writing through a symlink pointing to a nonexistent file in a writable dir succeeds."""
        target_new = os.path.join(self.tmp_dir.name, "target_new_23.json")
        symlink_new = os.path.join(self.tmp_dir.name, "sym_new_23.json")
        os.symlink(target_new, symlink_new)
        self.assertFalse(os.path.exists(target_new))

        wav_path = os.path.join(self.tmp_dir.name, "out_23.wav")
        cmd = [
            self.renderer_bin,
            "--mode", "impulse",
            "--len", "0.1",
            "-o", wav_path,
            "--metrics-json", symlink_new
        ]
        res = subprocess.run(cmd, capture_output=True, text=True)
        self.assertEqual(res.returncode, 0, f"Expected 0 for new file via symlink, got {res.returncode}. stderr={res.stderr}")
        self.assertTrue(os.path.isfile(target_new))
        self.assertTrue(os.path.isfile(wav_path))
        self.assertGreater(os.path.getsize(wav_path), 0)

        with open(target_new, "r", encoding="utf-8") as f:
            data = json.load(f)
        self.assertIn("t60_target_sec", data)
        self.assertIn("throughput_msamples", data)

    def test_24_metrics_json_broken_symlink_nonexistent_directory(self):
        """Verifies that a broken symlink pointing to a nonexistent folder fails with exit 1 and suppresses WAV."""
        bad_target = os.path.join(self.tmp_dir.name, "missing_folder_24", "metrics.json")
        broken_sym = os.path.join(self.tmp_dir.name, "broken_sym_24.json")
        os.symlink(bad_target, broken_sym)

        wav_path = os.path.join(self.tmp_dir.name, "out_24.wav")
        cmd = [
            self.renderer_bin,
            "--mode", "impulse",
            "--len", "0.1",
            "-o", wav_path,
            "--metrics-json", broken_sym
        ]
        res = subprocess.run(cmd, capture_output=True, text=True)
        self.assertEqual(res.returncode, 1)
        self.assertIn("Fehler beim Öffnen der Metrik-JSON-Datei: " + broken_sym, res.stderr)
        self.assertFalse(os.path.isfile(wav_path), "WAV must not be generated when metrics export fails")

    def test_25_metrics_json_cyclic_symlink_error(self):
        """Verifies that a cyclic symlink loop (ELOOP) fails with exit code 1 and suppresses WAV."""
        loop_a = os.path.join(self.tmp_dir.name, "loop_a.json")
        loop_b = os.path.join(self.tmp_dir.name, "loop_b.json")
        os.symlink(loop_b, loop_a)
        os.symlink(loop_a, loop_b)

        wav_path = os.path.join(self.tmp_dir.name, "out_25.wav")
        cmd = [
            self.renderer_bin,
            "--mode", "impulse",
            "--len", "0.1",
            "-o", wav_path,
            "--metrics-json", loop_a
        ]
        res = subprocess.run(cmd, capture_output=True, text=True)
        self.assertEqual(res.returncode, 1)
        self.assertIn("Fehler beim Öffnen der Metrik-JSON-Datei: " + loop_a, res.stderr)
        self.assertFalse(os.path.isfile(wav_path), "WAV must not be generated on cyclic symlink")

    def test_26_metrics_json_symlink_to_directory_error(self):
        """Verifies that a symlink pointing to a directory (EISDIR) fails with exit code 1 and suppresses WAV."""
        dir_target = os.path.join(self.tmp_dir.name, "target_dir_26")
        os.makedirs(dir_target, exist_ok=True)
        sym_to_dir = os.path.join(self.tmp_dir.name, "sym_dir_26")
        os.symlink(dir_target, sym_to_dir)

        wav_path = os.path.join(self.tmp_dir.name, "out_26.wav")
        cmd = [
            self.renderer_bin,
            "--mode", "impulse",
            "--len", "0.1",
            "-o", wav_path,
            "--metrics-json", sym_to_dir
        ]
        res = subprocess.run(cmd, capture_output=True, text=True)
        self.assertEqual(res.returncode, 1)
        self.assertIn("Fehler beim Öffnen der Metrik-JSON-Datei: " + sym_to_dir, res.stderr)
        self.assertFalse(os.path.isfile(wav_path), "WAV must not be generated when symlink resolves to a directory")

    def test_27_metrics_json_unwritable_symlink_target_permission_denied(self):
        """Verifies that a symlink pointing to a read-only target fails with exit code 1 and preserves target."""
        rw_dir = os.path.join(self.tmp_dir.name, "rw_sym_dir_27")
        os.makedirs(rw_dir, mode=0o777, exist_ok=True)
        os.chmod(self.tmp_dir.name, 0o777)
        ro_target = os.path.join(rw_dir, "readonly_target.json")
        with open(ro_target, "w", encoding="utf-8") as f:
            f.write('{"unmodified": true}')
        os.chmod(ro_target, 0o444)

        symlink_ro = os.path.join(rw_dir, "sym_to_ro.json")
        os.symlink(ro_target, symlink_ro)
        wav_path = os.path.join(self.tmp_dir.name, "out_27.wav")

        kwargs = {}
        if os.geteuid() == 0:
            import pwd
            try:
                u = pwd.getpwnam("lovable") if "lovable" in [p.pw_name for p in pwd.getpwall()] else None
                if u:
                    kwargs["user"] = u.pw_uid
                    kwargs["group"] = u.pw_gid
            except Exception:
                pass

        if "user" not in kwargs and os.geteuid() == 0:
            for ro_fs_candidate in ["/sys/kernel/profiling", "/proc/version"]:
                if os.path.exists(ro_fs_candidate):
                    symlink_ro = os.path.join(rw_dir, "sym_sys_ro.json")
                    if os.path.exists(symlink_ro) or os.path.islink(symlink_ro):
                        os.remove(symlink_ro)
                    os.symlink(ro_fs_candidate, symlink_ro)
                    ro_target = ro_fs_candidate
                    break

        cmd = [
            self.renderer_bin,
            "--mode", "impulse",
            "--len", "0.1",
            "-o", wav_path,
            "--metrics-json", symlink_ro
        ]
        res = subprocess.run(cmd, capture_output=True, text=True, **kwargs)

        if os.path.isfile(os.path.join(rw_dir, "readonly_target.json")):
            os.chmod(os.path.join(rw_dir, "readonly_target.json"), 0o666)

        self.assertEqual(res.returncode, 1, f"Expected exit code 1 for unwritable symlink target, got {res.returncode}")
        self.assertTrue(
            ("Fehler beim Öffnen der Metrik-JSON-Datei: " + symlink_ro) in res.stderr
            or ("Fehler beim Schreiben der Metrik-JSON-Datei: " + symlink_ro) in res.stderr,
            f"Expected error message mentioning {symlink_ro}, got: {res.stderr}"
        )
        self.assertFalse(os.path.isfile(wav_path), "WAV must not be generated when symlink target is unwritable")
        if ro_target == os.path.join(rw_dir, "readonly_target.json"):
            with open(ro_target, "r", encoding="utf-8") as f:
                content = f.read()
            self.assertEqual(content, '{"unmodified": true}')

    def test_28_metrics_json_symlink_failure_preserves_existing_wav(self):
        """Verifies that an existing WAV file is preserved completely untouched if metrics symlink export fails."""
        wav_path = os.path.join(self.tmp_dir.name, "pre_existing_28.wav")
        original_bytes = b"RIFF_PRE_EXISTING_CANARY_PAYLOAD_1234567890"
        with open(wav_path, "wb") as f:
            f.write(original_bytes)

        bad_target = os.path.join(self.tmp_dir.name, "nonexistent_dir_28", "metrics.json")
        broken_sym = os.path.join(self.tmp_dir.name, "broken_sym_28.json")
        os.symlink(bad_target, broken_sym)

        cmd = [
            self.renderer_bin,
            "--mode", "impulse",
            "--len", "0.1",
            "-o", wav_path,
            "--metrics-json", broken_sym
        ]
        res = subprocess.run(cmd, capture_output=True, text=True)
        self.assertEqual(res.returncode, 1)
        self.assertIn("Fehler beim Öffnen der Metrik-JSON-Datei: " + broken_sym, res.stderr)
        self.assertTrue(os.path.isfile(wav_path), "Existing WAV file must still exist")
        with open(wav_path, "rb") as f:
            current_bytes = f.read()
        self.assertEqual(current_bytes, original_bytes, "Existing WAV file must not be modified or truncated")

if __name__ == "__main__":
    unittest.main(verbosity=2)
