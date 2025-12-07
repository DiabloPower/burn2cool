#!/usr/bin/env bash
# Validate JSON files and check for missing keys compared to en.json
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
I18N_DIR="$ROOT_DIR/gui_tray/i18n"
EN_FILE="$I18N_DIR/en.json"
if ! command -v python3 >/dev/null 2>&1; then
  echo "python3 required for validation script" >&2
  exit 2
fi
python3 - "$ROOT_DIR" <<'PY'
import json,sys,os
root=os.path.join(sys.argv[1], 'gui_tray', 'i18n')
root=os.path.abspath(root)
en=os.path.join(root, 'en.json')
def collect_keys(obj, prefix=''):
    keys=set()
    if isinstance(obj, dict):
        for k,v in obj.items():
            p = f"{prefix}.{k}" if prefix else k
            if isinstance(v, dict):
                keys |= collect_keys(v, p)
            else:
                keys.add(p)
    return keys

with open(en,'r',encoding='utf-8') as f:
    enobj=json.load(f)
en_keys=collect_keys(enobj)
# Do not require locales to replicate the human-friendly 'meta.name' field
if 'meta.name' in en_keys:
    en_keys.remove('meta.name')
print(f"Reference locale: {en}")
errors=0
for fn in sorted(os.listdir(root)):
    if not fn.endswith('.json'): continue
    path=os.path.join(root,fn)
    try:
        with open(path,'r',encoding='utf-8') as f:
            obj=json.load(f)
    except Exception as e:
        print(f"[ERROR] {fn}: invalid JSON: {e}")
        errors+=1
        continue
    keys=collect_keys(obj)
    # allow each locale to have its own human-friendly meta.name (don't treat as extra/missing)
    if 'meta.name' in keys:
        keys.remove('meta.name')
    missing = sorted(en_keys - keys)
    extra = sorted(keys - en_keys)
    if missing:
        print(f"[MISSING] {fn}: {len(missing)} keys missing")
        for k in missing: print(f"  - {k}")
        errors+=1
    else:
        print(f"[OK] {fn}: all keys present")
    if extra:
        print(f"[EXTRA] {fn}: {len(extra)} extra keys")
        for k in extra: print(f"  + {k}")

if errors:
    sys.exit(1)
PY
