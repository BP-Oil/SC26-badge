#!/usr/bin/env python3
"""Boot-trace emulator for an RP2350 (ARM) pico-sdk firmware image.

Runs the firmware from reset in Unicorn (Cortex-M33) with a model of just enough
RP2350 peripherals to get through init, and logs every write to pin/peripheral
registers: IO_BANK0 function select, pads, SIO GPIO, SPI, I2C, UART, PIO, PWM, DMA.
"""
import struct, sys, collections, json, os
from PIL import Image
from unicorn import *
from unicorn.arm_const import *

FW_PATH = sys.argv[1] if len(sys.argv) > 1 else "fw.bin"
MAX_INSNS = int(sys.argv[2]) if len(sys.argv) > 2 else 40_000_000
FW = open(FW_PATH, "rb").read()

FLASH, SRAM, SRAM_SZ = 0x10000000, 0x20000000, 0x82000
ROM_LOOKUP_STUB = 0x00007000
ROM_FN_BASE = 0x00007100

log = []            # (kind, pc, details)
def L(kind, **kw):
    kw["pc"] = hex(cur_pc[0]); log.append((kind, kw))

cur_pc = [0]
regs = collections.defaultdict(int)      # canonical peripheral register file
sio = dict(out=0, oe=0, hi_out=0, hi_oe=0)
gpio_in = 0xFFFFFFFF                       # all inputs high (buttons released, pull-ups)
fifo = collections.deque()
now_us = [1_000_000]
spi_rx = collections.defaultdict(int)
i2c_rx = collections.defaultdict(int)
rom_fns = {}

PERIPH = {
    0x40000000: "SYSINFO", 0x40008000: "SYSCFG", 0x40010000: "CLOCKS", 0x40018000: "PSM",
    0x40020000: "RESETS", 0x40028000: "IO_BANK0", 0x40030000: "IO_QSPI", 0x40038000: "PADS_BANK0",
    0x40040000: "PADS_QSPI", 0x40048000: "XOSC", 0x40050000: "PLL_SYS", 0x40058000: "PLL_USB",
    0x40060000: "ACCESSCTRL", 0x40068000: "BUSCTRL", 0x40070000: "UART0", 0x40078000: "UART1",
    0x40080000: "SPI0", 0x40088000: "SPI1", 0x40090000: "I2C0", 0x40098000: "I2C1",
    0x400a0000: "ADC", 0x400a8000: "PWM", 0x400b0000: "TIMER0", 0x400b8000: "TIMER1",
    0x400c0000: "HSTX_CTRL", 0x400c8000: "XIP_CTRL", 0x400d0000: "XIP_QMI", 0x400d8000: "WATCHDOG",
    0x400e0000: "BOOTRAM", 0x400e8000: "ROSC", 0x400f0000: "TRNG", 0x400f8000: "SHA256",
    0x40100000: "POWMAN", 0x40108000: "TICKS", 0x40120000: "OTP", 0x40130000: "OTP_DATA",
    0x50000000: "DMA", 0x50100000: "USB_DPRAM", 0x50110000: "USB", 0x50200000: "PIO0",
    0x50300000: "PIO1", 0x50400000: "PIO2", 0x50500000: "XIP_AUX", 0x50600000: "HSTX_FIFO",
}
def periph_of(a):
    if 0x50100000 <= a < 0x50110000: return 0x50100000, "USB_DPRAM"
    base = a & ~0x7FFF if a < 0x50000000 else a & ~0xFFFFF
    if a >= 0x50000000 and base not in PERIPH: base = a & ~0x7FFF
    return base, PERIPH.get(base, "UNK_%08x" % base)

# ---------------------------------------------------------------- APB/AHB MMIO
def canon(addr):
    """Strip RP2350 atomic alias (XOR +0x1000, SET +0x2000, CLR +0x3000)."""
    if 0x50100000 <= addr < 0x50110000 or 0x40130000 <= addr < 0x40140000:
        return addr, 0
    return addr & ~0x3000, (addr >> 12) & 3

