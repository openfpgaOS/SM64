# Upstream provenance of this vendored tree

This directory (`src/sm64/sm64/`) is **not** work of the openfpgaOS SM64 core
project. It is a vendored copy of a third-party Super Mario 64 PC port, with a
small number of files modified or added locally to build against the
openfpgaOS SDK. Attribution for everything in here is in the repository-root
`NOTICE`.

## Identity

| | |
|---|---|
| Upstream repository | `fgsfdsfgs/sm64-port` |
| Upstream branch | `dos` |
| That fork's parent | `sm64-port/sm64-port` — *inferred*; see the caveat under Evidence |
| Decompilation base | `n64decomp/sm64`, **Refresh 11** |
| Upstream commit SHA | **not recoverable** (see below) |
| Vendoring date | not recorded upstream; the tree first appears in this repo's history in commit `0794cfb` ("Work in progress"), which is the vendoring commit, not the upstream date |

> Everything in this file is derived from files inside this directory. Claims
> about what *upstream* repositories contain cannot be checked from a
> flattened copy, so where such a claim appears below it is marked as an
> inference rather than stated as verified.

## Evidence

Everything above was checked against files in this tree. Citations are
`path:line` relative to this directory.

**That this is a port of `sm64-port`, forked by `fgsfdsfgs`, branch `dos`:**

- `README.md:1` — title `# Super Mario 64 DOS Port`
- `README.md:3` — "This is a novelty port of the sm64-port to DOS. Do not
  expect it to be playable."
- `README.md:5`, `README.md:7`, `README.md:9` — image URLs pinned to the fork
  and branch:
  `https://raw.githubusercontent.com/fgsfdsfgs/sm64-port/dos/media/screenshot_0{0,1,2}.png`

**That the DOS target is the default configuration of this tree:**

- `Makefile:25` — `TARGET_DOS ?= 1`, i.e. DOS is the default target of *this*
  tree. (What upstream `sm64-port` defaults it to is not checkable from here.)
- `Makefile:490` — `CPP := i586-pc-msdosdjgpp-cpp -P`, and the surrounding
  block (`Makefile:489-495`) selecting the whole DJGPP toolchain
- `Makefile:516` — `-Llib/allegro -lalleg`; `Makefile:557` onwards — `DOS_GL`
  selection between `dmesa` (3Dfx/Glide) and `osmesa`
- DOS-specific sources present: `src/pc/gfx/gfx_dos_api.c`,
  `src/pc/gfx/gfx_soft.c`, `src/pc/audio/audio_sb16.c`,
  `src/pc/controller/controller_dos_keyboard.c`
- DOS-specific prebuilt libraries: `lib/allegro/`, `lib/dmesa/`,
  `lib/glide3/`, `lib/osmesa/`

**That the port layer comes from `sm64-port` — INFERRED, not verified:**

- `README.md:3` calls this "a novelty port of the sm64-port to DOS", and the
  tree contains a full `src/pc/` port layer (`configfile.c`, `mixer.c`,
  `ultra_reimplementation.c`, `pc_main.c`, the `audio/`, `controller/` and
  `gfx/` backend directories) of the kind sm64-port provides.
- Nothing in this tree names the org/repo `sm64-port/sm64-port`, and whether
  any particular file also exists in the pure decomp cannot be checked from a
  flattened copy. Treat the parent-repo row in the table above as an
  inference drawn from `README.md:3`.

**That the decomp base is `n64decomp/sm64` at Refresh 11:**

