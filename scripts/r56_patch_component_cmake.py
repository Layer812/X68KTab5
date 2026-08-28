from pathlib import Path
import sys

MARKER = '"tab5_screen_manager.c"'
project = Path.cwd()
cmake = project / 'src' / 'CMakeLists.txt'
source = project / 'src' / 'tab5_screen_manager.c'

if not source.exists():
    print(f'R56 ERROR: missing {source}')
    sys.exit(2)
if not cmake.exists():
    print(f'R56 ERROR: missing {cmake}')
    sys.exit(3)

text = cmake.read_text(encoding='utf-8', errors='strict')
if 'idf_component_register(' not in text:
    print(f'R56 ERROR: {cmake} is not an ESP-IDF component manifest')
    sys.exit(4)

if MARKER in text:
    print('R56 CMake: tab5_screen_manager.c already registered')
    sys.exit(0)

anchor = '"tab5_compose.c"'
pos = text.find(anchor)
if pos < 0:
    print('R56 ERROR: tab5_compose.c anchor not found; refusing to rewrite unknown source list')
    sys.exit(5)

line_end = text.find('\n', pos)
if line_end < 0:
    line_end = len(text)
line_start = text.rfind('\n', 0, pos) + 1
indent = text[line_start:pos]
if not indent.isspace():
    indent = '        '
insert = f'\n{indent}{MARKER}'
text = text[:line_end] + insert + text[line_end:]
cmake.write_text(text, encoding='utf-8', newline='\n')

verify = cmake.read_text(encoding='utf-8')
if verify.count(MARKER) != 1:
    print('R56 ERROR: post-patch source registration verification failed')
    sys.exit(6)
print('R56 CMake: registered tab5_screen_manager.c exactly once')
