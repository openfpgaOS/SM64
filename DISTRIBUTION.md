# Distribution Policy

**This project is distributed as source only.**

Building it and copying the result to your own Analogue Pocket is fine and
fully supported — that is what `make` and `make copy` are for. What this
repository does not do is publish, upload, or hand out a *built* bundle.

```
cd src/sm64 && make          # build (extracts assets from YOUR ROM)
cd src/sm64 && make copy     # deploy to YOUR OWN SD card
cd src/sm64 && make package  # refuses, on purpose
```

---

## Why a built bundle cannot ship

Four independent reasons. Any one of them alone would be sufficient.

### 1. `app.elf` embeds assets extracted from a Super Mario 64 ROM

The build extracts textures, audio samples, sequences, skyboxes, animations
and demo data from the ROM you supply, converts them, and links them into
`app.elf`. Roughly 6.3 MB of the linked image is ROM-extracted media.

Super Mario 64 is © 1996 Nintendo Co., Ltd. Distributing a binary containing
that data would be distributing Nintendo's copyrighted work, regardless of how
it got there. Extracting it from a copy you own, on your own machine, for your
own device, is a different act from redistributing the result.

See `src/sm64/Makefile` (the asset-extraction block) and the README's ROM
section.

### 2. The Fast3D renderer forbids binary redistribution outright

`src/sm64/sm64/src/pc/gfx/LICENSE.txt` — © 2020 Emill, MaikelChan:

> 2. Redistributions in binary form are not allowed.

This covers `gfx_pc.c` and `gfx_cc.c`, which this core links, and applies to
the whole `src/pc/gfx` directory — including this project's own `gfx_gpu.c`,
which lives inside it. **This reason is entirely independent of Nintendo**: it
would block a binary release even if every asset were original.

### 3. The bundled sample bank is third-party

`runtime/bank.ofsf` is an SC-55 SoundFont whose underlying samples are
© Roland Corporation. See `LICENSES/LicenseRef-Proprietary-SampleData.txt` and
the `NOTICE` file.

### 4. The prebuilt bitstream carries third-party RTL

`runtime/pocket/os30.rbf_r` and `runtime/pocket/os.bin` embed the Analogue
Pocket Framework (proprietary), MiSTer-derived video cores (GPL-2.0) and
Intel/Altera megafunction IP. See `NOTICE`.

---

## What *is* redistributable

This repository's own source, under Apache-2.0, with the third-party notices
retained. `LICENSE`, `NOTICE`, `REUSE.toml` and `LICENSES/` are the
authoritative record of what is covered by what.

Note that the vendored decompilation under `src/sm64/sm64/` carries no license
grant from anyone — neither `n64decomp/sm64` nor `sm64-port` publishes one.
Source-only distribution is upstream's own posture and the reason upstream
ships source only. It reduces exposure; it does not eliminate it.

---

## No ROM data is committed

Verified clean — nothing matching `*.z64` / `*.n64` / `*.v64`, no extracted
textures, no `.aiff` samples, no `.m64` sequences. `make check-dist` asserts
this and fails the build if a binary artifact or ROM image is ever staged.

**But two ROM images do sit in the working tree:**

```
Mario64.z64                     (repo root)
src/sm64/sm64/baserom.us.z64
```

Both are gitignored, so `git clone` and `git archive` are clean. A naive
`tar`/`zip` of the working directory, an IDE "export project", or a container
build that does `COPY . .` **would ship a Nintendo ROM.** Prefer
`git archive` when making a source drop, and consider moving `Mario64.z64`
outside the repository entirely.

---

## The gates

Four places refuse to produce or publish a bundle:

| Location | Behaviour |
|---|---|
| `src/sm64/Makefile` → `package` | Hard refusal, no override |
| `scripts/package.sh` | Refuses unless `OF_ALLOW_BINARY_DIST` is set |
| `Makefile` → `release` | Refuses unless `OF_ALLOW_BINARY_DIST` is set |
| `scripts/release.sh` | Refuses unless `OF_ALLOW_BINARY_DIST` is set |

Only the first is durable. The other three live in SDK-owned files that
`make push DEST=...` rsyncs from an upstream openfpgaSDK checkout and can
silently revert — upstream the same change if you can.

The `OF_ALLOW_BINARY_DIST` escape hatch exists because those three scripts are
SDK-generic and other cores may legitimately be redistributable. **It is not
an escape hatch for this core**, which is blocked by reasons 1 and 2 above.
