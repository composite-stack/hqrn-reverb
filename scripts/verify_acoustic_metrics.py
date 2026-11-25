#!/usr/bin/env python3
"""
HQRN Audio DSP - Acoustic Metrics Plausibility Verifier
Validates Schroeder decay times (EDT, T20, T30) and level metrics for acoustic plausibility.
Ensures decay times conform to target T60 bounds, exhibit stereo symmetry, and preserve
monotone decay curvature. Emits detailed diagnostic output when invalid.
"""

import sys
import os
import json
import math
import struct
import wave
import argparse
from typing import Dict, Any, List, Tuple, Optional

def linear_regression(x: List[float], y: List[float]) -> Optional[Tuple[float, float]]:
    n = len(x)
    if n < 2 or n != len(y):
        return None
    sum_x = sum(x)
    sum_y = sum(y)
    sum_xy = sum(xi * yi for xi, yi in zip(x, y))
    sum_xx = sum(xi * xi for xi in x)
    denom = n * sum_xx - sum_x * sum_x
    if abs(denom) < 1e-12:
        return None
    slope = (n * sum_xy - sum_x * sum_y) / denom
    intercept = (sum_y - slope * sum_x) / n
    return slope, intercept

def evaluate_schroeder_decay(samples: List[float], sample_rate: float) -> Tuple[float, float, float]:
    """
    Computes EDT (0 to -10 dB), T20 (-5 to -25 dB), T30 (-5 to -35 dB) via
    Schroeder backward energy integration and linear regression.
    Returns (-1.0, -1.0, -1.0) on failure or insufficient duration.
    """
    n = len(samples)
    if n < int(sample_rate * 0.1):
        return -1.0, -1.0, -1.0

    energy = [0.0] * n
    accum = 0.0
    for i in range(n - 1, -1, -1):
        accum += samples[i] * samples[i]
        energy[i] = accum

    total_energy = energy[0]
    if total_energy <= 1e-12:
        return -1.0, -1.0, -1.0

    edc_db = [
        10.0 * math.log10(e / total_energy) if e > 1e-12 else -120.0
        for e in energy
    ]

    def fit_range(start_db: float, end_db: float) -> float:
        t_fit = []
        db_fit = []
        for i in range(n):
            val = edc_db[i]
            if start_db >= val >= end_db:
                t_fit.append(i / sample_rate)
                db_fit.append(val)
        if len(t_fit) < 20:
            return -1.0
        res = linear_regression(t_fit, db_fit)
        if not res:
            return -1.0
        slope, _ = res
        if slope >= -1e-4:
            return -1.0
        return float(-60.0 / slope)

    edt = fit_range(0.0, -10.0)
    t20 = fit_range(-5.0, -25.0)
    t30 = fit_range(-5.0, -35.0)
    return edt, t20, t30

def read_wav_stereo(path: str) -> Tuple[List[float], List[float], int, float]:
    """Reads stereo WAV file and returns (left, right, sample_rate, duration_sec)."""
    with wave.open(path, 'rb') as w:
        n_channels = w.getnchannels()
        sampwidth = w.getsampwidth()
        sample_rate = w.getframerate()
        n_frames = w.getnframes()
        raw_bytes = w.readframes(n_frames)

    duration_sec = n_frames / sample_rate if sample_rate > 0 else 0.0
    left = []
    right = []

    if sampwidth == 2:  # 16-bit PCM
        fmt = f"<{n_frames * n_channels}h"
        samples = struct.unpack(fmt, raw_bytes)
        scale = 32768.0
        if n_channels == 1:
            left = [s / scale for s in samples]
            right = list(left)
        elif n_channels == 2:
            left = [samples[i] / scale for i in range(0, len(samples), 2)]
            right = [samples[i + 1] / scale for i in range(0, len(samples), 2)]
    elif sampwidth == 3:  # 24-bit PCM
        scale = 8388608.0
        step = 3 * n_channels
        for i in range(0, len(raw_bytes), step):
            b_l = raw_bytes[i:i+3]
            val_l = int.from_bytes(b_l, byteorder='little', signed=True) / scale
            left.append(val_l)
            if n_channels >= 2:
                b_r = raw_bytes[i+3:i+6]
                val_r = int.from_bytes(b_r, byteorder='little', signed=True) / scale
                right.append(val_r)
            else:
                right.append(val_l)
    elif sampwidth == 4:  # 32-bit float or PCM
        fmt = f"<{n_frames * n_channels}f"
        try:
            samples = struct.unpack(fmt, raw_bytes)
            if n_channels == 1:
                left = list(samples)
                right = list(samples)
            elif n_channels == 2:
                left = [samples[i] for i in range(0, len(samples), 2)]
                right = [samples[i + 1] for i in range(0, len(samples), 2)]
        except Exception:
            # Fallback for 32-bit integer PCM
            fmt_i = f"<{n_frames * n_channels}i"
            samples_i = struct.unpack(fmt_i, raw_bytes)
            scale = 2147483648.0
            if n_channels == 1:
                left = [s / scale for s in samples_i]
                right = list(left)
            elif n_channels == 2:
                left = [samples_i[i] / scale for i in range(0, len(samples_i), 2)]
                right = [samples_i[i + 1] / scale for i in range(0, len(samples_i), 2)]

    return left, right, sample_rate, duration_sec

