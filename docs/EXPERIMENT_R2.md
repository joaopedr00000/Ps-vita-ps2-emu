# Experimental R2: automatic diagnostics and CPU/GS experiments

Install over R1 (VTPS2E001). No official release. Play normally; latest session writes perf.csv, hardware.csv, threads.csv, log.txt and a bounded GS trace after 30 s. CPU percentages are per thread, not per-core system utilization; affinity is the effective kernel mask, not a measurement of actual core residency. core3_app.txt now uses the experimental data directory. No additional kernel patch is introduced. 500 MHz is recorded if supplied by an external clock plugin, not forced.

Optional ux0:data/VitaPS2exp/experiment.ini (remove to use defaults):
```ini
diagnostic_logs=1
gs_diag=120
gs_diag_after=30
auto_tune=0
iop_thread=0
iop_core=1
threaded_vu1=1
threaded_spu=1
readback_finish=0
target_pixels_neon16=0
```

Repeat the same gameplay scene with iop_thread=1 (existing experimental parallel IOP, not a new dynarec), then 0. Independently test readback_finish=1 for corrupt graphics; it is expensive and may lower FPS. New NEON texture page-stamp scan preserves scalar semantics and wrapping. No confirmed FPS gain. Logs are local, flushed each second, and contain game paths and thread metadata. Copy logs after each run. To disable detailed automatic logging set diagnostic_logs=0. Scripted runs still log as before.
