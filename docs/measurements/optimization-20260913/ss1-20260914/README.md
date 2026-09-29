SS1 SM64 measurements, 14 September 2026

`comparison.json` summarizes two baseline and two optimized captures on the same
100 MHz full-feature MiSTer core. `state-comparison.json` checks all 4,800
recorded game/camera states for each run against the fresh software baseline.

Each run contains three compressed raw save-slot traces plus metadata and
per-scene summaries. The header gives format version, CPU frequency and
HW_FEATURES. Diagnostic V1 has no physical-presentation counters; V2 does.
The parser records the exact column names in each summary. Reproduce a result
from this directory with `python3 analyze.py full-optimized-100-b`.

The first 90 and last 30 simulation ticks of every active demo are excluded
consistently from steady statistics; full scenes and transitions remain in
the trace. The profiling capture is excluded from the A/B FPS average.

`diagnostic-artifacts-v1.json` and `diagnostic-artifacts-v2.json` identify the
private instrumented ELFs. `final-builds.json` identifies the later normal and
profiling production builds including the portable save alias. The diagnostic
ELFs and user ROM assets are not included.
