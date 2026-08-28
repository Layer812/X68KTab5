from pathlib import Path
import sys

if len(sys.argv) != 3:
    print('usage: r56k2_verify_postlink.py <elf> <fmg_obj>', file=sys.stderr)
    sys.exit(2)
elf = Path(sys.argv[1])
obj = Path(sys.argv[2])
if not elf.is_file():
    print(f'R56k2 postlink ERROR: ELF missing: {elf}', file=sys.stderr); sys.exit(3)
if not obj.is_file():
    print(f'R56k2 postlink ERROR: YM object missing: {obj}', file=sys.stderr); sys.exit(4)
eb = elf.read_bytes()
ob = obj.read_bytes()

def need(buf: bytes, text: str, what: str, rc: int):
    if text.encode('ascii') not in buf:
        print(f'R56k2 postlink ERROR: {what} marker missing: {text}', file=sys.stderr)
        sys.exit(rc)
    print(f'R56k2 postlink OK: {what}')

def forbid(buf: bytes, text: str, what: str, rc: int):
    if text.encode('ascii') in buf:
        print(f'R56k2 postlink ERROR: stale {what} marker survived: {text}', file=sys.stderr)
        sys.exit(rc)
    print(f'R56k2 postlink OK: no stale {what}')

# YM marker is deliberately source/object evidence, not a runtime string.
# __attribute__((used)) keeps it in fmg_wrap.cpp.obj, while final --gc-sections
# is expected to discard its unreferenced .rodata section.
need(ob, 'PX68K_HOST_R56K: YM prio=3 complete-event boundary-yield', 'YM rebuilt object', 10)

# These are live runtime diagnostics and therefore MUST survive final linking.
need(eb, 'PX68K_HOST_R56K: Screen Manager prio=3', 'Screen Manager runtime', 11)
need(eb, 'PX68K_HOST_R56K: compositor prio=3', 'compositor runtime', 12)
need(eb, 'PX68K_HOST_R56K: LCD presenter prio=3', 'LCD runtime', 13)
need(eb, 'PX68K_HOST_R56K: CPU0 host scheduler contract ACTIVE', 'host scheduler startup', 14)
need(eb, 'R56K_HOST f=', 'CPU1 health sampler', 15)
need(eb, 'PX68K_R56H1: TWDT reconfigured', 'R56h1 TWDT policy', 16)
need(eb, 'PX68K_SCREEN_R56D', 'R56d managed-present', 17)

forbid(eb, 'PX68K_AUDIO_R56I', 'R56i audio probe', 20)
forbid(eb, 'PX68K_AUDIO_R56J', 'R56j codec probe', 21)
forbid(eb, 'PX68K_FM_R56F', 'R56f scheduler', 22)
forbid(eb, 'PX68K_FM_R56G', 'R56g scheduler', 23)
print('R56k2 postlink VERIFIED: compiled substrate matches R56k stability contract')
