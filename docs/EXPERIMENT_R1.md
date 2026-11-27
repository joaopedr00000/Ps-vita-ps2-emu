# General performance R1
Separate test app VTPS2E001 and data directory ux0:data/VitaPS2exp. No upstream release.
GS16 NEON conversion is off by default; set target_pixels_neon16=1 in autostart.txt to run differential check and ABBA microbenchmark. Full GS RAM comparison and skip redundant scalar indexes. WSOLA sliding energy, bound input position, compact backlog, clean reset. Auto-tuner accepts finite slow samples and rejects baseline drift. Test on two games, warm JIT before repeated runs. Preserve perf.csv, selftest.txt, screenshots. No measured FPS gain yet.