# Numerical tolerance epsilon to guard against IEEE 754 floating point representation artifacts at exact boundaries
FLOAT_TOL_EPS = 1e-9

def verify_plausibility(
    metrics: Dict[str, Any],
    target_t60: float,
    tolerance: float = 0.15,
    stereo_tolerance: float = 0.15,
    decay_tolerance: float = 0.20
) -> Tuple[bool, List[str], Dict[str, Any]]:
    """
    Performs comprehensive acoustic plausibility checks on EDT, T20, and T30.
    Returns (is_valid, list_of_violations, details_dict).
    """
    violations = []
    details = {}

    edt_l = float(metrics.get("edt_l_sec", -1.0))
    edt_r = float(metrics.get("edt_r_sec", -1.0))
    t20_l = float(metrics.get("t20_l_sec", -1.0))
    t20_r = float(metrics.get("t20_r_sec", -1.0))
    t30_l = float(metrics.get("t30_l_sec", -1.0))
    t30_r = float(metrics.get("t30_r_sec", -1.0))

    details["target_t60"] = target_t60
    details["tolerance"] = tolerance
    details["stereo_tolerance"] = stereo_tolerance
    details["decay_tolerance"] = decay_tolerance
    details["values"] = {
        "edt_l": edt_l, "edt_r": edt_r,
        "t20_l": t20_l, "t20_r": t20_r,
        "t30_l": t30_l, "t30_r": t30_r
    }

    # 1. Positivity and numerical finiteness check
    metric_map = {
        "EDT (Links)": edt_l,
        "EDT (Rechts)": edt_r,
        "T20 (Links)": t20_l,
        "T20 (Rechts)": t20_r,
        "T30 (Links)": t30_l,
        "T30 (Rechts)": t30_r
    }

    for name, val in metric_map.items():
        if math.isnan(val) or math.isinf(val):
            violations.append(f"{name} ist nicht-finit ({val}). Numerische Instabilität oder NaN im Renderer.")
        elif val <= 0.0:
            violations.append(
                f"{name} konnte nicht bestimmt werden ({val:.3f} s <= 0). "
                f"Ursache: Audio zu kurz (< T60 Abklingzeit), Pegel unter Messbereich oder fehlerhafter EDC-Fit."
            )

    # If any metric is not positive/finite, stop further relative comparison
    if violations:
        return False, violations, details

    # 2. Target T60 Deviation Check
    deviations = {}
    for name, val, key in [
        ("EDT_L", edt_l, "err_edt_l"),
        ("EDT_R", edt_r, "err_edt_r"),
        ("T20_L", t20_l, "err_t20_l"),
        ("T20_R", t20_r, "err_t20_r"),
        ("T30_L", t30_l, "err_t30_l"),
        ("T30_R", t30_r, "err_t30_r"),
    ]:
        rel_err = (val - target_t60) / target_t60
        pct_err = rel_err * 100.0
        deviations[key] = pct_err
        if abs(rel_err) > (tolerance + FLOAT_TOL_EPS):
            violations.append(
                f"{name} ({val:.3f} s) weicht um {pct_err:+.1f}% vom Zielwert T60 ({target_t60:.3f} s) ab "
                f"(zulässige Toleranz: ±{tolerance * 100:.1f}%)."
            )
    details["deviations"] = deviations

    # 3. Stereo Decay Symmetry Check (L vs R consistency)
    def check_stereo_sym(val_l: float, val_r: float, label: str):
        denom = max(val_l, val_r)
        if denom > 1e-6:
            diff_rel = abs(val_l - val_r) / denom
            details[f"sym_{label.lower()}"] = diff_rel * 100.0
            if diff_rel > (stereo_tolerance + FLOAT_TOL_EPS):
                violations.append(
                    f"Stereo-Asymmetrie bei {label}: L={val_l:.3f} s, R={val_r:.3f} s "
                    f"ergibt {diff_rel * 100.0:.1f}% Differenz (zulässig: <= {stereo_tolerance * 100:.1f}%)."
                )

    check_stereo_sym(edt_l, edt_r, "EDT")
    check_stereo_sym(t20_l, t20_r, "T20")
    check_stereo_sym(t30_l, t30_r, "T30")

    # 4. Decay Profile Consistency (EDT vs T20 vs T30 Monotonicity & Curvature)
    def check_decay_curve(edt: float, t20: float, t30: float, ch_name: str):
        # EDT should not wildly differ from T20
        diff_edt_t20 = abs(edt - t20) / t20
        if diff_edt_t20 > (decay_tolerance + FLOAT_TOL_EPS):
            violations.append(
                f"Inkonsistente Abklingkurve {ch_name}: Diskrepanz zwischen EDT ({edt:.3f} s) und T20 ({t20:.3f} s) "
                f"beträgt {diff_edt_t20 * 100.0:.1f}% (zulässig: <= {decay_tolerance * 100:.1f}%)."
            )
        # T20 should not wildly differ from T30
        diff_t20_t30 = abs(t20 - t30) / t30
        if diff_t20_t30 > (decay_tolerance + FLOAT_TOL_EPS):
            violations.append(
                f"Inkonsistente Abklingkurve {ch_name}: Diskrepanz zwischen T20 ({t20:.3f} s) und T30 ({t30:.3f} s) "
                f"beträgt {diff_t20_t30 * 100.0:.1f}% (zulässig: <= {decay_tolerance * 100:.1f}%)."
            )

    check_decay_curve(edt_l, t20_l, t30_l, "Kanal Links")
    check_decay_curve(edt_r, t20_r, t30_r, "Kanal Rechts")

    is_valid = (len(violations) == 0)
    return is_valid, violations, details

