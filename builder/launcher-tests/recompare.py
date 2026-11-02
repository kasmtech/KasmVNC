#!/usr/bin/env python3
"""Recompare recorded raw results after a comparison-only harness correction."""
import argparse
import json
from pathlib import Path
from parity import compare, require_container

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('report')
parser.add_argument('output')
args = parser.parse_args()
require_container(parser)
source, destination = Path(args.report), Path(args.output)
if source.resolve() == destination.resolve() or (destination.exists() and source.samefile(destination)):
    parser.error('output must not overwrite the original report')
report = json.loads(source.read_text())
for row in report['results']:
    if row.get('candidate') is None:
        parser.error('oracle-only reports cannot be compared')
    item = row.get('input', {'prefix': row['oracle'].get('command', {}).get('prefix', [])})
    row['problems'] = compare(row['oracle'], row['candidate'], item)
    if row['name'] == 'defaults' and row['oracle']['status'] != 0:
        row['problems'].append('oracle-baseline-must-succeed')
summary = report['summary']
summary['failed'] = sum(bool(row['problems']) for row in report['results'])
summary['semantic_failures'] = sum(any(p != 'diagnostics' for p in row['problems']) for row in report['results'])
summary['diagnostic_only_failures'] = sum(row['problems'] == ['diagnostics'] for row in report['results'])
report['recompared_from'] = args.report
Path(args.output).write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
print(json.dumps(summary))
