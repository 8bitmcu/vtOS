import os
import esp32
import time
import machine

import tdeck_kbd
import tdeck_trk
import st7789

# T-Deck board pin/parameter definitions.
# Based on https://github.com/Xinyuan-LilyGO/T-Deck/blob/master/examples/UnitTest/utilities.h

POWERON = 10

I2S_WS = 5
I2S_BCK = 7
I2S_DOUT = 6

I2C_SDA = 18
I2C_SCL = 8

BAT_ADC = 4

TOUCH_INT = 16
KEYBOARD_INT = 46

SDCARD_CS = 39
TFT_CS = 12
RADIO_CS = 9

TFT_DC = 11
TFT_BACKLIGHT = 42

SPI_MOSI = 41
SPI_MISO = 38
SPI_SCK = 40

TBOX_G02 = 2
TBOX_G01 = 3
TBOX_G04 = 1
TBOX_G03 = 15

ES7210_MCLK = 48
ES7210_LRCK = 21
ES7210_SCK = 47
ES7210_DIN = 14

RADIO_BUSY = 13
RADIO_RST = 17
RADIO_DIO1 = 45

BOOT = 0

BL = 42

GPS_TX = 43
GPS_RX = 44

RADIO_BANDWIDTH = 125.0
RADIO_SF = 10
RADIO_CR = 6

DEFAULT_OPA = 100



# TFT and SD card share one physical SPI bus (same SCK/MOSI/MISO, separate
# CS). The display wants max throughput; SD cards don't
# support anywhere near 80MHz in SPI mode (sdcard.py itself only ever
# negotiates up to 1.32MHz as its safe steady-state speed). FastSDCard
# below throttles the shared bus down to SD_BAUDRATE for the duration of
# each block transfer and restores DISPLAY_BAUDRATE right after.
DISPLAY_BAUDRATE = 80000000
SD_BAUDRATE = 20000000

# Bound to the shared hardware SPI object once it's constructed below.
# Stays None during the initial mount, which still happens over the
# separate sd_spi (SoftSPI) -- FastSDCard uses that to tell whether a
# given transfer is sharing the bus with the display at all.
spi = None

def init_board():
    # Must be called before initializing LCD / Keyboard
    machine.Pin(POWERON, machine.Pin.OUT, value=1)
    time.sleep(0.1)

    # Pull all SPI CS pins HIGH before touching the bus (prevents bus conflicts)
    machine.Pin(TFT_CS, machine.Pin.OUT, value=1)
    machine.Pin(RADIO_CS, machine.Pin.OUT, value=1)
    machine.Pin(SDCARD_CS, machine.Pin.OUT, value=1)
    machine.Pin(SPI_MISO, machine.Pin.IN, machine.Pin.PULL_UP)
    time.sleep(0.1)


def init_spi(env):
    # Without this, `spi = machine.SPI(...)` below would create a
    # function-local that shadows the module-level `spi` for the rest of
    # this function -- init_tft()'s bare `spi` reference resolves at the
    # module scope, so it would still see the module-level None and hand
    # st7789.ST7789() a dead SPI bus.
    global spi

    # Init SD Card first
    try:
        import sdcard

        class FastSDCard(sdcard.SDCard):
            def _with_sd_speed(self, fn, *args):
                if self.spi is not spi:
                    # Still on sd_spi (SoftSPI) -- not shared with anything,
                    # nothing to throttle.
                    return fn(*args)
                # env.sd_busy: true while a SD block transfer has the shared
                # bus parked at SD_BAUDRATE. Anything else that writes to the
                # display over the same bus (scheduled_fast's draw(),
                # statusbar.refresh()) checks this and skips its turn rather
                # than racing a draw call against the lowered bus speed.
                env.sd_busy = True
                spi.init(baudrate=SD_BAUDRATE)
                try:
                    return fn(*args)
                finally:
                    spi.init(baudrate=DISPLAY_BAUDRATE)
                    env.sd_busy = False

            def readblocks(self, block_num, buf):
                return self._with_sd_speed(super(FastSDCard, self).readblocks, block_num, buf)

            def writeblocks(self, block_num, buf):
                return self._with_sd_speed(super(FastSDCard, self).writeblocks, block_num, buf)

            def ioctl(self, op, arg):
                return self._with_sd_speed(super(FastSDCard, self).ioctl, op, arg)

        sd_spi = machine.SoftSPI(
                baudrate=400000,
                sck=machine.Pin(SPI_SCK),
                mosi=machine.Pin(SPI_MOSI),
                miso=machine.Pin(SPI_MISO))

        sd = FastSDCard(sd_spi, machine.Pin(SDCARD_CS, machine.Pin.OUT))

        os.mount(os.VfsFat(sd), '/sd')
    except Exception as e:
        sd = None


    # Create hardware SPI; this configures the GPIO matrix for pins 40/41.
    spi = machine.SPI(
            1,
            baudrate=DISPLAY_BAUDRATE,
            sck=machine.Pin(SPI_SCK),
            mosi=machine.Pin(SPI_MOSI),
            miso=machine.Pin(SPI_MISO))

    # The card is already initialized over sd_spi. From here on, FastSDCard's
    # readblocks/writeblocks/ioctl will detect self.spi is spi and throttle
    # the shared bus down to SD_BAUDRATE around every transfer.
    if sd is not None:
        sd.spi = spi