def format_invalid_report(
    metrics: Dict[str, Any],
    violations: List[str],
    details: Dict[str, Any],
    source_name: str = ""
) -> str:
    """Formats a comprehensive diagnostic error report for invalid acoustic results."""
    lines = []
    lines.append("================================================================================")
    lines.append("FEHLER: AKUSTISCHE METRIK-PLAUSIBILITÄTSPRÜFUNG FEHLGESCHLAGEN")
    lines.append("================================================================================")
    target_t60 = details.get("target_t60", 0.0)
    tol = details.get("tolerance", 0.15) * 100.0
    s_tol = details.get("stereo_tolerance", 0.15) * 100.0
    d_tol = details.get("decay_tolerance", 0.20) * 100.0

    lines.append("Konfiguration & Grenzwerte:")
    lines.append(f"  - Ziel-T60 (Nachhallzeit):  {target_t60:.3f} s (Zulässiger Bereich: ±{tol:.1f}%)")
    lines.append(f"  - Max. Stereo-Asymmetrie:   {s_tol:.1f}%")
    lines.append(f"  - Max. Decay-Divergenz:     {d_tol:.1f}% (EDT vs T20 vs T30)")
    if source_name:
        lines.append(f"  - Quelle / Eingabedatei:    {source_name}")
    lines.append("")

    lines.append("Gemessene Abklingwerte & Abweichungen:")
    lines.append("  " + "-" * 74)
    lines.append("  Metrik   Kanal Links    Abw. (%)  Status      Kanal Rechts   Abw. (%)  Status")
    lines.append("  " + "-" * 74)

    vals = details.get("values", {})
    devs = details.get("deviations", {})

    def row(label, l_key, r_key, dev_l_key, dev_r_key):
        val_l = vals.get(l_key, -1.0)
        val_r = vals.get(r_key, -1.0)
        
        # status L
        if val_l <= 0.0 or math.isnan(val_l):
            stat_l = "UNGÜLTIG "
            txt_l = f"{val_l:7.3f} s" if not math.isnan(val_l) else "   NaN s"
            dev_l_txt = "  N/A  "
        else:
            dev_l = devs.get(dev_l_key, 0.0)
            dev_l_txt = f"{dev_l:+6.1f}%"
            stat_l = "GÜLTIG   " if abs(dev_l) <= (tol + 1e-7) else "ABWEICHUNG"
            txt_l = f"{val_l:7.3f} s"

        # status R
        if val_r <= 0.0 or math.isnan(val_r):
            stat_r = "UNGÜLTIG "
            txt_r = f"{val_r:7.3f} s" if not math.isnan(val_r) else "   NaN s"
            dev_r_txt = "  N/A  "
        else:
            dev_r = devs.get(dev_r_key, 0.0)
            dev_r_txt = f"{dev_r:+6.1f}%"
            stat_r = "GÜLTIG   " if abs(dev_r) <= (tol + 1e-7) else "ABWEICHUNG"
            txt_r = f"{val_r:7.3f} s"

        return f"  {label:<8} {txt_l}    {dev_l_txt:<8}  {stat_l:<10}  {txt_r}    {dev_r_txt:<8}  {stat_r}"

    lines.append(row("EDT", "edt_l", "edt_r", "err_edt_l", "err_edt_r"))
    lines.append(row("T20", "t20_l", "t20_r", "err_t20_l", "err_t20_r"))
    lines.append(row("T30", "t30_l", "t30_r", "err_t30_l", "err_t30_r"))
    lines.append("  " + "-" * 74)
    lines.append("")

    lines.append(f"Identifizierte Plausibilitätsfehler ({len(violations)}):")
    for idx, v in enumerate(violations, 1):
        lines.append(f"  [{idx}] {v}")
    lines.append("")

    lines.append("Signaltheoretischer Kontext:")
    if "peak_l_dbfs" in metrics:
        lines.append(f"  - Peak Pegel:               L: {metrics.get('peak_l_dbfs'):.2f} dBFS  |  R: {metrics.get('peak_r_dbfs'):.2f} dBFS")
    if "rms_l_dbfs" in metrics:
        lines.append(f"  - RMS Pegel:                L: {metrics.get('rms_l_dbfs'):.2f} dBFS  |  R: {metrics.get('rms_r_dbfs'):.2f} dBFS")
    if "duration_sec" in metrics:
        lines.append(f"  - Audiodauer:               {metrics.get('duration_sec'):.2f} s ({metrics.get('sample_rate', 48000)} Hz)")
    if "clipped" in metrics:
        lines.append(f"  - True-Peak Clipper Status: {'Intervention / Begrenzung' if metrics.get('clipped') else 'Bit-genau linear'}")
    lines.append("================================================================================")
    return "\n".join(lines)

