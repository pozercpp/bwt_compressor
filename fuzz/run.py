#!/usr/bin/env python3
"""Run one bounded libFuzzer campaign; keep generated corpus/artifacts out of source seeds."""
import argparse
import os
import pathlib
import resource
import shutil
import subprocess
import tempfile


def positive(value):
    value = int(value)
    if value < 1:
        raise argparse.ArgumentTypeError('must be positive')
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('target', choices=('bwc1', 'bwa1', 'bwa2'))
    parser.add_argument('--build-dir', type=pathlib.Path, default=pathlib.Path('build-fuzz'))
    parser.add_argument('--work-dir', type=pathlib.Path, required=True, help='new directory for corpus, artifacts, log and private temporary files')
    parser.add_argument('--seconds', type=positive, default=60)
    parser.add_argument('--runs', type=positive, help='optional additional iteration limit')
    parser.add_argument('--seed', type=positive, default=20261121)
    args = parser.parse_args()
    executable = (args.build_dir / 'fuzz' / ('fuzz_' + args.target)).resolve()
    if not executable.is_file():
        parser.error('build the fuzz target first')
    root = args.work_dir.resolve()
    root.mkdir(mode=0o700)  # Never mix runs or overwrite earlier findings.
    corpus = root / 'corpus'
    shutil.copytree(pathlib.Path(__file__).resolve().parent / 'corpus' / args.target, corpus)
    artifacts = root / 'artifacts'
    artifacts.mkdir()
    # Avoid symbolizing every new function during mutation; sanitizer reports
    # still retain full symbols. Slow symbolizers must not consume the campaign.
    command = [str(executable), str(corpus), f'-artifact_prefix={artifacts}/',
               '-print_funcs=0', '-timeout=5', '-rss_limit_mb=512', '-malloc_limit_mb=128',
               '-max_len=65536', f'-max_total_time={args.seconds}', f'-seed={args.seed}']
    if args.runs:
        command.append(f'-runs={args.runs}')

    def limits():
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
        resource.setrlimit(resource.RLIMIT_FSIZE, (8 * 1024 * 1024, 8 * 1024 * 1024))
        resource.setrlimit(resource.RLIMIT_NOFILE, (256, 256))
        resource.setrlimit(resource.RLIMIT_CPU, (args.seconds + 10, args.seconds + 15))

    env = os.environ.copy()
    env['ASAN_OPTIONS'] = 'detect_leaks=1:abort_on_error=1'
    env['UBSAN_OPTIONS'] = 'halt_on_error=1:print_stacktrace=1'
    with tempfile.TemporaryDirectory(prefix='private-', dir=root) as temporary:
        env['TMPDIR'] = temporary
        with (root / 'run.log').open('w') as log:
            log.write('Command: ' + repr(command) + '\n')
            log.flush()
            try:
                result = subprocess.run(command, cwd=temporary, env=env, preexec_fn=limits,
                                        stdout=log, stderr=log, timeout=args.seconds + 20)
                code = result.returncode
            except subprocess.TimeoutExpired:
                log.write('\nRunner wall-clock budget exhausted.\n')
                code = 124
    print(f'Exit {code}; corpus, artifacts and log: {root}')
    raise SystemExit(code if code >= 0 else 128 - code)


if __name__ == '__main__':
    main()
