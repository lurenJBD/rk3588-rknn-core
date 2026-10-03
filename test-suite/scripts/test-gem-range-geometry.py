#!/usr/bin/env python3
"""Extract the range-sync helper from the driver and check its physical byte
coverage against a flat oracle. Offline only: no module, device or accelerator
access."""
import argparse, hashlib, json, pathlib, subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--output', required=True, type=pathlib.Path)
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=True)

suite = pathlib.Path(__file__).resolve().parent.parent   # .../test-suite
repo = suite.parent                                      # .../rk3588-rknn-core
source = (repo / 'src/rknpu_gem.c').read_text()
start = source.index('static int rknpu_gem_sync_sg_range(')
end = source.index('\nstatic int rknpu_gem_sync_core_maps(', start)
fragment = source[start:end]
(a.output / 'gem-range-under-test.inc').write_text(fragment)

cmd = ['gcc', '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
       '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-I',
       str(a.output), str(suite / 'src/gem_range_geometry_test.c'), '-o',
       str(a.output / 'gem-range-test')]
subprocess.run(cmd, check=True)
run = subprocess.run([str(a.output / 'gem-range-test')], capture_output=True,
                     text=True, timeout=60)
(a.output / 'result.log').write_text(run.stdout + run.stderr)
(a.output / 'identity.json').write_text(json.dumps({
    'source_sha256': hashlib.sha256(source.encode()).hexdigest(),
    'fragment_sha256': hashlib.sha256(fragment.encode()).hexdigest(),
    'command': cmd, 'rc': run.returncode}, indent=2))
print(run.stdout + run.stderr, end='')
raise SystemExit(run.returncode)
