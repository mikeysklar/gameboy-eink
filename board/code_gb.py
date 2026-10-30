# code.py for the Game Boy demo: buffers first, on a fresh heap, then the code.
# gbeink.mpy (about 21 KB) and the buffers need big blocks the heap no longer has
# once other code is loaded.
import gc
import gbeink
ROM = [bytearray(16384) for _ in range(2)]          # 32 KiB cart (Tetris)
RING = [bytearray(gbeink.FRAME_SIZE) for _ in range(7)]
ST = bytearray(gbeink.STATE_SIZE)
OUT = bytearray(2400)
gc.collect()
import gbdemo
gbdemo.run(ROM, RING, ST, OUT, "/Tetris.gb")
