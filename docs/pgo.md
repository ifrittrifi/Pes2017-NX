# LTO and PGO (after BO2-360-NX)

Build options (build-switch.sh environment, or Actions > "Build Switch NRO (full rebuild, manual)" > Run workflow):

- `PES17_PGO=generate`: training build. The generated PPC code and the renderer are instrumented.
  Play entrance, a match (long passes, shots), a goal celebration and a replay, then hold
  **ZL + ZR + Minus**. The counters are written to `sdmc:/switch/pes2017/pgo/` (wait until
  `WRITTEN.txt` appears; it can take a minute). The build is slower than the normal one.
- `PES17_PGO=use`, `PES17_PGO_DIR=<folder>`: build with that profile. In CI the folder is
  `pgo/pgo.zip` of the repo (the files of the folder above, zipped).
- `PES17_LTO=ON`: link-time optimization of the PPC objects. It needs a lot of memory at the link
  and may fail on a small runner; PGO is the larger gain (about 9 % in BO2).

A profile only fits the code it was trained on: after the PPC output or these flags change, train again.
