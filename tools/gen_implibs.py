"""Write kernel32.def / user32.def from the declarations in src/common/win.h (stdcall names need @bytes)."""
import re, sys, pathlib
root = pathlib.Path(__file__).resolve().parents[1]
out = pathlib.Path(sys.argv[1])
src = (root / "src/common/win.h").read_text()
libs = {"kernel32": [], "user32": []}
for m in re.finditer(r"DLLIMPORT\s+[^;(]*?(WINAPI|__cdecl)\s+(\w+)\((.*?)\);", src):
    cc, name, args = m.groups()
    args = args.strip()
    n, depth = (0 if args in ("void", "") else 1), 0
    for ch in args:
        depth += ch == "("; depth -= ch == ")"
        n += ch == "," and depth == 0
    lib = "user32" if name in ("wsprintfA", "MessageBeep") else "kernel32"
    libs[lib].append(name if cc == "__cdecl" else f"{name}@{4 * n}")
for lib, names in libs.items():
    (out / f"{lib}.def").write_text(f"LIBRARY {lib}.dll\nEXPORTS\n" + "\n".join(names) + "\n")
