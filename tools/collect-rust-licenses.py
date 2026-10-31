#!/usr/bin/env python3
"""Refresh bundled dependency notices from the locked host-platform Cargo graph."""
import json
from pathlib import Path
import shutil
import subprocess

root = Path(__file__).resolve().parents[1]
host = next(line.split(': ', 1)[1] for line in subprocess.check_output(['rustc', '-vV'], text=True).splitlines() if line.startswith('host:'))
metadata = json.loads(subprocess.check_output(['cargo', 'metadata', '--manifest-path', str(root / 'adapter/Cargo.toml'), '--offline', '--locked', '--filter-platform', host, '--format-version', '1'], text=True))
out = root / 'adapter/THIRD_PARTY_LICENSES'
out.mkdir(exist_ok=True)
summary = ['# Rust dependency licenses', '', 'Notices for the host-platform dependency graph resolved by Cargo.lock, including build and test dependencies.', '']
missing = []
for package in sorted(metadata['packages'], key=lambda p: (p['name'], p['version'])):
    if package['name'] in ('feather-transfer', 'owly-airdrop'):
        continue
    source = Path(package['manifest_path']).parent
    dest = out / (package['name'] + '-' + package['version'])
    files = [p for p in source.iterdir() if p.is_file() and p.name.upper().startswith(('LICENSE', 'LICENCE', 'COPYING', 'COPYRIGHT', 'NOTICE', 'UNLICENSE'))]
    for folder in ('LICENSES', 'licenses'):
        if (source / folder).is_dir():
            files.extend(p for p in (source / folder).rglob('*') if p.is_file())
    if not files:
        missing.append(package['name'])
    for path in files:
        target = dest / path.relative_to(source)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(path, target)
    summary.append(f"- {package['name']} {package['version']}: {package.get('license') or 'See bundled notices'}")
(out / 'README.md').write_text('\n'.join(summary) + '\n')
if missing:
    raise SystemExit('Missing license notices: ' + ', '.join(missing))
print(f"Collected notices for {len(metadata['packages']) - 2} dependencies")