def init_tft(env):
    # Initialze LCD
    tft = st7789.ST7789(
            spi,
            env.screen_height,
            env.screen_width,
            dc=machine.Pin(TFT_DC, machine.Pin.OUT),
            cs=machine.Pin(TFT_CS, machine.Pin.OUT),
            backlight=machine.Pin(BL, machine.Pin.OUT),
            rotation=1,
            buffer_size=env.screen_width*16*2)  # buffer_size needs to be: (screen_width * max_font_height * 2)
    tft.init()
    return tft


def init_keyboard():
    # Initialize keyboard
    kbd = tdeck_kbd.Keyboard(
            sda=I2C_SDA,
            scl=I2C_SCL)

    # Initialize Trackball
    tdeck_trk.init(
            up=TBOX_G01,
            down=TBOX_G03,
            left=TBOX_G04,
            right=TBOX_G02,
            click=BOOT)
    return kbd

def init_flash():
    try:
        p = esp32.Partition.find(esp32.Partition.TYPE_DATA, label='vfs')[0]
    except IndexError:
        print("Error: 'vfs' partition not found in partition table.")

    try:
        os.mount(p, '/flash')
        print("Filesystem mounted at /flash")
    except OSError as e:
        # Error 19: ENODEV (Unformatted or corrupt)
        # Error 22: EINVAL (Invalid filesystem)
        if e.args[0] in (19, 22):
            print("Filesystem not found. Formatting partition... (Please wait)")
            try:
                os.VfsLfs2.mkfs(p)
                os.mount(p, '/flash')
                print("Format successful and mounted at /flash")
            except Exception as format_err:
                print(f"Format failed: {format_err}")
        else:
            print(f"Unexpected mount error: {e}")

    try:
        os.chdir('/flash')
    except:
        pass


def init_audio(env):
    import audioplayer
    env.audio = audioplayer.AudioPlayer(
            bck=I2S_BCK,
            ws=I2S_WS,
            dout=I2S_DOUT)


def init_mic(env):
    import audioplayer
    env.rec = audioplayer.AudioRecorder(
            mclk=ES7210_MCLK,
            bck=ES7210_SCK,
            ws=ES7210_LRCK,
            din=ES7210_DIN,
            i2c_sda=I2C_SDA,
            i2c_scl=I2C_SCL,
            i2s_num=1,
            channels=1,
            mic_gain=9,
            dma_buf_count=6,
            sample_rate=8000,
            mclk_ratio=512,
            i2c_shared=True)


def init_radio(env):
    import lora
    import json
    lr = lora.LoRa(cs=RADIO_CS,
                   dio1=RADIO_DIO1,
                   rst=RADIO_RST,
                   busy=RADIO_BUSY,
                   sck=SPI_SCK,
                   miso=SPI_MISO,
                   mosi=SPI_MOSI)

    try:
        with open("/flash/.radio.json", "r") as f:
            config = json.load(f)
    except Exception as e:
        print("Failed to load radio config. Have you run loracfg?")
        return

    try:
        lr.begin(freq=float(config["freq"]),
                 bw=RADIO_BANDWIDTH,
                 sf=RADIO_SF,
                 cr=RADIO_CR,
                 sync_word=0x12,
                 power=int(config["pwr"]))
    except Exception as e:
        print(f"Radio initialization failed: {e}")
        return

    errors = lr.get_device_errors()
    if errors != 0x0000:
        print(f"SX1262 device errors: 0x{errors:04X}")
        return

    env.radio = lr