def periph_read(addr, size):
    a, _ = canon(addr)
    base, name = periph_of(a); off = a - base
    v = regs[a]
    if name == "SYSINFO" and off == 0: return 0x20004927
    if name == "SYSINFO" and off == 4: return 1
    if name == "CLOCKS" and off == 0x38: return 1 << (regs[base + 0x30] & 3)
    if name == "CLOCKS" and off == 0x44: return 1 << (regs[base + 0x3C] & 1)
    if name == "CLOCKS" and off < 0x78 and off % 12 == 8: return 1
    if name == "CLOCKS" and off >= 0x80: return 0x10 | (v & ~0)  # FC0 done-ish
    if name == "RESETS" and off == 8: return 0xFFFFFFFF
    if name == "XOSC" and off == 4: return 0x80001000
    if name in ("PLL_SYS", "PLL_USB") and off == 0: return v | 0x80000000
    if name == "ROSC" and off == 0x18: return 0x80001000
    if name in ("TIMER0", "TIMER1"):
        if off in (0x28, 0x0C): now_us[0] += 50; return now_us[0] & 0xFFFFFFFF
        if off in (0x24, 0x08): return now_us[0] >> 32
        if off == 0x20: return 0
    if name in ("UART0", "UART1"):
        if off == 0x18: return 0x90
        if off == 0x00: return 0
    if name in ("SPI0", "SPI1"):
        if off == 0x0C: return 0x3 | (4 if spi_rx[name] else 0)
        if off == 0x08:
            if spi_rx[name]: spi_rx[name] -= 1
            return 0
    if name in ("I2C0", "I2C1"):
        if off == 0x70: return 0x6 | (8 if i2c_rx[name] else 0)
        if off == 0x34: return 0x210
        if off in (0x74,): return 0
        if off == 0x78: return i2c_rx[name]
        if off == 0x80: return 0
        if off == 0x9C: return regs[base + 0x6C] & 1
        if off == 0x10:
            if i2c_rx[name]: i2c_rx[name] -= 1
            return 0xFF
        if 0x40 <= off <= 0x6C and off not in (0x6C,): return 0   # IC_CLR_* reads
    if name == "ADC":
        if off == 0: return v | 0x100
        if off == 4: return 0x800
    if name.startswith("PIO"):
        if off == 0x04: return 0x0F000F00
    if name == "DMA" and off in (0x40C, 0x41C): return dma_ints[(off >> 4) & 1]
    if name == "DMA" and off in (0x400, 0x410): return dma_ints[(off >> 4) & 1]
    if name == "DMA":
        ch_off = off & 0x3F
        if off < 0x400 and ch_off in (0x0C, 0x10, 0x20, 0x30): return v & ~(1 << 26)
        if off < 0x400 and ch_off in (0x08,): return 0
    if name == "WATCHDOG" and off == 0x08: return 0
    if name == "BOOTRAM" and 0x80C <= off < 0x830: return 1
    if name == "BOOTRAM" and off < 0x800: return 0x47704770     # bootrom's XIP-setup stub -> "bx lr"
    if name == "USB" and off == 0x50: return 0
    return v

