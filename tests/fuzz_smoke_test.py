"""Bounded libFuzzer integration checks; only enabled in fuzz builds."""
import pathlib
import subprocess
import sys
import tempfile

repo = pathlib.Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='fuzz-smoke-') as temporary:
    root = pathlib.Path(temporary)
    for target in ('bwc1', 'bwa1', 'bwa2'):
        work = root / target
        result = subprocess.run([sys.executable, str(repo / 'fuzz' / 'run.py'), target,
                                '--build-dir', sys.argv[1], '--work-dir', str(work),
                                '--runs', '100', '--seconds', '10'], timeout=40)
        log = (work / 'run.log').read_text()
        if result.returncode or 'Done 100 runs' not in log:
            raise AssertionError(f'{target} failed:\n{log}')
        if list(work.glob('private-*')) or list((work / 'artifacts').iterdir()):
            raise AssertionError(f'{target}: leaked temporary files or crash artifacts')
print('All bounded fuzz targets passed')
