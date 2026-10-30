# gbdemo: Game Boy (gbeink.mpy, Peanut-GB) on the Adafruit 4.2" SSD1683 panel with the
# balanced lag trick and dirty windows from eink-high-freq, plus an optional Mini I2C
# Gamepad. Compile with mpy-cross; code.py makes the buffers before importing this so
# they come from an unfragmented heap (see code.py).
#
# Panel method (eink-high-freq teapot_demo/teapot_win): 1-frame custom LUT at 125 Hz;
# RAM 0x24 holds frame N and 0x26 frame N-lag, so every pixel that changed in the last
# `lag` steps gets lag frames of drive, equal toward black and white. Only the box
# around this and the previous step's changes is written. Every `every` s the picture
# is drained to white over lag+2 steps and a balanced wipe runs; every `clean` s an OTP
# clean. The game pauses during both, so they are spaced out (30 s, 120 s) at the cost of
# a little more ghosting. The next frame is emulated while the panel is
# BUSY with the last one.
import board, busio, digitalio, gc, time, microcontroller
import gbeink
from micropython import const

W, H = 400, 300
STRIDE = W // 8
GX0, GY0 = 5, 6          # picture corner: RAM byte 5 (x 40), row 6; 320x288 at 2x

spi = busio.SPI(board.EPD_SCK, board.EPD_MOSI)
while not spi.try_lock():
    pass
spi.configure(baudrate=20_000_000)


def _out(pin, v):
    p = digitalio.DigitalInOut(pin)
    p.switch_to_output(v)
    return p


cs = _out(board.EPD_CS, True)
dc = _out(board.EPD_DC, True)
rst = _out(board.EPD_RESET, True)
busy = digitalio.DigitalInOut(board.EPD_BUSY)
CB = [bytes((i,)) for i in range(256)]
FULL_X = bytes([0, STRIDE - 1])
FULL_Y = bytes([0, 0, (H - 1) & 0xFF, (H - 1) >> 8])
NO_PINGPONG = bytes([0x00, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00])
VSS, VSH1, VSL = 0x00, 0x40, 0x80
FILLW = b"\xff" * (STRIDE * 30)
FILLB = bytes(STRIDE * 30)
_xy = bytearray(4)


def cmd(c, data=None):
    cs.value = False
    dc.value = False
    spi.write(CB[c])
    if data:
        dc.value = True
        spi.write(data)
    cs.value = True


def wb(limit=400000):
    i = 0
    while busy.value:
        i += 1
        if i > limit:
            return False
    return True


def refresh(mode):
    cmd(0x22, CB[mode])
    cmd(0x20)
    wb(10000000)


def init():
    rst.value = False
    time.sleep(0.01)
    rst.value = True
    time.sleep(0.01)
    wb(10000000)
    cmd(0x12)  # SW reset
    wb(10000000)
    cmd(0x3C, b"\x05")
    cmd(0x18, b"\x80")  # internal temp sensor
    cmd(0x11, b"\x03")  # X inc, Y inc
    cmd(0x44, FULL_X)
    cmd(0x45, FULL_Y)
    cmd(0x01, bytes([(H - 1) & 0xFF, (H - 1) >> 8, 0]))


