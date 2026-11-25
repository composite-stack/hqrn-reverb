#!/usr/bin/env python3
"""
Automated Test Suite for verify_acoustic_metrics.py
Tests positive validation, negative injection scenarios, and error formatting
for EDT, T20, and T30 plausibility verification.
"""

import os
import sys
import json
import tempfile
import subprocess
import unittest
import math

class TestVerifyAcousticMetrics(unittest.TestCase):
    def setUp(self):
        self.script_path = os.path.join(
            os.path.dirname(os.path.abspath(__file__)),
            "verify_acoustic_metrics.py"
        )
        self.tmp_dir = tempfile.TemporaryDirectory()

    def tearDown(self):
        self.tmp_dir.cleanup()

    def _run_verifier(self, args):
        cmd = [sys.executable, self.script_path] + args
        res = subprocess.run(cmd, capture_output=True, text=True)
        return res.returncode, res.stdout, res.stderr

    def _write_json(self, data):
        p = os.path.join(self.tmp_dir.name, "test_metrics.json")
        with open(p, "w", encoding="utf-8") as f:
            json.dump(data, f, indent=2)
        return p

    def test_01_valid_metrics_pass(self):
        """Valid EDT, T20, T30 matching target T60 within tolerance must pass with exit 0."""
        data = {
            "mode": "impulse",
            "sample_rate": 48000.0,
            "duration_sec": 2.0,
            "t60_target_sec": 1.2,
            "fbase_hz": 120.0,
            "peak_l_dbfs": -34.5,
            "peak_r_dbfs": -29.8,
            "rms_l_dbfs": -53.8,
            "rms_r_dbfs": -46.5,
            "edt_l_sec": 1.201,
            "edt_r_sec": 1.199,
            "t20_l_sec": 1.200,
            "t20_r_sec": 1.200,
            "t30_l_sec": 1.200,
            "t30_r_sec": 1.200,
            "clipped": False
        }
        json_path = self._write_json(data)
        rc, out, err = self._run_verifier(["--metrics-json", json_path])
        self.assertEqual(rc, 0, f"Expected 0 but got {rc}. Stderr: {err}")
        self.assertIn("AKUSTISCHE METRIK-PLAUSIBILITÄTSPRÜFUNG BESTANDEN", out)
        self.assertIn("Ziel-T60: 1.200 s", out)
        self.assertIn("EDT", out)
        self.assertIn("T20", out)
        self.assertIn("T30", out)

    def test_02_negative_uncalculated_edt(self):
        """Uncalculated EDT (-1.0 s) must fail with exit 1 and detailed diagnostic error output."""
        data = {
            "t60_target_sec": 1.2,
            "edt_l_sec": -1.0,
            "edt_r_sec": 1.20,
            "t20_l_sec": 1.20,
            "t20_r_sec": 1.20,
            "t30_l_sec": 1.20,
            "t30_r_sec": 1.20
        }
        json_path = self._write_json(data)
        rc, out, err = self._run_verifier(["--metrics-json", json_path])
        self.assertEqual(rc, 1)
        self.assertIn("FEHLER: AKUSTISCHE METRIK-PLAUSIBILITÄTSPRÜFUNG FEHLGESCHLAGEN", err)
        self.assertIn("EDT (Links)", err)
        self.assertIn("konnte nicht bestimmt werden", err)
        self.assertIn("Metrik   Kanal Links", err)

    def test_03_negative_uncalculated_t30(self):
        """Uncalculated T30 (-1.0 s) must fail with exit 1 and specific reason."""
        data = {
            "t60_target_sec": 1.5,
            "edt_l_sec": 1.48,
            "edt_r_sec": 1.49,
            "t20_l_sec": 1.50,
            "t20_r_sec": 1.50,
            "t30_l_sec": 1.50,
            "t30_r_sec": -1.0
        }
        json_path = self._write_json(data)
        rc, out, err = self._run_verifier(["--metrics-json", json_path])
        self.assertEqual(rc, 1)
        self.assertIn("FEHLER: AKUSTISCHE METRIK-PLAUSIBILITÄTSPRÜFUNG FEHLGESCHLAGEN", err)
        self.assertIn("T30 (Rechts)", err)
        self.assertIn("konnte nicht bestimmt werden", err)

    def test_04_negative_nan_or_inf(self):
        """NaN or Inf metrics must fail with exit 1 and numerical instability notice."""
        data = {
            "t60_target_sec": 1.2,
            "edt_l_sec": float("nan"),
            "edt_r_sec": 1.20,
            "t20_l_sec": 1.20,
            "t20_r_sec": 1.20,
            "t30_l_sec": 1.20,
            "t30_r_sec": 1.20
        }
        p = os.path.join(self.tmp_dir.name, "nan_metrics.json")
        with open(p, "w", encoding="utf-8") as f:
            f.write('{"t60_target_sec": 1.2, "edt_l_sec": NaN, "edt_r_sec": 1.2, "t20_l_sec": 1.2, "t20_r_sec": 1.2, "t30_l_sec": 1.2, "t30_r_sec": 1.2}')
        rc, out, err = self._run_verifier(["--metrics-json", p])
        self.assertEqual(rc, 1)
        self.assertIn("FEHLER: AKUSTISCHE METRIK-PLAUSIBILITÄTSPRÜFUNG FEHLGESCHLAGEN", err)
        self.assertIn("nicht-finit", err)

    def test_05_negative_t60_out_of_tolerance(self):
        """Values deviating beyond tolerance (default 15%) must fail with exit 1."""
        data = {
            "t60_target_sec": 1.2,
            "edt_l_sec": 1.20,
            "edt_r_sec": 1.20,
            "t20_l_sec": 1.20,
            "t20_r_sec": 1.20,
            "t30_l_sec": 1.20,
            "t30_r_sec": 0.85 # -29.2% error
        }
        json_path = self._write_json(data)
        rc, out, err = self._run_verifier(["--metrics-json", json_path, "--tolerance", "0.15"])
        self.assertEqual(rc, 1)
        self.assertIn("FEHLER: AKUSTISCHE METRIK-PLAUSIBILITÄTSPRÜFUNG FEHLGESCHLAGEN", err)
        self.assertIn("T30_R", err)
        self.assertIn("weicht um", err)
        self.assertIn("vom Zielwert T60", err)

    def test_06_negative_stereo_asymmetry(self):
        """Asymmetry between L and R exceeding stereo_tolerance must fail with exit 1."""
        data = {
            "t60_target_sec": 1.2,
            "edt_l_sec": 1.10,
            "edt_r_sec": 1.30,
            "t20_l_sec": 1.05,
            "t20_r_sec": 1.32, # 20.5% diff
            "t30_l_sec": 1.20,
            "t30_r_sec": 1.20
        }
        json_path = self._write_json(data)
        rc, out, err = self._run_verifier(["--metrics-json", json_path, "--stereo-tolerance", "0.10"])
        self.assertEqual(rc, 1)
        self.assertIn("Stereo-Asymmetrie bei T20", err)

    def test_07_negative_decay_profile_divergence(self):
        """Severe divergence between EDT and T20/T30 indicating abnormal decay curve must fail."""
        data = {
            "t60_target_sec": 1.2,
            "edt_l_sec": 1.35, # +28.6% vs T20
            "edt_r_sec": 1.05,
            "t20_l_sec": 1.05,
            "t20_r_sec": 1.05,
            "t30_l_sec": 1.05,
            "t30_r_sec": 1.05
        }
        json_path = self._write_json(data)
        rc, out, err = self._run_verifier(["--metrics-json", json_path, "--tolerance", "0.30", "--decay-tolerance", "0.20"])
        self.assertEqual(rc, 1)
        self.assertIn("Inkonsistente Abklingkurve", err)

    def test_08_missing_file_exit_2(self):
        """Missing input file must return exit code 2."""
        rc, out, err = self._run_verifier(["--metrics-json", "non_existent_file.json"])
        self.assertEqual(rc, 2)
        self.assertIn("[ERROR]", err)

    def test_09_corrupted_json_exit_2(self):
        """Corrupted JSON syntax must return exit code 2."""
        p = os.path.join(self.tmp_dir.name, "bad.json")
        with open(p, "w") as f:
            f.write("{invalid-json-content")
        rc, out, err = self._run_verifier(["--metrics-json", p])
        self.assertEqual(rc, 2)
        self.assertIn("[ERROR]", err)

    def test_10_missing_args_exit_2(self):
        """Running with no inputs must return exit code 2."""
        rc, out, err = self._run_verifier([])
        self.assertEqual(rc, 2)
        self.assertIn("[ERROR]", err)

    def test_11_t60_boundary_exact_lower_and_upper(self):
        """Boundary values exactly at target T60 +/- tolerance must pass without floating-point false rejection."""
        target = 1.0
        tol = 0.15
        # Exact lower bound: 0.850 s
        data_lower = {
            "t60_target_sec": target,
            "edt_l_sec": 0.850, "edt_r_sec": 0.850,
            "t20_l_sec": 0.850, "t20_r_sec": 0.850,
            "t30_l_sec": 0.850, "t30_r_sec": 0.850
        }
        json_path = self._write_json(data_lower)
        rc, out, err = self._run_verifier(["--metrics-json", json_path, "--tolerance", str(tol)])
        self.assertEqual(rc, 0, f"Exact lower boundary failed: {err}")
        self.assertIn("BESTANDEN", out)

        # Exact upper bound: 1.150 s
        data_upper = {
            "t60_target_sec": target,
            "edt_l_sec": 1.150, "edt_r_sec": 1.150,
            "t20_l_sec": 1.150, "t20_r_sec": 1.150,
            "t30_l_sec": 1.150, "t30_r_sec": 1.150
        }
        json_path = self._write_json(data_upper)
        rc, out, err = self._run_verifier(["--metrics-json", json_path, "--tolerance", str(tol)])
        self.assertEqual(rc, 0, f"Exact upper boundary failed: {err}")
        self.assertIn("BESTANDEN", out)

    def test_12_t60_boundary_inside_epsilon(self):
        """Boundary values just inside target T60 +/- (tolerance - epsilon) must pass."""
        target = 1.2
        tol = 0.15
        # Inside lower bound: -14.99%
        val_low = target * (1.0 - (tol - 1e-4))
        data_lower = {
            "t60_target_sec": target,
            "edt_l_sec": val_low, "edt_r_sec": val_low,
            "t20_l_sec": val_low, "t20_r_sec": val_low,
            "t30_l_sec": val_low, "t30_r_sec": val_low
        }
        json_path = self._write_json(data_lower)
        rc, out, err = self._run_verifier(["--metrics-json", json_path, "--tolerance", str(tol)])
        self.assertEqual(rc, 0, f"Inside lower epsilon failed: {err}")

        # Inside upper bound: +14.99%
        val_high = target * (1.0 + (tol - 1e-4))
        data_upper = {
            "t60_target_sec": target,
            "edt_l_sec": val_high, "edt_r_sec": val_high,
            "t20_l_sec": val_high, "t20_r_sec": val_high,
            "t30_l_sec": val_high, "t30_r_sec": val_high
        }
        json_path = self._write_json(data_upper)
        rc, out, err = self._run_verifier(["--metrics-json", json_path, "--tolerance", str(tol)])
        self.assertEqual(rc, 0, f"Inside upper epsilon failed: {err}")

    def test_13_t60_boundary_outside_epsilon_lower(self):
        """Values just 1e-4 below the lower T60 tolerance must reliably fail with exit 1."""
        target = 1.0
        tol = 0.15
        # Outside lower bound: 0.8499 s (-15.01%)
        val_low = target * (1.0 - tol) - 0.0001
        data = {
            "t60_target_sec": target,
            "edt_l_sec": target, "edt_r_sec": target,
            "t20_l_sec": target, "t20_r_sec": target,
            "t30_l_sec": target, "t30_r_sec": val_low
        }
        json_path = self._write_json(data)
        rc, out, err = self._run_verifier(["--metrics-json", json_path, "--tolerance", str(tol)])
        self.assertEqual(rc, 1)
        self.assertIn("FEHLER: AKUSTISCHE METRIK-PLAUSIBILITÄTSPRÜFUNG FEHLGESCHLAGEN", err)
        self.assertIn("T30_R", err)
        self.assertIn("weicht um", err)
        self.assertIn("vom Zielwert T60", err)

    def test_14_t60_boundary_outside_epsilon_upper(self):
        """Values just 1e-4 above the upper T60 tolerance must reliably fail with exit 1."""
        target = 1.0
        tol = 0.15
        # Outside upper bound: 1.1501 s (+15.01%)
        val_high = target * (1.0 + tol) + 0.0001
        data = {
            "t60_target_sec": target,
            "edt_l_sec": val_high, "edt_r_sec": target,
            "t20_l_sec": target, "t20_r_sec": target,
            "t30_l_sec": target, "t30_r_sec": target
        }
        json_path = self._write_json(data)
        rc, out, err = self._run_verifier(["--metrics-json", json_path, "--tolerance", str(tol)])
        self.assertEqual(rc, 1)
        self.assertIn("FEHLER: AKUSTISCHE METRIK-PLAUSIBILITÄTSPRÜFUNG FEHLGESCHLAGEN", err)
        self.assertIn("EDT_L", err)
        self.assertIn("weicht um", err)
        self.assertIn("vom Zielwert T60", err)

    def test_15_stereo_symmetry_boundary_exact(self):
        """Stereo discrepancy exactly at stereo_tolerance (e.g. 15.0%) must pass without rounding false rejection."""
        # Target = 0.95 s, L = 1.0 s (+5.26%), R = 0.85 s (-10.53%), diff = |1.0 - 0.85|/1.0 = 0.150 = 15.0%
        # Both within target T60 tolerance (15%), exactly at stereo tolerance (15%)
        target = 0.95
        data_lr = {
            "t60_target_sec": target,
            "edt_l_sec": 1.000, "edt_r_sec": 0.850,
            "t20_l_sec": 1.000, "t20_r_sec": 0.850,
            "t30_l_sec": 1.000, "t30_r_sec": 0.850
        }
        json_path = self._write_json(data_lr)
        rc, out, err = self._run_verifier([
            "--metrics-json", json_path,
            "--tolerance", "0.15",
            "--stereo-tolerance", "0.15",
            "--decay-tolerance", "0.20"
        ])
        self.assertEqual(rc, 0, f"Exact stereo tolerance L/R failed: {err}")
        self.assertIn("BESTANDEN", out)

        # Reverse R/L: L = 0.850, R = 1.000
        data_rl = {
            "t60_target_sec": target,
            "edt_l_sec": 0.850, "edt_r_sec": 1.000,
            "t20_l_sec": 0.850, "t20_r_sec": 1.000,
            "t30_l_sec": 0.850, "t30_r_sec": 1.000
        }
        json_path = self._write_json(data_rl)
        rc, out, err = self._run_verifier([
            "--metrics-json", json_path,
            "--tolerance", "0.15",
            "--stereo-tolerance", "0.15",
            "--decay-tolerance", "0.20"
        ])
        self.assertEqual(rc, 0, f"Exact stereo tolerance R/L failed: {err}")
        self.assertIn("BESTANDEN", out)

    def test_16_stereo_symmetry_boundary_inside_epsilon(self):
        """Stereo discrepancy just inside tolerance (14.99%) must pass."""
        target = 0.95
        # L = 1.000, R = 0.8501 -> diff = 0.1499 = 14.99%
        data = {
            "t60_target_sec": target,
            "edt_l_sec": 1.000, "edt_r_sec": 0.8501,
            "t20_l_sec": 1.000, "t20_r_sec": 0.8501,
            "t30_l_sec": 1.000, "t30_r_sec": 0.8501
        }
        json_path = self._write_json(data)
        rc, out, err = self._run_verifier([
            "--metrics-json", json_path,
            "--tolerance", "0.15",
            "--stereo-tolerance", "0.15"
        ])
        self.assertEqual(rc, 0, f"Inside stereo epsilon failed: {err}")

    def test_17_stereo_symmetry_boundary_outside_epsilon(self):
        """Stereo discrepancy just 1e-4 outside tolerance (15.02%) must fail with Stereo-Asymmetrie violation."""
        target = 0.95
        # L = 1.000, R = 0.8498 -> diff = 0.1502 = 15.02% > 15.0%
        # Test on EDT
        data_edt = {
            "t60_target_sec": target,
            "edt_l_sec": 1.000, "edt_r_sec": 0.8498,
            "t20_l_sec": 0.950, "t20_r_sec": 0.950,
            "t30_l_sec": 0.950, "t30_r_sec": 0.950
        }
        json_path = self._write_json(data_edt)
        rc, out, err = self._run_verifier([
            "--metrics-json", json_path,
            "--tolerance", "0.15",
            "--stereo-tolerance", "0.15"
        ])
        self.assertEqual(rc, 1)
        self.assertIn("Stereo-Asymmetrie bei EDT", err)

        # Test on T20
        data_t20 = {
            "t60_target_sec": target,
            "edt_l_sec": 0.950, "edt_r_sec": 0.950,
            "t20_l_sec": 1.000, "t20_r_sec": 0.8498,
            "t30_l_sec": 0.950, "t30_r_sec": 0.950
        }
        json_path = self._write_json(data_t20)
        rc, out, err = self._run_verifier([
            "--metrics-json", json_path,
            "--tolerance", "0.15",
            "--stereo-tolerance", "0.15"
        ])
        self.assertEqual(rc, 1)
        self.assertIn("Stereo-Asymmetrie bei T20", err)

        # Test on T30
        data_t30 = {
            "t60_target_sec": target,
            "edt_l_sec": 0.950, "edt_r_sec": 0.950,
            "t20_l_sec": 0.950, "t20_r_sec": 0.950,
            "t30_l_sec": 1.000, "t30_r_sec": 0.8498
        }
        json_path = self._write_json(data_t30)
        rc, out, err = self._run_verifier([
            "--metrics-json", json_path,
            "--tolerance", "0.15",
            "--stereo-tolerance", "0.15"
        ])
        self.assertEqual(rc, 1)
        self.assertIn("Stereo-Asymmetrie bei T30", err)

    def test_18_decay_curve_boundary_exact(self):
        """Decay profile consistency (EDT vs T20 and T20 vs T30) exactly at decay_tolerance must pass."""
        target = 1.0
        # EDT = 1.200, T20 = 1.000, T30 = 1.000 -> |1.2 - 1.0|/1.0 = 0.200 (exact 20.0%)
        data_edt_upper = {
            "t60_target_sec": target,
            "edt_l_sec": 1.200, "edt_r_sec": 1.200,
            "t20_l_sec": 1.000, "t20_r_sec": 1.000,
            "t30_l_sec": 1.000, "t30_r_sec": 1.000
        }
        json_path = self._write_json(data_edt_upper)
        rc, out, err = self._run_verifier([
            "--metrics-json", json_path,
            "--tolerance", "0.25",
            "--decay-tolerance", "0.20"
        ])
        self.assertEqual(rc, 0, f"Exact EDT upper decay boundary failed: {err}")
        self.assertIn("BESTANDEN", out)

        # EDT = 0.800, T20 = 1.000, T30 = 1.000 -> |0.8 - 1.0|/1.0 = 0.200 (exact 20.0%)
        data_edt_lower = {
            "t60_target_sec": target,
            "edt_l_sec": 0.800, "edt_r_sec": 0.800,
            "t20_l_sec": 1.000, "t20_r_sec": 1.000,
            "t30_l_sec": 1.000, "t30_r_sec": 1.000
        }
        json_path = self._write_json(data_edt_lower)
        rc, out, err = self._run_verifier([
            "--metrics-json", json_path,
            "--tolerance", "0.25",
            "--decay-tolerance", "0.20"
        ])
        self.assertEqual(rc, 0, f"Exact EDT lower decay boundary failed: {err}")

        # T20 = 1.200, T30 = 1.000 -> |1.2 - 1.0|/1.0 = 0.200 (exact 20.0%)
        data_t20_upper = {
            "t60_target_sec": target,
            "edt_l_sec": 1.200, "edt_r_sec": 1.200,
            "t20_l_sec": 1.200, "t20_r_sec": 1.200,
            "t30_l_sec": 1.000, "t30_r_sec": 1.000
        }
        json_path = self._write_json(data_t20_upper)
        rc, out, err = self._run_verifier([
            "--metrics-json", json_path,
            "--tolerance", "0.25",
            "--decay-tolerance", "0.20"
        ])
        self.assertEqual(rc, 0, f"Exact T20 vs T30 upper decay boundary failed: {err}")

    def test_19_decay_curve_boundary_inside_epsilon(self):
        """Decay divergence just inside tolerance (19.99%) must pass."""
        target = 1.0
        # EDT = 1.1999, T20 = 1.000 -> 19.99% divergence
        data = {
            "t60_target_sec": target,
            "edt_l_sec": 1.1999, "edt_r_sec": 1.1999,
            "t20_l_sec": 1.000, "t20_r_sec": 1.000,
            "t30_l_sec": 1.000, "t30_r_sec": 1.000
        }
        json_path = self._write_json(data)
        rc, out, err = self._run_verifier([
            "--metrics-json", json_path,
            "--tolerance", "0.25",
            "--decay-tolerance", "0.20"
        ])
        self.assertEqual(rc, 0, f"Inside decay epsilon failed: {err}")

    def test_20_decay_curve_boundary_outside_epsilon(self):
        """Decay divergence just 1e-4 outside tolerance (20.02%) must fail with Inkonsistente Abklingkurve."""
        target = 1.0
        # EDT = 1.2002, T20 = 1.000 -> 20.02% divergence > 20.0%
        data_edt = {
            "t60_target_sec": target,
            "edt_l_sec": 1.2002, "edt_r_sec": 1.000,
            "t20_l_sec": 1.000, "t20_r_sec": 1.000,
            "t30_l_sec": 1.000, "t30_r_sec": 1.000
        }
        json_path = self._write_json(data_edt)
        rc, out, err = self._run_verifier([
            "--metrics-json", json_path,
            "--tolerance", "0.25",
            "--decay-tolerance", "0.20"
        ])
        self.assertEqual(rc, 1)
        self.assertIn("Inkonsistente Abklingkurve Kanal Links", err)
        self.assertIn("Diskrepanz zwischen EDT", err)

        # T20 = 1.2002, T30 = 1.000 -> 20.02% divergence
        data_t20 = {
            "t60_target_sec": target,
            "edt_l_sec": 1.200, "edt_r_sec": 1.000,
            "t20_l_sec": 1.000, "t20_r_sec": 1.2002,
            "t30_l_sec": 1.000, "t30_r_sec": 1.000
        }
        json_path = self._write_json(data_t20)
        rc, out, err = self._run_verifier([
            "--metrics-json", json_path,
            "--tolerance", "0.25",
            "--decay-tolerance", "0.20"
        ])
        self.assertEqual(rc, 1)
        self.assertIn("Inkonsistente Abklingkurve Kanal Rechts", err)
        self.assertIn("Diskrepanz zwischen T20", err)

    def test_21_compound_boundary_all_three_criteria_at_limit(self):
        """Simultaneous placement of all three criteria on their exact respective boundaries must pass."""
        data = {
            "t60_target_sec": 1.0,
            "edt_l_sec": 1.150, "edt_r_sec": 1.000,
            "t20_l_sec": 1.000, "t20_r_sec": 0.850,
            "t30_l_sec": 0.850, "t30_r_sec": 0.850
        }
        json_path = self._write_json(data)
        rc, out, err = self._run_verifier([
            "--metrics-json", json_path,
            "--tolerance", "0.15",
            "--stereo-tolerance", "0.15",
            "--decay-tolerance", "0.20"
        ])
        self.assertEqual(rc, 0, f"Compound simultaneous boundary test failed: {err}")
        self.assertIn("BESTANDEN", out)

    def test_22_scale_invariance_across_t60_ranges(self):
        """Boundary logic must remain invariant across diverse acoustic spaces (0.2s, 1.2s, 4.5s)."""
        tol = 0.15
        for target in [0.20, 1.20, 4.50]:
            val_exact_lower = target * (1.0 - tol)
            val_exact_upper = target * (1.0 + tol)
            
            data_pass = {
                "t60_target_sec": target,
                "edt_l_sec": val_exact_lower, "edt_r_sec": val_exact_upper,
                "t20_l_sec": target, "t20_r_sec": target,
                "t30_l_sec": target, "t30_r_sec": target
            }
            json_path = self._write_json(data_pass)
            rc, out, err = self._run_verifier([
                "--metrics-json", json_path,
                "--tolerance", str(tol),
                "--stereo-tolerance", "0.30",
                "--decay-tolerance", "0.30"
            ])
            self.assertEqual(rc, 0, f"Scale invariance failed for target {target}s: {err}")

            # Slightly below lower bound: must fail
            data_fail = {
                "t60_target_sec": target,
                "edt_l_sec": val_exact_lower - 0.0001, "edt_r_sec": target,
                "t20_l_sec": target, "t20_r_sec": target,
                "t30_l_sec": target, "t30_r_sec": target
            }
            json_path = self._write_json(data_fail)
            rc, out, err = self._run_verifier([
                "--metrics-json", json_path,
                "--tolerance", str(tol)
            ])
            self.assertEqual(rc, 1, f"Expected failure below boundary for target {target}s")

if __name__ == "__main__":
    unittest.main()
