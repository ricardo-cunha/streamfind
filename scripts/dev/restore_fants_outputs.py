"""Restore final FANTS deliverables from cached structures, without network calls."""
import csv
import hashlib
import importlib.util
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DATA = Path('H:/Project_261009_fa_nts_ring_trail')
spec = importlib.util.spec_from_file_location('resolver', ROOT / 'scripts/dev/resolve_fants_v2.py')
resolver = importlib.util.module_from_spec(spec)
spec.loader.exec_module(resolver)
originals = ['internal_standards_fants_V3.csv', 'suspect_list_fants.csv']
hashes = {n: hashlib.sha256((DATA / n).read_bytes()).hexdigest() for n in originals}
cache = json.loads(Path('C:/Users/cunha/AppData/Local/hermes/cache/scratch/cir_cache.json').read_text(encoding='utf-8'))
rows = resolver.read(DATA / originals[1])
restored = []
for row in rows:
    key = row['InChiKey']
    canonical = {'name': row['name'], 'formula': row['formula'], 'InChIKey': key}
    result = ROOT / 'tmp/fants_v2' / (key + '_result.json')
    if result.exists():
        accepted = json.loads(result.read_text(encoding='utf-8'))['accepted']
        assert accepted, key
        smiles, inchi = accepted['SMILES'], accepted['InChI']
    else:
        values = cache[key]['values']
        smiles, inchi = values['smiles'].strip(), values['stdinchi'].strip()
    out, status = resolver.validate(canonical, smiles, inchi, key)
    assert status == 'validated_full_key_formula_and_structure', (key, status)
    restored.append(out)
assert len(restored) == len({r['InChIKey'] for r in restored}) == 300
standards = resolver.read(DATA / originals[0])
assert len(standards) == 22
assert all(not r[k].strip() for r in standards for k in ['ms2_positive', 'ms2_negative'])
fields = [k for k in standards[0] if k not in ['ms2_positive', 'ms2_negative']]
cleaned = [{k: r[k] for k in fields} for r in standards]
for name, output, headers in [
    ('internal_standards_fants_V3_cleaned.csv', cleaned, fields),
    ('suspect_list_fants_enriched_v2.csv', restored, resolver.FIELDS),
]:
    with (DATA / name).open('x', encoding='utf-8', newline='') as f:
        writer = csv.DictWriter(f, fieldnames=headers)
        writer.writeheader()
        writer.writerows(output)
    assert resolver.read(DATA / name) == output
    print(f'RESTORED AND VERIFIED: {DATA / name}: {len(output)} rows')
assert all(hashlib.sha256((DATA / n).read_bytes()).hexdigest() == h for n, h in hashes.items())
print('Originals unchanged. All 300 full InChIKeys, formulas, and SMILES/InChI identities validated; masses recalculated with RDKit.')
