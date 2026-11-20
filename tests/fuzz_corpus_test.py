"""Replay every saved input with explicit expected acceptance, without fuzzing."""
import hashlib
import json
import pathlib
import subprocess
import sys
import tempfile

repo = pathlib.Path(__file__).resolve().parents[1]
corpus = repo / 'fuzz' / 'corpus'
manifest = json.loads((corpus / 'manifest.json').read_text())
files = {p.relative_to(corpus).as_posix() for p in corpus.rglob('*') if p.is_file()}
if files != set(manifest) | {'manifest.json'}:
    raise AssertionError('unlisted or missing corpus inputs')
with tempfile.TemporaryDirectory(prefix='fuzz-corpus-test-') as temp:
    regenerated = pathlib.Path(temp) / 'corpus'
    subprocess.run([sys.executable, str(repo / 'fuzz' / 'make_corpus.py'), '--output', str(regenerated)], check=True)
    if (regenerated / 'manifest.json').read_bytes() != (corpus / 'manifest.json').read_bytes():
        raise AssertionError('seed manifest is not reproducible')
    for name, expected in manifest.items():
        data = (corpus / name).read_bytes()
        if data != (regenerated / name).read_bytes() or len(data) != expected['bytes'] or hashlib.sha256(data).hexdigest() != expected['sha256']:
            raise AssertionError(f'corpus identity mismatch: {name}')
        result = subprocess.run([sys.argv[1], name.split('/')[0], str(corpus / name)], timeout=10)
        if result.returncode != (0 if expected['valid'] else 1):
            raise AssertionError(f'unexpected decoder result for {name}: {result.returncode}')
# Findings are independent of the seed generator. Each has expected behavior.
regressions = repo / 'fuzz' / 'regressions'
for name, expected in json.loads((regressions / 'manifest.json').read_text()).items():
    result = subprocess.run([sys.argv[1], expected['target'], str(regressions / name)], timeout=10)
    if result.returncode != (0 if expected['valid'] else 1):
        raise AssertionError(f'fuzz regression: {name}')
print(f'{len(manifest)} corpus inputs and saved regressions passed')