- `CHANGES:1` — the file's first line is `Refresh 11`, and the entries beneath
  it are the n64decomp/sm64 refresh changelog (PRs #1039-#1049), followed by
  the `Refresh #10.1` and `Refresh #10` blocks.
- Decomp-specific infrastructure is present and intact: `Makefile.split`,
  `diff.py`, `first-diff.py`, `undefined_syms.txt`, `sm64.ld`,
  `sm64.{us,eu,jp,sh}.sha1`, `tools/asm_processor/`, `assets.json`,
  `extract_assets.py`.
- `Makefile:27` — `COMPILER ?= ido`, i.e. the tree still expects the IDO 5.3
  compiler (`tools/ido5.3_compiler/`) used for matching decomp builds.
- `sm64.us.sha1` pins `9bef1128717f958171a4afac3ed78ee2bb4e86ce` for
  `build/us/sm64.us.z64` — the US retail ROM checksum the decomp targets.

**No contradiction was found** between the in-tree evidence and the believed
identity. Note, however, that the branch/fork attribution rests on the README
image URLs (`README.md:5-9`) and the DOS defaults; nothing in the tree names
`n64decomp/sm64` literally, so the decomp base is established by the `CHANGES`
refresh changelog and the decomp build infrastructure rather than by a direct
reference.

## Flattened copy — no recoverable SHA

This is a **flattened** copy: the upstream history was not preserved.

- there is no `.git` directory here and no `.gitmodules` entry anywhere in the
  repository pointing at an upstream SM64 remote;
- no upstream `git` metadata, tag, version file or vendoring manifest was
  copied in;
- `CHANGES` records the *decomp* refresh level, not the fork's commit.

The exact upstream commit is therefore **not recoverable from this repository**
and must not be guessed. Do not add a SHA here unless it is obtained by
actually matching this tree against upstream (see below).

## Local modifications

Files in this tree that were changed or added for the openfpgaOS core (these
are the ones that will conflict on any re-sync):

- **Added** — `src/pc/gfx/gfx_gpu.c`, `src/pc/gfx/gfx_gpu.h` (the os30
  hardware-GPU Fast3D backend), `src/pc/of_voice.c`, `src/pc/of_voice.h`
  (hardware voice synthesis).
  Note that `gfx_gpu.{c,h}` sit **inside** `src/pc/gfx/`, which is governed by
  `src/pc/gfx/LICENSE.txt` (Emill / MaikelChan, source-redistribution only,
  binary redistribution forbidden).
- **Modified** — `src/pc/gfx/gfx_pc.c`, `src/pc/gfx/gfx_rendering_api.h`,
  `src/pc/pc_main.c`, `src/pc/mixer.c`, `src/pc/mixer.h`,
  `src/pc/ultra_reimplementation.c`, `src/pc/controller/controller_entry_point.c`,
  `src/audio/heap.c`, `src/audio/load.c`, `src/audio/synthesis.c`,
  `src/engine/graph_node.h`, `src/game/paintings.c`,
  `src/game/rendering_graph_node.c`, `src/game/shadow.c`.
- The openfpgaOS platform backends themselves live **outside** this directory,
  in `src/sm64/pocket/`.

A quick way to re-locate the touched files:

```sh
grep -rl TARGET_OPENFPGA src/sm64/sm64 --exclude-dir=build --exclude=UPSTREAM.md
```

Note that this finds only the files gated on the `TARGET_OPENFPGA` macro.
Some edits (e.g. in `src/audio/synthesis.c`, `src/game/shadow.c`) are
unconditional; the authoritative list is the one above.

## Re-syncing with upstream

There is no automated vendoring script. To update:

1. Clone the upstream fork out-of-tree:
   `git clone -b dos https://github.com/fgsfdsfgs/sm64-port /tmp/sm64-upstream`
2. **Identify the current base first.** Diff this tree against upstream commits
   on `dos` (e.g. `git log --oneline` plus `diff -ru` against a few candidates,
   ignoring the local modifications listed above) until one matches. If a match
   is found, record its SHA in the table at the top of this file — that removes
   the "not recoverable" caveat for future updates.
3. Copy the upstream tree over this directory, **excluding** `.git/`, then
   restore/re-apply the local modifications listed above.
4. Do not commit `baserom.us.z64`, `build/`, or any extracted asset. ROM media
   is supplied by the user at build time and is deliberately untracked.
5. Re-check `REUSE.toml` and the root `NOTICE`: new upstream files must not be
   swept into this project's Apache-2.0 claim. In particular watch for new
   `*.json`, `*.md` and `*.bin` files under this directory, which broad
   extension globs will otherwise capture.
6. Re-read `src/pc/gfx/LICENSE.txt` if the renderer changed. The
   binary-redistribution ban is what keeps this project source-only.
