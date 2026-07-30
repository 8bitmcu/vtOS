# MicroPython SD card driver (SPI mode)
# Sourced from micropython-lib (MIT License)
import os, time

_CMD_TIMEOUT = const(100)
_R1_IDLE_STATE = const(1 << 0)
_R1_ILLEGAL_COMMAND = const(1 << 2)
_TOKEN_CMD25 = const(0xFC)
_TOKEN_STOP_TRAN = const(0xFD)
_TOKEN_DATA = const(0xFE)


class SDCard:
    def __init__(self, spi, cs):
        self.spi = spi
        self.cs = cs
        self.cmdbuf = bytearray(6)
        self.dummybuf = bytearray(512)
        self.tokenbuf = bytearray(1)
        for i in range(512):
            self.dummybuf[i] = 0xFF
        self.dummybuf_memoryview = memoryview(self.dummybuf)

        self.cs.init(self.cs.OUT, value=1)
        self.init_card()

    def init_spi(self, baudrate):
        try:
            master = self.spi.MASTER
        except AttributeError:
            self.spi.init(baudrate=baudrate, phase=0, polarity=0)
        else:
            self.spi.init(master, baudrate=baudrate, phase=0, polarity=0)

    def init_card(self):
        self.init_spi(100000)
        # Send 80 clock cycles with CS high
        self.cs(1)
        for i in range(10):
            self.spi.write(b"\xff")
        # CMD0: reset
        for _ in range(_CMD_TIMEOUT):
            if self.cmd(0, 0, 0x95) == _R1_IDLE_STATE:
                break
        else:
            raise OSError("SD init: CMD0 failed")
        # CMD8: check voltage
        r = self.cmd(8, 0x01AA, 0x87, 4)
        if r == _R1_IDLE_STATE:
            self.init_card_v2()
        elif r == (_R1_IDLE_STATE | _R1_ILLEGAL_COMMAND):
            self.init_card_v1()
        else:
            raise OSError("SD init: CMD8 failed")
        # CMD58: read OCR
        self.cmd(58, 0, 0, 4)
        self.init_spi(1320000)

    def init_card_v1(self):
        for _ in range(_CMD_TIMEOUT):
            self.cmd(55, 0, 0)
            if self.cmd(41, 0, 0) == 0:
                self.cdv = 512
                return
        raise OSError("SD init: v1 timeout")

    def init_card_v2(self):
        for _ in range(_CMD_TIMEOUT):
            time.sleep_ms(50)
            self.cmd(58, 0, 0, 4)
            self.cmd(55, 0, 0)
            if self.cmd(41, 0x40000000, 0) == 0:
                self.cmd(58, 0, 0, 4)
                self.cdv = 1
                return
        raise OSError("SD init: v2 timeout")

    def cmd(self, cmd, arg, crc, final=0, release=True, skip1=False):
        self.cs(0)
        buf = self.cmdbuf
        buf[0] = 0x40 | cmd
        buf[1] = arg >> 24
        buf[2] = arg >> 16
        buf[3] = arg >> 8
        buf[4] = arg
        buf[5] = crc
        self.spi.write(buf)
        if skip1:
            self.spi.readinto(self.tokenbuf, 0xFF)
        for _ in range(_CMD_TIMEOUT):
            self.spi.readinto(self.tokenbuf, 0xFF)
            response = self.tokenbuf[0]
            if not (response & 0x80):
                for _ in range(final):
                    self.spi.write(b"\xff")
                if release:
                    self.cs(1)
                    self.spi.write(b"\xff")
                return response
        self.cs(1)
        self.spi.write(b"\xff")
        return -1

    def readblocks(self, block_num, buf):
        nblocks = len(buf) // 512
        assert nblocks and not len(buf) % 512, "buf length must be multiple of 512"
        if nblocks == 1:
            if self.cmd(17, block_num * self.cdv, 0, release=False) != 0:
                self.cs(1)
                raise OSError(5)
            self.readinto(buf)
        else:
            if self.cmd(18, block_num * self.cdv, 0, release=False) != 0:
                self.cs(1)
                raise OSError(5)
            offset = 0
            mv = memoryview(buf)
            while nblocks:
                self.readinto(mv[offset : offset + 512])
                offset += 512
                nblocks -= 1
            if self.cmd(12, 0, 0xFF, skip1=True) != 0:
                raise OSError(32)

    def readinto(self, buf):
        self.cs(0)
        for _ in range(_CMD_TIMEOUT):
            self.spi.readinto(self.tokenbuf, 0xFF)
            if self.tokenbuf[0] == _TOKEN_DATA:
                break
        else:
            self.cs(1)
            raise OSError(5)
        self.spi.readinto(buf, 0xFF)
        self.spi.write(b"\xff\xff")  # CRC
        self.cs(1)

    def writeblocks(self, block_num, buf):
        nblocks, err = divmod(len(buf), 512)
        assert nblocks and not err, "buf length must be multiple of 512"
        if nblocks == 1:
            if self.cmd(24, block_num * self.cdv, 0) != 0:
                raise OSError(5)
            self.write(buf, _TOKEN_DATA)
        else:
            if self.cmd(25, block_num * self.cdv, 0) != 0:
                raise OSError(5)
            offset = 0
            mv = memoryview(buf)
            while nblocks:
                self.write(mv[offset : offset + 512], _TOKEN_CMD25)
                offset += 512
                nblocks -= 1
            self.write_token(_TOKEN_STOP_TRAN)

    def write(self, buf, token):
        self.cs(0)
        self.spi.read(1, token)
        self.spi.write(buf)
        self.spi.write(b"\xff\xff")  # CRC
        for _ in range(_CMD_TIMEOUT):
            self.spi.readinto(self.tokenbuf, 0xFF)
            response = self.tokenbuf[0] & 0x1F
            if response == 0x05:
                break
        else:
            self.cs(1)
            raise OSError(5)
        while True:
            self.spi.readinto(self.tokenbuf, 0xFF)
            if self.tokenbuf[0] != 0:
                break
        self.cs(1)

    def write_token(self, token):
        self.cs(0)
        self.spi.read(1, token)
        self.spi.write(b"\xff")
        while True:
            self.spi.readinto(self.tokenbuf, 0xFF)
            if self.tokenbuf[0] != 0:
                break
        self.cs(1)

    def ioctl(self, op, arg):
        if op == 4:  # BP_IOCTL_SEC_COUNT
            return self.sectors()
        if op == 5:  # BP_IOCTL_SEC_SIZE
            return 512

    def sectors(self):
        buf = bytearray(18)
        self.cs(0)
        if self.cmd(9, 0, 0, release=False) != 0:
            self.cs(1)
            raise OSError(5)
        self.readinto(buf)
        if buf[0] >> 6 == 1:  # SDHC
            c_size = ((buf[7] & 0x3F) << 16) | (buf[8] << 8) | buf[9]
            return (c_size + 1) * 1024
        else:
            c_size = ((buf[6] & 0x03) << 10) | (buf[7] << 2) | ((buf[8] >> 6) & 0x03)
            c_size_mult = ((buf[9] & 0x03) << 1) | (buf[10] >> 7)
            read_bl_len = buf[5] & 0x0F
            return (c_size + 1) * (2 ** (c_size_mult + 2)) * (2 ** (read_bl_len - 9))
