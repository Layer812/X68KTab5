from pathlib import Path
import shutil, sys
root = Path(__file__).resolve().parents[1]
target = root / 'components' / 'M5Unified' / 'src' / 'utility' / 'Speaker_Class.cpp'
bak = target.with_suffix(target.suffix + '.r57e91.bak')
if not bak.exists():
    print('No R57E91 backup found; nothing restored.')
    sys.exit(0)
shutil.copy2(bak, target)
print('Restored:', target)