def periph_write(addr, size, value):
    a, op = canon(addr)
    old = regs[a]
    new = [value, old ^ value, old | value, old & ~value][op]
    regs[a] = new & 0xFFFFFFFF
    base, name = periph_of(a); off = a - base
    opn = ["", "^=", "|=", "&=~"][op]
    if name == "IO_BANK0" and off < 0x180 and off % 8 == 4:
        L("FUNCSEL", gpio=off // 8, funcsel=new & 0x1F, ctrl=hex(new))
    elif name == "PADS_BANK0" and 4 <= off < 0xC4:
        L("PAD", gpio=(off - 4) // 4, val=hex(new))
    elif name in ("SPI0", "SPI1"):
        if off == 0x08:
            spi_rx[name] += 1
            L("SPI_DR", spi=name, byte=value & 0xFFFF, gpio_out=hex(sio["out"]), gpio_hi=hex(sio["hi_out"]))
        else:
            L("SPI_REG", spi=name, off=hex(off), op=opn, val=hex(new))
    elif name in ("I2C0", "I2C1"):
        if off == 0x10:
            if value & 0x100: i2c_rx[name] += 1
            L("I2C_DATA", i2c=name, cmd=hex(value))
        else:
            L("I2C_REG", i2c=name, off=hex(off), val=hex(new))
    elif name in ("UART0", "UART1"):
        if off == 0:
            L("UART_TX", uart=name, ch=value & 0xFF)
        else:
            L("UART_REG", uart=name, off=hex(off), val=hex(new))
    elif name.startswith("PIO"):
        if name == "PIO1" and off == 0x10:
            lcd_fifo(value if size == 1 else (value >> 24))
        L("PIO", pio=name, off=hex(off), op=opn, val=hex(new))
        if name == "PIO1" and off == 0x10 and len(log) > 20000: log.pop()
    elif name == "DMA":
        dma_write(off, new)
        L("DMA", off=hex(off), op=opn, val=hex(new))
    elif name in ("PWM", "ADC", "HSTX_CTRL", "HSTX_FIFO"):
        L(name, off=hex(off), op=opn, val=hex(new))
    elif name in ("CLOCKS", "PLL_SYS", "PLL_USB", "XOSC"):
        L("CLK", blk=name, off=hex(off), val=hex(new))

# ---------------------------------------------------------------- SIO
snap_at = sorted(float(x) for x in os.environ.get("LCD_SNAP_AT", "").split(",") if x)   # emulated seconds
def apply_buttons():
    global gpio_in
    while snap_at and now_us[0] / 1e6 >= snap_at[0]:
        t = snap_at.pop(0)
        Image.frombytes("RGB", (LCD_W, LCD_H), bytes(lcd_fb)).save("%s_at%05.2f.png" % (os.environ.get("LCD_SNAPS", "lcd"), t))
    while button_events and now_us[0] / 1e6 >= button_events[0][0]:
        t, pin, down = button_events.pop(0)
        gpio_in = (gpio_in & ~(1 << pin)) if down else (gpio_in | (1 << pin))
        print("  button GP%d %s @%.2fs" % (pin, "down" if down else "up", t))

def sio_read(off, size):
    apply_buttons()
    if off == 0x000: return 0
    if off == 0x004: return gpio_in
    if off == 0x008: return 0xFFFF
    if off == 0x010: return sio["out"]
    if off == 0x014: return sio["hi_out"]
    if off == 0x030: return sio["oe"]
    if off == 0x034: return sio["hi_oe"]
    if off == 0x050: return 0x2 | (1 if fifo else 0)
    if off == 0x058: return fifo.popleft() if fifo else 0
    if 0x100 <= off < 0x180: return 1 << ((off - 0x100) // 4)
    return regs[0xD0000000 + off]

def sio_write(off, size, v):
    names = {0x010: ("out", "="), 0x018: ("out", "|"), 0x020: ("out", "&~"), 0x028: ("out", "^"),
             0x014: ("hi_out", "="), 0x01C: ("hi_out", "|"), 0x024: ("hi_out", "&~"), 0x02C: ("hi_out", "^"),
             0x030: ("oe", "="), 0x038: ("oe", "|"), 0x040: ("oe", "&~"), 0x048: ("oe", "^"),
             0x034: ("hi_oe", "="), 0x03C: ("hi_oe", "|"), 0x044: ("hi_oe", "&~"), 0x04C: ("hi_oe", "^")}
    if off in names:
        k, op = names[off]; o = sio[k]
        sio[k] = {"=": v, "|": o | v, "&~": o & ~v, "^": o ^ v}[op] & 0xFFFFFFFF
        L("SIO_" + k.upper(), op=op, mask=hex(v), now=hex(sio[k]))
        if k == "out": lcd_cs((sio["out"] >> 15) & 1)
    elif off == 0x054:
        fifo.append(v); L("FIFO_PUSH", val=hex(v))
    else:
        regs[0xD0000000 + off] = v

# ---------------------------------------------------------------- PPB
def ppb_read(off, size): return regs[0xE0000000 + off]
def ppb_write(off, size, v):
    regs[0xE0000000 + off] = v
    if off == 0xED08: L("VTOR", val=hex(v))


# ---------------------------------------------------------------- virtual ST77916 + DMA + IRQ
LCD_W = LCD_H = 360
lcd_fb = bytearray(LCD_W * LCD_H * 3)
lcd = dict(cs=1, buf=[], win=[0, 0, 359, 359], cmds=[], pixels=0)
def lcd_cs(level):
    if level == 0 and lcd["cs"] == 1: lcd["buf"] = []
    if level == 1 and lcd["cs"] == 0: lcd_txn(lcd["buf"])
    lcd["cs"] = level
def single(raw):
    bits = []
    for b in raw: bits += [(b >> 4) & 1, b & 1]
    return [int("".join(map(str, bits[i:i + 8])), 2) for i in range(0, len(bits) - 7, 8)]
def lcd_txn(raw):
    if len(raw) < 16: return
    hdr = single(raw[:16])
    if hdr[0] == 0x02:
        cmd = hdr[2]; params = single(raw[16:])
        lcd["cmds"].append((cmd, params))
        if cmd == 0x2A and len(params) >= 4: lcd["win"][0], lcd["win"][2] = params[0] << 8 | params[1], params[2] << 8 | params[3]
        if cmd == 0x2B and len(params) >= 4: lcd["win"][1], lcd["win"][3] = params[0] << 8 | params[1], params[2] << 8 | params[3]
    elif hdr[0] == 0x32 and hdr[2] == 0x2C:
        x0, y0, x1, y1 = lcd["win"]; w = x1 - x0 + 1
        data = raw[16:]
        for i in range(0, len(data) - 1, 2):
            p = data[i] << 8 | data[i + 1]; n = i // 2
            x, y = x0 + n % w, y0 + n // w
            if 0 <= x < LCD_W and 0 <= y < LCD_H:
                o = (y * LCD_W + x) * 3
                lcd_fb[o:o + 3] = bytes((((p >> 11) & 31) * 255 // 31, ((p >> 5) & 63) * 255 // 63, (p & 31) * 255 // 31))
        lcd["pixels"] += len(data) // 2
        snap = os.environ.get("LCD_SNAPS")
        every = float(os.environ.get("LCD_SNAP_MS", "0")) * 1000
        due = every and now_us[0] >= lcd.get("next_snap", 0)
        if due: lcd["next_snap"] = now_us[0] + every
        if snap and (len(data) > 20000 or due):
            lcd["snaps"] = lcd.get("snaps", 0) + 1
            Image.frombytes("RGB", (LCD_W, LCD_H), bytes(lcd_fb)).save("%s_%03d.png" % (snap, lcd["snaps"]))
        lcd["cmds"].append((0x2C, len(data) // 2))
def lcd_fifo(byte):
    if lcd["cs"] == 0: lcd["buf"].append(byte & 0xFF)

pending_irqs = []
DMA_ALIAS = {0x00: "r", 0x04: "w", 0x08: "n", 0x0C: "c", 0x10: "c", 0x14: "r", 0x18: "w", 0x1C: "n",
             0x20: "c", 0x24: "n", 0x28: "r", 0x2C: "w", 0x30: "c", 0x34: "w", 0x38: "n", 0x3C: "r"}
dma_ch = collections.defaultdict(lambda: dict(r=0, w=0, n=0, c=0))
dma_ints = [0, 0]
def dma_write(off, value):
    if off < 0x400:
        ch, o = off >> 6, off & 0x3F
        dma_ch[ch][DMA_ALIAS[o]] = value
        if o in (0x0C, 0x1C, 0x2C, 0x3C): dma_trigger(ch)
    elif off == 0x430:  # MULTI_CHAN_TRIGGER
        for ch in range(16):
            if value & (1 << ch): dma_trigger(ch)
    elif off in (0x40C, 0x41C):  # INTS0/1 write-1-to-clear
        dma_ints[(off >> 4) & 1] &= ~value
def dma_trigger(ch):
    c = dma_ch[ch]
    if not (c["c"] & 1): return
    count = c["n"] & 0x0FFFFFFF
    if not count: return
    size = 1 << ((c["c"] >> 2) & 3)
    data = bytes(mu.mem_read(c["r"], count * size)) if c["c"] & 0x10 else None
    if c["w"] == 0x50300010 and data is not None:
        for i in range(0, len(data), size):
            lcd_fifo(data[i] if size == 1 else data[i + size - 1])
    L("DMA_XFER", ch=ch, read=hex(c["r"]), write=hex(c["w"]), count=count, size=size)
    for k, (inte_off, irq) in enumerate(((0x404, 10), (0x414, 11))):
        if regs[0x50000000 + inte_off] & (1 << ch):
            dma_ints[k] |= 1 << ch; pending_irqs.append(irq)

# ---------------------------------------------------------------- setup
mu = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
mu.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_M33)
mu.mem_map(0, 0x8000)
try:
    mu.reg_write(UC_ARM_REG_C1_C0_2, 0x00F00000); mu.reg_write(UC_ARM_REG_FPEXC, 0x40000000)
except Exception as e: print("fpu enable:", e)
mu.mem_map(FLASH, 0x1000000)
mu.mem_write(FLASH, FW)
flash_ops = [0, 0]
FS_OFF = int(os.environ.get("FS_OFF", "0x400000"), 16)
# BUTTONS="t:pin:down,..." e.g. "2.0:20:1,3.6:20:0" holds CENTER (GP20) from 2.0 s to 3.6 s (emulated time)
button_events = sorted([(float(t), int(p), int(v)) for t, p, v in
                        (e.split(":") for e in os.environ.get("BUTTONS", "").split(",") if e)])
if os.environ.get("SETTINGS_IMG"):
    mu.mem_write(FLASH + 0x3FF000, open(os.environ["SETTINGS_IMG"], "rb").read())
if os.environ.get("FS_IMG"):
    mu.mem_write(FLASH + FS_OFF, open(os.environ["FS_IMG"], "rb").read())
for alias in (0x14000000, 0x18000000, 0x1C000000):
    pass
# untranslated / uncached XIP aliases (read-only copies of flash as loaded)
for _alias in (0x14000000, 0x1C000000):
    mu.mem_map(_alias, 0x1000000)
    mu.mem_write(_alias, bytes(mu.mem_read(FLASH, 0x1000000)))
mu.mem_map(SRAM, SRAM_SZ)
mu.mem_map(0x18000000, 0x4000000)   # XIP cache-maintenance window (writes ignored)
mu.mem_map(0x20082000, 0x2000)  # scratch_x/y end guard
mu.mmio_map(0x40000000, 0x200000, lambda uc, o, s, d: periph_read(0x40000000 + o, s), None,
            lambda uc, o, s, v, d: periph_write(0x40000000 + o, s, v), None)
mu.mmio_map(0x50000000, 0x700000, lambda uc, o, s, d: periph_read(0x50000000 + o, s), None,
            lambda uc, o, s, v, d: periph_write(0x50000000 + o, s, v), None)
mu.mmio_map(0xD0000000, 0x10000, lambda uc, o, s, d: sio_read(o, s), None,
            lambda uc, o, s, v, d: sio_write(o, s, v), None)
mu.mmio_map(0xE0000000, 0x100000, lambda uc, o, s, d: ppb_read(o, s), None,
            lambda uc, o, s, v, d: ppb_write(o, s, v), None)

# Fake bootrom: lookup function pointer at 0x16 and 0x18 -> our stub
rom = bytearray(b"\x00\xbe" * 0x4000)        # bkpt everywhere
rom[0x10:0x13] = b"Mu\x02"
struct.pack_into("<H", rom, 0x16, ROM_LOOKUP_STUB | 1)
struct.pack_into("<H", rom, 0x18, ROM_LOOKUP_STUB | 1)
ROM_DATA = 0x7800   # fake ROM data: pointer -> flash devinfo16 (CS0 = 16 MB)
struct.pack_into("<I", rom, ROM_DATA, ROM_DATA + 0x10)
struct.pack_into("<H", rom, ROM_DATA + 0x10, 0x0C00)
mu.mem_write(0, bytes(rom))

def hook_rom(uc, addr, size, ud):
    lr = uc.reg_read(UC_ARM_REG_LR)
    if addr == ROM_LOOKUP_STUB and (uc.reg_read(UC_ARM_REG_R1) & 0x40):    # RT_FLAG_DATA lookup
        L("ROMDATA", code=struct.pack("<H", uc.reg_read(UC_ARM_REG_R0) & 0xFFFF).decode("latin1"))
        uc.reg_write(UC_ARM_REG_R0, ROM_DATA)
        uc.reg_write(UC_ARM_REG_PC, lr); return
    if addr == ROM_LOOKUP_STUB:
        code = uc.reg_read(UC_ARM_REG_R0)
        if code not in rom_fns: rom_fns[code] = ROM_FN_BASE + 4 * len(rom_fns)
        uc.reg_write(UC_ARM_REG_R0, rom_fns[code] | 1)
    else:
        code = {v: k for k, v in rom_fns.items()}.get(addr, 0)
        nm = struct.pack("<H", code & 0xFFFF).decode("latin1")
        r0, r1, r2 = (uc.reg_read(r) for r in (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2))
        if nm == "RE":      # flash_range_erase(offset, count, block_size, block_cmd)
            uc.mem_write(FLASH + r0, b"\xff" * r1); flash_ops[0] += 1
        elif nm == "RP":    # flash_range_program(offset, data, count)
            uc.mem_write(FLASH + r0, bytes(uc.mem_read(r1, r2))); flash_ops[1] += 1
        else:
            L("ROMCALL", fn=nm, r0=hex(r0), r1=hex(r1))
        uc.reg_write(UC_ARM_REG_R0, 0)
    uc.reg_write(UC_ARM_REG_PC, lr)
mu.hook_add(UC_HOOK_CODE, hook_rom, begin=ROM_LOOKUP_STUB, end=ROM_FN_BASE + 0x400)

EXC_RET = 0x00007E00
in_irq = [0]
def inject_irq(uc):
    irq = pending_irqs.pop(0)
    vtor = regs[0xE000ED08] or 0x10000000
    handler = struct.unpack("<I", bytes(uc.mem_read(vtor + 4 * (16 + irq), 4)))[0]
    sp = uc.reg_read(UC_ARM_REG_SP) - 32
    frame = [uc.reg_read(r) for r in (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3,
                                      UC_ARM_REG_R12, UC_ARM_REG_LR)] + [uc.reg_read(UC_ARM_REG_PC), uc.reg_read(UC_ARM_REG_XPSR)]
    uc.mem_write(sp, struct.pack("<8I", *frame))
    uc.reg_write(UC_ARM_REG_SP, sp)
    uc.reg_write(UC_ARM_REG_LR, EXC_RET | 1)
    uc.reg_write(UC_ARM_REG_PC, handler)
    in_irq[0] += 1
def exc_return(uc, addr, size, ud):
    sp = uc.reg_read(UC_ARM_REG_SP)
    r0, r1, r2, r3, r12, lr, pc, xpsr = struct.unpack("<8I", bytes(uc.mem_read(sp, 32)))
    for r, v in zip((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_R12, UC_ARM_REG_LR), (r0, r1, r2, r3, r12, lr)):
        uc.reg_write(r, v)
    uc.reg_write(UC_ARM_REG_SP, sp + 32)
    uc.reg_write(UC_ARM_REG_XPSR, xpsr)
    uc.reg_write(UC_ARM_REG_PC, pc | 1)
    in_irq[0] -= 1
mu.mem_write(EXC_RET, b"\x00\xbf" * 0x40)
mu.hook_add(UC_HOOK_CODE, exc_return, begin=EXC_RET, end=EXC_RET + 2)
mu.mem_protect(0, 0x8000, UC_PROT_READ | UC_PROT_EXEC)   # boot ROM is read-only on real silicon

insn_count = [0]
STUCK_LIMIT = int(os.environ.get('STUCK_LIMIT', '1500000'))
pc_hist = collections.Counter()
PROF_FN = int(os.environ.get("PROFILE_FN", "0"), 16)
TRACE_OPEN = int(os.environ.get("TRACE_OPEN", "0"), 16)     # address of a fn whose r0 is a C string
opened = []
prof = dict(icount=0, start=None, ret=None, samples=[], entries=[])
if PROF_FN:
    def _cnt(uc, a, sz, ud): prof["icount"] += 1
    mu.hook_add(UC_HOOK_CODE, _cnt)
def hook_block(uc, addr, size, ud):
    cur_pc[0] = addr
    insn_count[0] += 1
    if TRACE_OPEN and addr == TRACE_OPEN:
        raw = bytes(uc.mem_read(uc.reg_read(UC_ARM_REG_R0), 96)).split(b"\0")[0].decode("latin1")
        opened.append((round(now_us[0] / 1e6, 2), raw)); print("  open @%.2fs: %s" % opened[-1])
    if PROF_FN:
        if addr == PROF_FN and prof["start"] is None:
            prof["start"] = prof["icount"]; prof["ret"] = uc.reg_read(UC_ARM_REG_LR) & ~1
            prof["entries"].append(prof["icount"])
        elif prof["start"] is not None and addr == prof["ret"]:
            prof["samples"].append(prof["icount"] - prof["start"]); prof["start"] = None
    pc_hist[addr] += 1
    if pc_hist[addr] > STUCK_LIMIT:
        print("stuck in loop at %08x" % addr); uc.emu_stop()
    if insn_count[0] % 200000 == 0:
        now_us[0] += 1000
    if pending_irqs and not in_irq[0] and (uc.reg_read(UC_ARM_REG_PRIMASK) & 1) == 0:
        inject_irq(uc)
mu.hook_add(UC_HOOK_BLOCK, hook_block)

def hook_unmapped(uc, access, addr, size, value, ud):
    L("UNMAPPED", access=access, addr=hex(addr), size=size)
    print("UNMAPPED access %d at %08x (pc %08x)" % (access, addr, uc.reg_read(UC_ARM_REG_PC)))
    return False
mu.hook_add(UC_HOOK_MEM_UNMAPPED, hook_unmapped)

def gpioc(uc, pc):
    """Emulate one coprocessor instruction at pc (GPIO coproc p0, DCP p4/p5 stubbed)."""
    h1, h2 = struct.unpack("<HH", bytes(uc.mem_read(pc, 4)))
    cp = (h2 >> 8) & 0xF
    R = lambda n: uc.reg_read(UC_ARM_REG_R0 + n) if n < 13 else uc.reg_read([UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC][n - 13])
    def W(n, v): uc.reg_write(UC_ARM_REG_R0 + n if n < 13 else UC_ARM_REG_LR, v & 0xFFFFFFFF)
    if (h1 & 0xEF00) == 0xEE00 and (h2 & 0x10):            # MCR / MRC (and MCR2/MRC2)
        opc1 = (h1 >> 5) & 7; is_read = bool(h1 & 0x10); crn = h1 & 0xF; rt = (h2 >> 12) & 0xF; crm = h2 & 0xF
        if cp == 0:
            tgt = {0: "out", 1: "hi_out", 4: "oe", 5: "hi_oe"}.get(crm)
            if is_read:
                apply_buttons()
                v = gpio_in if crm == 8 else 0xFFFF if crm == 9 else sio.get(tgt, 0)
                if rt == 15: uc.reg_write(UC_ARM_REG_APSR, (uc.reg_read(UC_ARM_REG_APSR) & 0x0FFFFFFF) | (v & 0xF0000000))
                else: W(rt, v)
            else:
                v = R(rt)
                if opc1 < 4:
                    o = sio[tgt]; sio[tgt] = [v, o ^ v, o | v, o & ~v][opc1] & 0xFFFFFFFF
                    if tgt == "out": lcd_cs((sio["out"] >> 15) & 1)
                    L("GPIOC_" + tgt.upper(), op=["=", "^", "|", "&~"][opc1], mask=hex(v), now=hex(sio[tgt]))
                else:   # single-bit xor/set/clr by pin number
                    pin = v & 0x3F; k = ("out" if crm == 0 else "oe") if pin < 32 else ("hi_out" if crm == 0 else "hi_oe")
                    b = 1 << (pin & 31); o = sio[k]; sio[k] = [o ^ b, o | b, o & ~b][opc1 - 5] if opc1 >= 5 else o
                    L("GPIOC_BIT_" + ("OUT" if crm == 0 else "OE"), pin=pin, op=["^", "|", "&~"][opc1 - 5] if opc1 >= 5 else "?", now=hex(sio[k]))
                    if crm == 0 and pin == 15: lcd_cs((sio["out"] >> 15) & 1)
        elif is_read and rt != 15:
            W(rt, 0)
    elif (h1 & 0xEFE0) == 0xEC40:                           # MCRR / MRRC (and 2-variants)
        is_read = bool(h1 & 0x10); rt2 = h1 & 0xF; rt = (h2 >> 12) & 0xF; opc1 = (h2 >> 4) & 0xF; crm = h2 & 0xF
        if cp == 0:
            if is_read:
                lo, hi = (gpio_in, 0xFFFF) if crm == 8 else (sio["out"], sio["hi_out"]) if crm == 0 else (sio["oe"], sio["hi_oe"])
                W(rt, lo); W(rt2, hi)
            else:
                a, b = R(rt), R(rt2)
                lo_k, hi_k = ("out", "hi_out") if crm == 0 else ("oe", "hi_oe")
                if opc1 < 4:
                    for k, v in ((lo_k, a), (hi_k, b)):
                        o = sio[k]; sio[k] = [v, o ^ v, o | v, o & ~v][opc1] & 0xFFFFFFFF
                    L("GPIOC_HILO_" + lo_k.upper(), op=opc1, lo=hex(a), hi=hex(b))
                elif opc1 < 8:  # bit put/xor2/set2/clr2: pin=a, val=b
                    pin = a & 0x3F; k = lo_k if pin < 32 else hi_k; bit = 1 << (pin & 31); o = sio[k]
                    if opc1 == 4: sio[k] = (o | bit) if b else (o & ~bit)
                    elif b: sio[k] = [o ^ bit, o | bit, o & ~bit][opc1 - 5]
                    if k == "out" and pin == 15: lcd_cs((sio["out"] >> 15) & 1)
                    L("GPIOC_BIT_" + ("OUT" if crm == 0 else "OE"), pin=pin, val=b, opc=opc1, now=hex(sio[k]))
                else:           # indexed put/xor/set/clr: reg index b, value a
                    k = (lo_k if b == 0 else hi_k); o = sio[k]
                    sio[k] = [a, o ^ a, o | a, o & ~a][opc1 - 8] & 0xFFFFFFFF
                    L("GPIOC_IDX_" + lo_k.upper(), idx=b, op=opc1, val=hex(a), now=hex(sio[k]))
        elif is_read:
            W(rt, 0); W(rt2, 0)
            dcp_count[0] += 1
    else:
        dcp_count[0] += 1      # CDP (DCP) - no-op
    return True

dcp_count = [0]
def hook_intr(uc, intno, ud):
    pc = uc.reg_read(UC_ARM_REG_PC)
    if intno in (1, 17):
        h1 = struct.unpack("<H", bytes(uc.mem_read(pc, 2)))[0]
        if (h1 & 0xEF00) == 0xEE00 or (h1 & 0xEFE0) == 0xEC40:
            gpioc(uc, pc); uc.reg_write(UC_ARM_REG_PC, (pc + 4) | 1); return
    L("EXCEPTION", intno=intno, at=hex(pc))
    print("exception %d at %08x" % (intno, pc)); uc.emu_stop()
mu.hook_add(UC_HOOK_INTR, hook_intr)

sp, reset = struct.unpack_from("<II", FW, 0)
mu.reg_write(UC_ARM_REG_SP, sp)
pc = reset | 1
status = "ok"
wfe_count = 0
while insn_count[0] < MAX_INSNS:
    try:
        mu.emu_start(pc, 0xFFFFFFFF, count=MAX_INSNS)
        status = "ok/stopped"; break
    except UcError as e:
        pc_now = mu.reg_read(UC_ARM_REG_PC)
        prev = struct.unpack("<H", bytes(mu.mem_read(pc_now - 2, 2)))[0]
        if e.errno == UC_ERR_INSN_INVALID and prev in (0xBF20, 0xBF30, 0xBF40):
            wfe_count += 1; now_us[0] += 500
            pc = pc_now | 1
            continue
        status = "UcError %s at pc=%08x" % (e, pc_now); break
print("wfe/wfi resumes:", wfe_count)
if PROF_FN:
    print("profile: instructions per call:", prof["samples"][:40])
    e = prof["entries"]; per = [b - a for a, b in zip(e, e[1:])]
    work = [p - c for p, c in zip(per, prof["samples"])]
    print("profile: instructions between calls:", per[:40])
    print("profile: work outside the call (period - in-call):", work[:40])
print("watchdog scratch0-3:", " ".join("%08x" % regs[0x400d800c + 4 * i] for i in range(4)))
print("stop:", status, "pc=%08x" % mu.reg_read(UC_ARM_REG_PC), "blocks=", insn_count[0], "log entries=", len(log))
from PIL import Image
Image.frombytes("RGB", (LCD_W, LCD_H), bytes(lcd_fb)).save(os.environ.get("LCD_PNG", "lcd.png"))
json.dump([(c, p) for c, p in lcd["cmds"]], open("lcd_cmds.json", "w"))
print("lcd: %d commands, %d pixels written; flash erases %d, programs %d" % (len(lcd["cmds"]), lcd["pixels"], flash_ops[0], flash_ops[1]))
if os.environ.get("FLASH_DUMP"):          # "offset:length:path"
    _o, _l, _p = os.environ["FLASH_DUMP"].split(":")
    open(_p, "wb").write(bytes(mu.mem_read(FLASH + int(_o, 16), int(_l, 16))))
if os.environ.get("SETTINGS_DUMP"):
    open(os.environ["SETTINGS_DUMP"], "wb").write(bytes(mu.mem_read(FLASH + 0x3FF000, 0x1000)))
if os.environ.get("FS_DUMP"):
    open(os.environ["FS_DUMP"], "wb").write(bytes(mu.mem_read(FLASH + FS_OFF, 0x1000000 - FS_OFF)))
json.dump({"log": log, "rom_fns": {hex(k): v for k, v in rom_fns.items()},
           "hot_blocks": [(hex(a), n) for a, n in pc_hist.most_common(30)]},
          open("emu_log.json", "w"), indent=0)
