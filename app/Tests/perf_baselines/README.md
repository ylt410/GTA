The unchanged `WasmFutex-build21.c` is the production global-queue implementation
from commit `140cf81a9cda33e12e65c5e4e827a183c68098c4`. Its SHA-256 is pinned by
`check_a17_efficiency.py`. Both versions run the same request/reply fixture and
compiler flags in ABBA order. This compares synchronization work on the host;
it does not measure the iPhone game, GPU utilization, or gameplay frame rate.
