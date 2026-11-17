# Making a release

Players get two things: the **loader** (`dinput8.dll`, from GitHub Releases, installed once) and the **mod pack**
(`hybrid_coop.pack`, from the Steam Workshop, updated automatically). The pack carries the co-op DLL and its
signature, so every new version reaches players through the Workshop.

## One-time setup

- Repository secret `SIGNING_KEY`: the Ed25519 private key (PEM) that belongs to `src/loader/pubkey.h`.
  Keep a private backup. If it is lost, a new key pair means a new loader for every player.

## Steps

1. **Tag the version** and push the tag:
   ```sh
   git tag v0.1.0
   git push origin v0.1.0
   ```
   GitHub Actions tests, builds and signs the DLLs (the version text and build id come from the tag) and creates
   a release with `dinput8.dll`, `hybrid_coop.dll`, `hybrid_coop.dll.sig`, `SHA256SUMS.txt` and the install scripts.
2. **Download** `hybrid_coop.dll` and `hybrid_coop.dll.sig` from that release into `out/`.
3. **Get the game's events.lua** (Creative Assembly's file, not in this repository):
   ```sh
   python tools/extract_events.py "C:/Program Files (x86)/Steam/steamapps/common/Total War Rome II"
   ```
4. **Build the pack:**
   ```sh
   python tools/build_pack.py --events out/events.lua --dll out/hybrid_coop.dll --sig out/hybrid_coop.dll.sig
   ```
5. **Test** with two PCs: put the pack into the game's `data` folder, enable it in the launcher, and check that
   `%LOCALAPPDATA%\RomeHybridCoop\loader.log` says `signature OK` and that the core log shows the new version.
6. **Upload** `out/hybrid_coop.pack` to the Workshop with the ROME II mod manager.

## Rules

- **Players must run the same build.** Every release gets its own build id. If two players' ids differ, both see
  "different mod versions" on the End Turn button and a `VERSION MISMATCH` line in the log.
- **The loader changes only when it must.** A changed public key, or a fix in the loader itself, means every
  player has to download it again.
- **The pack must contain the release's `hybrid_coop.dll`** exactly as it is: the signature covers every byte.