def fill(c, chunk=FILLW):
    cmd(0x44, FULL_X); cmd(0x45, FULL_Y)
    cmd(0x4E, b"\x00"); cmd(0x4F, b"\x00\x00")
    cs.value = False
    dc.value = False
    spi.write(CB[c])
    dc.value = True
    for _ in range(H // 30):
        spi.write(chunk)
    cs.value = True


def make_lut(frames, to_black, to_white, fr):
    # datasheet Fig 6-7: LUTC, LUTWW, LUTBW, LUTWB, LUTBB x 6 groups x 7 bytes
    def one(vs):
        g = bytearray(42)
        g[0:7] = bytes([1, vs | frames, 0, 0, 0, 1, 0])
        return g
    lut = bytearray(227)
    lut[0:42] = one(VSS)
    lut[42:84] = one(VSS)
    lut[84:126] = one(to_black)
    lut[126:168] = one(to_white)
    lut[168:210] = one(VSS)
    lut[224] = fr
    return lut


def fast_mode(nf, fr):
    # voltages from Adafruit_EPD ThinkInk_420_Grayscale4_MFGN
    cmd(0x32, make_lut(nf, VSH1, VSL, fr))
    cmd(0x3F, b"\x07")
    cmd(0x03, b"\x17")
    cmd(0x04, b"\x41\xa8\x32")
    cmd(0x2C, b"\x30")
    cmd(0x21, b"\x00\x00")
    cmd(0x3C, b"\x80")
    cmd(0x37, NO_PINGPONG)
    refresh(0xCC)


def go():
    cmd(0x44, FULL_X); cmd(0x45, FULL_Y)
    cmd(0x22, b"\x0c"); cmd(0x20)


def wipe(k):
    # balanced: every pixel k frames toward black, then k toward white
    fill(0x24, FILLB); fill(0x26)
    for _ in range(k):
        wb(); go()
    wb()
    fill(0x24); fill(0x26, FILLB)
    for _ in range(k):
        wb(); go()
    wb()
    fill(0x26)


def otpclean(nf, fr):
    # OTP waveform only runs after a SW reset; 0x1A temp + 0x91 + 0xC7 takes 3.3 s
    init(); fill(0x24); fill(0x26)
    cmd(0x1A, b"\x5a\x00")
    refresh(0x91)
    refresh(0xC7)
    fast_mode(nf, fr)


def finish():
    init(); fill(0x24); fill(0x26); refresh(0xF7)
    cmd(0x10, b"\x01")  # deep sleep, HV off


def win(x0, x1, y0, y1):
    xy = _xy
    xy[0] = x0; xy[1] = x1
    cmd(0x44, memoryview(xy)[:2])
    xy[0] = y0 & 0xFF; xy[1] = y0 >> 8; xy[2] = y1 & 0xFF; xy[3] = y1 >> 8
    cmd(0x45, xy)
    cmd(0x4E, CB[x0])
    cmd(0x4F, memoryview(xy)[:2])


def send(st, out, mv, c, which):
    # one RAM write command, the box streamed through out a few lines at a time
    cs.value = False
    dc.value = False
    spi.write(CB[c])
    dc.value = True
    while True:
        sz = gbeink.plane(st, out, which)
        if not sz:
            break
        spi.write(mv[:sz])
    cs.value = True


# fps counter in the right margin (RAM bytes 45..48, x 360..391), outside the picture.
# Two 16x24 digits from eink-high-freq's fps_glyphs.py, shown with the same lag trick.
try:
    from fps_glyphs import GCH, GLY
except ImportError:
    GCH = None
CX0, CW, CY0, CH = 45, 4, 270, 24
CWIN = (CX0, CX0 + CW - 1, CY0, CY0 + CH - 1)


def render(text, dst):
    for ci, ch in enumerate(text):
        g = GCH.find(ch) * 48
        o = 2 * ci
        for r in range(CH):
            dst[o] = GLY[g]
            dst[o + 1] = GLY[g + 1]
            g += 2
            o += CW


# Mini I2C Gamepad QT (product 5743). Pins from the schematic (Gamepad Seesaw QT rev A)
# and the Learn guide, checked on hardware; buttons are active low with seesaw pull-ups.
# The docs give address 0x50, but the board here answers at 0x49 (the seesaw default),
# so find_pad() looks for product 5743 on both.
PAD_ADDRS = (0x50, 0x51, 0x52, 0x53, 0x49)
PAD_PRODUCT = const(5743)
P_SEL, P_B, P_Y, P_A, P_X, P_START = 0, 1, 2, 5, 6, 16
PAD_MASK = const(0x10067)
JOY_X, JOY_Y = 14, 15        # horizontal, vertical (the schematic's net names are swapped)
# Game Boy face: A right, B left. The pad's diamond has A right, B bottom, Y left, X top,
# so Y also gives B and X also gives A.
PAD_MAP = ((P_A, gbeink.JOYPAD_A), (P_X, gbeink.JOYPAD_A),
           (P_B, gbeink.JOYPAD_B), (P_Y, gbeink.JOYPAD_B),
           (P_START, gbeink.JOYPAD_START), (P_SEL, gbeink.JOYPAD_SELECT))
# raw ADC 0..1023, centre about 510/526. Measured: left 823, right 220, up 31, down 877.
JOY_LO_X, JOY_HI_X = gbeink.JOYPAD_RIGHT, gbeink.JOYPAD_LEFT
JOY_LO_Y, JOY_HI_Y = gbeink.JOYPAD_UP, gbeink.JOYPAD_DOWN
JOY_LO, JOY_HI = 320, 700
# Hold Select + Start (without A or B, which with them is Tetris's own soft reset)
# for EXIT_NS to end run().
EXIT_KEYS = gbeink.JOYPAD_SELECT | gbeink.JOYPAD_START
EXIT_MASK = EXIT_KEYS | gbeink.JOYPAD_A | gbeink.JOYPAD_B
EXIT_NS = 1_000_000_000


class Pad:
    def __init__(self, i2c, addr):
        self.i2c = i2c
        self.addr = addr
        self.c = bytearray(6)
        self.r = bytearray(2)
        self.g = bytearray(4)
        self.a = bytearray(2)
        self.jx = self.jy = 512
        self.axis = 0
        for reg in (0x03, 0x0B, 0x05):      # DIRCLR, PULLENSET, BULK_SET: INPUT_PULLUP
            c = self.c
            c[0] = 1; c[1] = reg
            c[2] = (PAD_MASK >> 24) & 255; c[3] = (PAD_MASK >> 16) & 255
            c[4] = (PAD_MASK >> 8) & 255; c[5] = PAD_MASK & 255
            self.i2c.writeto(self.addr, c)

    def read(self):
        # buttons every call, one joystick axis per call (the ADC needs the longer wait)
        r = self.r
        r[0] = 1; r[1] = 4
        self.i2c.writeto(self.addr, r)
        microcontroller.delay_us(100)       # 0-250 us all read cleanly on hardware
        self.i2c.readfrom_into(self.addr, self.g)
        g = self.g
        bits = ~((g[1] << 16) | (g[2] << 8) | g[3])
        r[0] = 9; r[1] = 7 + (JOY_X if self.axis == 0 else JOY_Y)
        self.i2c.writeto(self.addr, r)
        microcontroller.delay_us(250)       # 100-500 us gave the same value
        self.i2c.readfrom_into(self.addr, self.a)
        v = (self.a[0] << 8) | self.a[1]
        if self.axis == 0:
            self.jx = v
        else:
            self.jy = v
        self.axis ^= 1
        m = 0
        for pin, key in PAD_MAP:
            if bits & (1 << pin):
                m |= key
        if self.jx < JOY_LO:
            m |= JOY_LO_X
        elif self.jx > JOY_HI:
            m |= JOY_HI_X
        if self.jy < JOY_LO:
            m |= JOY_LO_Y
        elif self.jy > JOY_HI:
            m |= JOY_HI_Y
        return m

    def deinit(self):
        self.i2c.unlock()
        self.i2c.deinit()


def find_pad():
    i2c = busio.I2C(board.SCL, board.SDA, frequency=400_000)
    while not i2c.try_lock():
        pass
    found = i2c.scan()
    v = bytearray(4)
    for addr in PAD_ADDRS:
        if addr not in found:
            continue
        try:
            i2c.writeto(addr, b"\x00\x02")     # seesaw STATUS VERSION: product << 16
            time.sleep(0.005)
            i2c.readfrom_into(addr, v)
            if (v[0] << 8) | v[1] == PAD_PRODUCT:
                p = Pad(i2c, addr)
                p.read()
                print("gamepad at 0x%02x" % addr)
                return p
        except OSError:
            pass
    print("no gamepad, I2C devices:", [hex(a) for a in found])
    i2c.unlock()
    i2c.deinit()
    return None


def run(rom, ring, st, out, rom_path="/Tetris.gb", lag=6, nf=1, fr=0x05, secs=0, every=30,
        k=8, clean=120, cpu=200_000_000, pad_every=2, fps=True, fin=True):
    """Play until secs elapse (0 = forever) or Select + Start is held 1 s. rom: list of 16 KiB bytearrays, ring: list of
    at least lag + 1 bytearrays of gbeink.FRAME_SIZE, st: STATE_SIZE, out: a few KB."""
    if len(st) < gbeink.STATE_SIZE:
        raise RuntimeError("state buffer too small, need %d" % gbeink.STATE_SIZE)
    f = open(rom_path, "rb")
    for b in rom:
        f.readinto(b)
    f.close()
    e = gbeink.init(st, rom)
    if e:
        raise RuntimeError("gbeink.init error %d" % e)
    gbeink.set_ring(st, ring[:lag + 1], GX0, GY0)
    mv = memoryview(out)
    pad = find_pad()
    fps = fps and GCH is not None
    cblank = b"\xff" * (CW * CH)
    c_new = bytearray(cblank)
    c_old = bytearray(cblank)
    c_age = lag + 1
    print("gamepad", "found" if pad else "absent", "free", gc.mem_free())
    if cpu:
        microcontroller.cpu.frequency = cpu
    init(); fill(0x24); fill(0x26); refresh(0xF7)
    fast_mode(nf, fr)
    if k > 0:
        wipe(k)
    gc.collect()
    n = gbf = wipes = cleans = nwin = 0
    drain = 0
    keys = seen = 0
    t_exit = 0
    t0 = time.monotonic_ns()
    tend = t0 + secs * 1000000000
    twipe = t0 + every * 1000000000
    tclean = t0 + clean * 1000000000
    tlast = t0
    nlast = 0
    t_cnt = t0
    n_cnt = 0
    try:
        while True:
            n += 1
            if drain:
                gbeink.blank(st)
            else:
                if pad and n % pad_every == 0:
                    try:
                        keys = pad.read()
                    except OSError:
                        keys = 0
                    seen |= keys
                    gbeink.set_joypad(st, keys)
                    if keys & EXIT_MASK == EXIT_KEYS:
                        tn = time.monotonic_ns()
                        if not t_exit:
                            t_exit = tn
                        elif tn - t_exit >= EXIT_NS:
                            print("exit combo")
                            break
                    else:
                        t_exit = 0
                gbeink.run_frames(st, 1)
                gbf += 1
            box = gbeink.step(st)
            wb()
            if box:
                x0, x1, y0, y1 = box
                win(x0, x1, y0, y1)
                send(st, out, mv, 0x26, 0)
                win(x0, x1, y0, y1)
                send(st, out, mv, 0x24, 1)
                nwin += 1
            cnt = fps and c_age <= lag
            if cnt:
                win(*CWIN)
                cmd(0x26, c_old if c_age < lag else c_new)
                win(*CWIN)
                cmd(0x24, c_new)
                c_age += 1
            if box or cnt:
                go()
            if drain:
                drain -= 1
                if drain == 0:
                    wb(); wipe(k); wipes += 1
                    gbeink.reset(st)
                    c_old[:] = cblank       # the wipe erased the counter: draw it again
                    c_age = 0
                    twipe = time.monotonic_ns() + every * 1000000000
                    t_cnt, n_cnt = time.monotonic_ns(), n
                continue
            if n & 31 == 0:
                tn = time.monotonic_ns()
                if fps and tn - t_cnt >= 1000000000:
                    v = int((n - n_cnt) * 1e9 / (tn - t_cnt) + 0.5)
                    c_old[:] = c_new
                    render("%2d" % min(v, 99), c_new)
                    c_age = 0
                    t_cnt, n_cnt = tn, n
                if tn - tlast > 5000000000:
                    print("%.1f steps/s, %d GB frames, keys seen %02x" %
                          ((n - nlast) * 1e9 / (tn - tlast), gbf, seen))
                    tlast, nlast = tn, n
                    seen = 0
                if secs and tn > tend:
                    break
                if clean and tn > tclean:
                    wb(); otpclean(nf, fr); wipe(k); cleans += 1
                    gbeink.reset(st)
                    c_old[:] = cblank
                    c_age = 0
                    twipe = time.monotonic_ns() + every * 1000000000
                    tclean = time.monotonic_ns() + clean * 1000000000
                    t_cnt, n_cnt = time.monotonic_ns(), n
                elif k > 0 and tn > twipe:
                    drain = lag + 2
    finally:
        wb()
        dt = (time.monotonic_ns() - t0) / 1e6 / n
        microcontroller.cpu.frequency = 125_000_000
        if pad:
            pad.deinit()
        if fin:
            time.sleep(1)
            finish()
        print("gbdemo lag %d: %d steps (%d windowed), %d GB frames, %d wipes, %d cleans,"
              " %.1f ms/step = %.1f steps/s" % (lag, n, nwin, gbf, wipes, cleans, dt,
                                                1000 / dt), gbeink.error(st))
