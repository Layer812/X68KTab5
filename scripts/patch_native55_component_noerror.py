from pathlib import Path
import sys
ROOT = Path(__file__).resolve().parents[1]
targets = [ROOT/"components"/"M5GFX"/"CMakeLists.txt", ROOT/"components"/"M5Unified"/"CMakeLists.txt"]
marker = "# PX68K_NATIVE55_WARNING_BRIDGE_V24"
flags = "-Wno-error=stringop-truncation -Wno-error=format-truncation -Wno-error=maybe-uninitialized -Wno-error=multistatement-macros -Wno-error=shift-count-overflow -Wno-error=overflow -Wno-error=deprecated-declarations -Wno-error=discarded-qualifiers -Wno-error=incompatible-pointer-types -Wno-error=int-conversion"
block = f"""
# PX68K_NATIVE55_WARNING_BRIDGE_V24
# Keep diagnostics visible, but override IDF's specific -Werror=<warning>.
if(TARGET ${{COMPONENT_LIB}})
    target_compile_options(${{COMPONENT_LIB}} PRIVATE {flags})
endif()
"""
for path in targets:
    if not path.exists():
        print(f"ERROR: missing {path}"); sys.exit(2)
    text = path.read_text(encoding="utf-8", errors="replace")
    for old_marker in ("# PX68K_NATIVE55_WARNING_BRIDGE_V24", "# PX68K_NATIVE55_WARNING_BRIDGE"):
        if old_marker in text:
            text = text[:text.find(old_marker)].rstrip()+"\n"
    text = text.rstrip()+"\n\n"+block.strip()+"\n"
    path.write_text(text, encoding="utf-8", newline="\n")
    verify = path.read_text(encoding="utf-8", errors="replace")
    if marker not in verify or "-Wno-error=stringop-truncation" not in verify:
        print(f"ERROR: verify failed: {path}"); sys.exit(3)
    print(f"*** PX68K_NATIVE55_WARNING_BRIDGE_V24: patched+verified {path}")
print("*** PX68K_NATIVE55_WARNING_BRIDGE_V24: specific warning de-escalation active")
