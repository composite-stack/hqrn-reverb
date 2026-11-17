#!/usr/bin/env python3
"""
HQRN CI - Diagnostic Metadata Consistency Verifier
Verifies that Run-Attempt, Trigger-Event, and Git-Ref are present and strictly
consistent across:
  1. diagnostic_metadata.txt
  2. metadata.json
  3. step_summary.md
"""

import os
import sys
import json
import re

def main():
    target_dir = os.environ.get('DIAGNOSTICS_DIR', 'sanitizer_diagnostics')
    exp_attempt = str(os.environ.get('EXP_RUN_ATTEMPT', '')).strip()
    exp_event = str(os.environ.get('EXP_TRIGGER_EVENT', '')).strip()
    exp_ref = str(os.environ.get('EXP_GIT_REF', '')).strip()
    exp_ref_name = str(os.environ.get('EXP_GIT_REF_NAME', '')).strip()

    print('======================================================================')
    print('HQRN CI - DIAGNOSTIC METADATA CONSISTENCY CHECK')
    print('======================================================================')
    print(f'Target Directory:     {target_dir}')
    print(f'Expected Run Attempt: {exp_attempt}')
    print(f'Expected Event:       {exp_event}')
    print(f'Expected Git Ref:     {exp_ref}')
    if exp_ref_name:
        print(f'Expected Ref Name:    {exp_ref_name}')
    print('----------------------------------------------------------------------')

    if not exp_attempt or not exp_event or not exp_ref:
        print('[ERROR] Missing one or more expected environment variables (EXP_RUN_ATTEMPT, EXP_TRIGGER_EVENT, EXP_GIT_REF)')
        sys.exit(2)

    errors = []

    # 1. Verify metadata.json
    json_path = os.path.join(target_dir, 'metadata.json')
    if not os.path.isfile(json_path):
        errors.append(f'Missing file: {json_path}')
    else:
        try:
            with open(json_path, 'r', encoding='utf-8') as f:
                meta = json.load(f)

            if not isinstance(meta, dict):
                errors.append(f'{json_path}: root element must be a JSON object')
            else:
                json_attempt = str(meta.get('workflow_run_attempt', '')).strip()
                json_event = str(meta.get('trigger_event', '')).strip()
                json_ref = str(meta.get('git_ref', '')).strip()

                if not json_attempt:
                    errors.append(f'{json_path}: key "workflow_run_attempt" is missing or empty')
                elif json_attempt != exp_attempt:
                    errors.append(f'{json_path}: workflow_run_attempt mismatch (got "{json_attempt}", expected "{exp_attempt}")')

                if not json_event:
                    errors.append(f'{json_path}: key "trigger_event" is missing or empty')
                elif json_event != exp_event:
                    errors.append(f'{json_path}: trigger_event mismatch (got "{json_event}", expected "{exp_event}")')

                if not json_ref:
                    errors.append(f'{json_path}: key "git_ref" is missing or empty')
                elif json_ref != exp_ref:
                    errors.append(f'{json_path}: git_ref mismatch (got "{json_ref}", expected "{exp_ref}")')

            if not any(e.startswith(json_path) for e in errors):
                print(f'[OK] Verified {json_path}')
        except Exception as e:
            errors.append(f'{json_path} parse error: {e}')

    # 2. Verify diagnostic_metadata.txt
    txt_path = os.path.join(target_dir, 'diagnostic_metadata.txt')
    if not os.path.isfile(txt_path):
        errors.append(f'Missing file: {txt_path}')
    else:
        try:
            with open(txt_path, 'r', encoding='utf-8') as f:
                content = f.read()

            m_att = re.search(r'^Workflow Run Attempt:\s*(\d+)', content, re.MULTILINE)
            if not m_att:
                errors.append(f'{txt_path}: "Workflow Run Attempt: <num>" header not found')
            elif m_att.group(1).strip() != exp_attempt:
                errors.append(f'{txt_path}: Workflow Run Attempt mismatch (got "{m_att.group(1)}", expected "{exp_attempt}")')

            m_ev = re.search(r'^Trigger Event:\s*(\S+)', content, re.MULTILINE)
            if not m_ev:
                errors.append(f'{txt_path}: "Trigger Event: <name>" header not found')
            elif m_ev.group(1).strip() != exp_event:
                errors.append(f'{txt_path}: Trigger Event mismatch (got "{m_ev.group(1)}", expected "{exp_event}")')

            m_ref = re.search(r'^Git Ref:\s*(\S+)', content, re.MULTILINE)
            if not m_ref:
                errors.append(f'{txt_path}: "Git Ref: <ref>" header not found')
            elif m_ref.group(1).strip() != exp_ref:
                errors.append(f'{txt_path}: Git Ref mismatch (got "{m_ref.group(1)}", expected "{exp_ref}")')

            if not any(e.startswith(txt_path) for e in errors):
                print(f'[OK] Verified {txt_path}')
        except Exception as e:
            errors.append(f'{txt_path} read error: {e}')

    # 3. Verify step_summary.md
    sum_path = os.path.join(target_dir, 'step_summary.md')
    if not os.path.isfile(sum_path):
        errors.append(f'Missing file: {sum_path}')
    else:
        try:
            with open(sum_path, 'r', encoding='utf-8') as f:
                summary = f.read()

            # Verify Run Attempt
            m_sum_att = re.search(r'\|\s*\*\*Run Attempt [^|]*\*\*\s*\|\s*\*\*Versuch #(\d+)\*\*', summary)
            if not m_sum_att:
                errors.append(f'{sum_path}: Run Attempt row "| **Run Attempt (Wiederholung)** | **Versuch #<num>** |" not found')
            elif m_sum_att.group(1).strip() != exp_attempt:
                errors.append(f'{sum_path}: Run Attempt mismatch in summary (got "{m_sum_att.group(1)}", expected "{exp_attempt}")')

            # Verify Trigger Event
            m_sum_ev = re.search(r'\|\s*\*\*Trigger-Event\*\*\s*\|\s*`([^`]+)`', summary)
            if not m_sum_ev:
                errors.append(f'{sum_path}: Trigger-Event row "| **Trigger-Event** | `<event>` |" not found')
            elif m_sum_ev.group(1).strip() != exp_event:
                errors.append(f'{sum_path}: Trigger-Event mismatch in summary (got "{m_sum_ev.group(1)}", expected "{exp_event}")')

            # Verify Git Ref
            m_sum_ref = re.search(r'\|\s*\*\*Git-Ref\*\*\s*\|\s*`([^`]+)`', summary)
            if not m_sum_ref:
                errors.append(f'{sum_path}: Git-Ref row "| **Git-Ref** | `<ref>` |" not found')
            elif m_sum_ref.group(1).strip() != exp_ref:
                errors.append(f'{sum_path}: Git-Ref mismatch in summary (got "{m_sum_ref.group(1)}", expected "{exp_ref}")')

            if not any(e.startswith(sum_path) for e in errors):
                print(f'[OK] Verified {sum_path}')
        except Exception as e:
            errors.append(f'{sum_path} read error: {e}')

    print('----------------------------------------------------------------------')
    if errors:
        print('[FAILED] Metadata consistency validation FAILED:')
        for err in errors:
            print(f'  - {err}')
        sys.exit(1)

    print('[SUCCESS] All diagnostic metadata representations are 100% consistent!')
    print('======================================================================')

if __name__ == '__main__':
    main()