def format_valid_report(metrics: Dict[str, Any], details: Dict[str, Any], source_name: str = "") -> str:
    """Formats a concise success report when all acoustic metrics are valid."""
    lines = []
    lines.append("================================================================================")
    lines.append("AKUSTISCHE METRIK-PLAUSIBILITÄTSPRÜFUNG BESTANDEN")
    lines.append("================================================================================")
    target_t60 = details.get("target_t60", 0.0)
    tol = details.get("tolerance", 0.15) * 100.0
    vals = details.get("values", {})
    devs = details.get("deviations", {})

    lines.append(f"Ziel-T60: {target_t60:.3f} s (Toleranzfenster: ±{tol:.1f}%)")
    if source_name:
        lines.append(f"Quelle:   {source_name}")
    lines.append("Verifizierte Abklingzeiten (Schroeder-Rückwärtsintegration):")
    lines.append(
        f"  - EDT (Early Decay):  L: {vals.get('edt_l', 0):.3f} s ({devs.get('err_edt_l', 0):+5.1f}%) | "
        f"R: {vals.get('edt_r', 0):.3f} s ({devs.get('err_edt_r', 0):+5.1f}%) -> OK"
    )
    lines.append(
        f"  - T20 (-5 bis -25dB): L: {vals.get('t20_l', 0):.3f} s ({devs.get('err_t20_l', 0):+5.1f}%) | "
        f"R: {vals.get('t20_r', 0):.3f} s ({devs.get('err_t20_r', 0):+5.1f}%) -> OK"
    )
    lines.append(
        f"  - T30 (-5 bis -35dB): L: {vals.get('t30_l', 0):.3f} s ({devs.get('err_t30_l', 0):+5.1f}%) | "
        f"R: {vals.get('t30_r', 0):.3f} s ({devs.get('err_t30_r', 0):+5.1f}%) -> OK"
    )
    lines.append("Plausibilitätskriterien:")
    lines.append(f"  - Positivität & Finitheit:   Alle 6 Metriken > 0 und endlich -> OK")
    lines.append(f"  - Stereo-Konsistenz:         L/R-Abweichung <= {details.get('stereo_tolerance', 0.15)*100:.1f}% -> OK")
    lines.append(f"  - Decay-Profil Monotonie:    EDT/T20/T30-Divergenz <= {details.get('decay_tolerance', 0.20)*100:.1f}% -> OK")
    lines.append("================================================================================")
    return "\n".join(lines)

