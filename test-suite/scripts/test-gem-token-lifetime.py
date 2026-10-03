#!/usr/bin/env python3
"""Compile the actual GEM token functions against a userspace lifetime/index
model. Offline only: no module, debugfs, device or accelerator access."""
import argparse, hashlib, json, pathlib, subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--output', required=True, type=pathlib.Path)
a = p.parse_args()
a.output = a.output.resolve()
a.output.mkdir(parents=True, exist_ok=True)

suite = pathlib.Path(__file__).resolve().parent.parent   # .../test-suite
repo = suite.parent                                      # .../rk3588-rknn-core
source = (repo / 'src/rknpu_gem.c').read_text()
header = (repo / 'src/include/rknpu_drv.h').read_text()

begin = header.index('enum rknpu_mem_stat {')
end = header.index('};', begin) + 2
(a.output / 'gem-token-stats.inc').write_text(header[begin:end])

start = source.index('static inline unsigned long\nrknpu_gem_dma_token_key(')
end = source.index('static void rknpu_gem_release(', start)
fragment = source[start:end]
(a.output / 'gem-token-under-test.inc').write_text(fragment)

cmd = ['gcc', '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
       '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-pthread',
       '-I', str(a.output), str(suite / 'src/gem_token_lifetime_test.c'),
       '-o', str(a.output / 'gem-token-test')]
subprocess.run(cmd, check=True)
run = subprocess.run([str(a.output / 'gem-token-test')], stdout=subprocess.PIPE,
                     stderr=subprocess.STDOUT, text=True, timeout=60)
(a.output / 'result.log').write_text(run.stdout)
(a.output / 'identity.json').write_text(json.dumps({
    'source_sha256': hashlib.sha256(source.encode()).hexdigest(),
    'fragment_sha256': hashlib.sha256(fragment.encode()).hexdigest(),
    'command': cmd, 'rc': run.returncode}, indent=2))
print(run.stdout, end='')
raise SystemExit(run.returncode)
