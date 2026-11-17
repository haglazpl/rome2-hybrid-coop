"""CI check: the private signing key (PEM, Ed25519) must belong to the public key compiled into the loader."""
import re, subprocess, sys, pathlib
root = pathlib.Path(__file__).resolve().parents[1]
want = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-f]{2})", (root / "src/loader/pubkey.h").read_text()))
der = subprocess.run(["openssl", "pkey", "-in", sys.argv[1], "-pubout", "-outform", "DER"], capture_output=True, check=True).stdout
if der[-32:] != want:
    sys.exit("SIGNING_KEY does not match src/loader/pubkey.h")
print("signing key matches the loader's public key")