def main():
    parser = argparse.ArgumentParser(description="Verifies acoustic metrics (EDT, T20, T30) for plausibility.")
    parser.add_argument("--metrics-json", type=str, help="Path to JSON metrics file exported by hqrn_render")
    parser.add_argument("--wav", type=str, help="Path to WAV file to evaluate directly")
    parser.add_argument("--target-t60", type=float, help="Target T60 in seconds")
    parser.add_argument("--tolerance", type=float, default=0.15, help="Max relative error vs target T60 (default: 0.15 = 15%%)")
    parser.add_argument("--stereo-tolerance", type=float, default=0.15, help="Max relative stereo discrepancy (default: 0.15 = 15%%)")
    parser.add_argument("--decay-tolerance", type=float, default=0.20, help="Max relative curvature divergence between EDT, T20, T30 (default: 0.20 = 20%%)")
    parser.add_argument("--report-json", type=str, help="Optional path to output machine-readable verification report")

    args = parser.parse_args()

    if not args.metrics_json and not args.wav:
        sys.stderr.write("[ERROR] Mindestens eines der Argumente --metrics-json oder --wav muss angegeben werden.\n")
        sys.exit(2)

    metrics: Dict[str, Any] = {}
    source_name = ""

    # Load JSON if provided
    if args.metrics_json:
        if not os.path.isfile(args.metrics_json):
            sys.stderr.write(f"[ERROR] Metrik-JSON-Datei nicht gefunden: {args.metrics_json}\n")
            sys.exit(2)
        try:
            with open(args.metrics_json, 'r', encoding='utf-8') as f:
                metrics = json.load(f)
            source_name = args.metrics_json
        except Exception as e:
            sys.stderr.write(f"[ERROR] Fehler beim Parsen von {args.metrics_json}: {e}\n")
            sys.exit(2)

    # If WAV is provided, compute or cross-validate
    if args.wav:
        if not os.path.isfile(args.wav):
            sys.stderr.write(f"[ERROR] WAV-Datei nicht gefunden: {args.wav}\n")
            sys.exit(2)
        try:
            left, right, sr, dur = read_wav_stereo(args.wav)
            edt_l, t20_l, t30_l = evaluate_schroeder_decay(left, float(sr))
            edt_r, t20_r, t30_r = evaluate_schroeder_decay(right, float(sr))

            wav_metrics = {
                "sample_rate": sr,
                "duration_sec": dur,
                "edt_l_sec": edt_l,
                "edt_r_sec": edt_r,
                "t20_l_sec": t20_l,
                "t20_r_sec": t20_r,
                "t30_l_sec": t30_l,
                "t30_r_sec": t30_r
            }
            if not metrics:
                metrics = wav_metrics
                source_name = args.wav
            else:
                # Store cross-validation
                metrics["wav_crosscheck"] = wav_metrics
                source_name = f"{args.metrics_json} + {args.wav}"
        except Exception as e:
            sys.stderr.write(f"[ERROR] Fehler bei der WAV-Analyse von {args.wav}: {e}\n")
            sys.exit(2)

    # Determine target T60
    target_t60 = args.target_t60
    if target_t60 is None:
        target_t60 = float(metrics.get("t60_target_sec", 0.0))
    if target_t60 <= 0.0:
        sys.stderr.write("[ERROR] Ungültiger oder fehlender Ziel-T60-Wert. Bitte --target-t60 angeben oder in JSON definieren.\n")
        sys.exit(2)

    is_valid, violations, details = verify_plausibility(
        metrics=metrics,
        target_t60=target_t60,
        tolerance=args.tolerance,
        stereo_tolerance=args.stereo_tolerance,
        decay_tolerance=args.decay_tolerance
    )

    if args.report_json:
        report_data = {
            "is_valid": is_valid,
            "target_t60": target_t60,
            "violations": violations,
            "details": details,
            "raw_metrics": metrics
        }
        with open(args.report_json, 'w', encoding='utf-8') as rf:
            json.dump(report_data, rf, indent=2)

    if not is_valid:
        report = format_invalid_report(metrics, violations, details, source_name)
        sys.stderr.write(report + "\n")
        sys.exit(1)
    else:
        report = format_valid_report(metrics, details, source_name)
        sys.stdout.write(report + "\n")
        sys.exit(0)

if __name__ == "__main__":
    main()
