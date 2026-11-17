"""Copy the game's original lua_scripts/events.lua out of the ROME II data packs (read-only).

tools/build_pack.py needs it: the mod's events.lua = the game's events.lua + lua/hybrid_coop.lua.
This file belongs to Creative Assembly, so it is not stored in this repository.

usage: python tools/extract_events.py "C:/Program Files (x86)/Steam/steamapps/common/Total War Rome II" [-o out/events.lua]
"""
import argparse, pathlib, struct

WANT = "lua_scripts\\events.lua"
GAME_PACK_TYPES = {0: "boot", 1: "release", 2: "patch"}      # 3 = mod, 4 = movie


def find_in_pack(path):
    with open(path, "rb") as f:
        magic = f.read(4)
        if magic not in (b"PFH4", b"PFH5"):
            return None
        ptype, _nref, refsz, nfiles, idxsz = struct.unpack("<5I", f.read(20))
        if (ptype & 0xF) not in GAME_PACK_TYPES or ptype & 0x90:   # only the game's own, unencrypted packs
            return None
        hdr = 28 + (20 if ptype & 0x100 else 0)
        f.seek(hdr + refsz)
        index = f.read(idxsz)
        o, data_off = 0, hdr + refsz + idxsz
        for _ in range(nfiles):
            size = struct.unpack_from("<I", index, o)[0]; o += 4
            if ptype & 0x40: o += 4
            if magic == b"PFH5": o += 1
            e = index.index(b"\0", o); name = index[o:e].decode("latin1"); o = e + 1
            if name.lower() == WANT:
                f.seek(data_off)
                return (ptype & 0xF), f.read(size)
            data_off += size
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("game", help="ROME II folder (the one with Rome2.exe)")
    ap.add_argument("-o", "--out", default="out/events.lua")
    a = ap.parse_args()
    found = []
    for p in sorted(pathlib.Path(a.game, "data").glob("*.pack")):
        r = find_in_pack(p)
        if r:
            found.append((r[0], p.name, r[1]))
            print(f"  {p.name} ({GAME_PACK_TYPES[r[0]]}): {len(r[1])} bytes")
    if not found:
        raise SystemExit("events.lua not found in the game's packs")
    found.sort(key=lambda x: (x[0], x[1]))          # patch packs override release packs
    kind, name, data = found[-1]
    if any(d != data for _, _, d in found):
        print("  note: the packs contain different versions, using", name)
    out = pathlib.Path(a.out); out.parent.mkdir(parents=True, exist_ok=True); out.write_bytes(data)
    print("wrote", out, f"({len(data)} bytes, from {name})")


if __name__ == "__main__":
    main()
