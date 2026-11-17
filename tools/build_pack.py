"""Build hybrid_coop.pack (PFH4, type 3 = mod) for ROME II.

The pack contains:
  lua_scripts/events.lua          the game's own events.lua + lua/hybrid_coop.lua appended (in-game status text)
  hybrid_coop/hybrid_coop.dll     the co-op DLL (started by the loader dinput8.dll)
  hybrid_coop/hybrid_coop.dll.sig its Ed25519 signature from the GitHub build (the loader refuses unsigned DLLs)

The game's original events.lua is NOT part of this repository (it belongs to Creative Assembly): extract
lua_scripts/events.lua from the game's data packs and pass it with --events.

usage: python tools/build_pack.py --events events.lua --dll out/hybrid_coop.dll --sig out/hybrid_coop.dll.sig -o out/hybrid_coop.pack
"""
import argparse, pathlib, struct

ROOT = pathlib.Path(__file__).resolve().parents[1]
HEAD = (b"\r\n\r\n-- ==== Rome II hybrid co-op mod: in-game status display (UI only) ====\r\ndo\r\n"
        b"local __hc_ok, __hc_err = pcall(function()\r\nlocal __hc = (function()\r\n")
TAIL = (b"\r\nend)()\r\n__hc.attach(_M)\r\nend)\r\nif not __hc_ok then pcall(function() local f = io.open("
        b"\"hybrid_coop_lua_log.txt\", \"a\"); if f then f:write(\"load error: \" .. tostring(__hc_err) .. \"\\n\"); "
        b"f:close() end end) end\r\nend\r\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--events", required=True, help="original lua_scripts/events.lua extracted from the game")
    ap.add_argument("--dll", required=True)
    ap.add_argument("--sig", required=True)
    ap.add_argument("-o", "--out", default=str(ROOT / "out/hybrid_coop.pack"))
    a = ap.parse_args()
    orig = pathlib.Path(a.events).read_bytes()
    if b"__hc_ok" in orig:
        raise SystemExit("--events must be the ORIGINAL events.lua, not one that already contains the mod")
    script = (ROOT / "lua/hybrid_coop.lua").read_bytes().replace(b"\r\n", b"\n").replace(b"\n", b"\r\n")
    sig = pathlib.Path(a.sig).read_bytes()
    if len(sig) != 64:
        raise SystemExit("signature must be 64 bytes")
    files = sorted({
        "hybrid_coop\\hybrid_coop.dll": pathlib.Path(a.dll).read_bytes(),
        "hybrid_coop\\hybrid_coop.dll.sig": sig,
        "lua_scripts\\events.lua": orig + HEAD + script + TAIL,
    }.items())
    index = b"".join(struct.pack("<I", len(blob)) + name.encode("latin1") + b"\0" for name, blob in files)
    data = b"".join(blob for _, blob in files)
    hdr = b"PFH4" + struct.pack("<6I", 3, 0, 0, len(files), len(index), 0)
    out = pathlib.Path(a.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(hdr + index + data)
    print(out, out.stat().st_size, "bytes:", [n for n, _ in files])


if __name__ == "__main__":
    main()
