#define MICROPY_HW_BOARD_NAME "LILYGO T-Deck"
#define MICROPY_HW_MCU_NAME "ESP32S3"

#define MICROPY_HW_ENABLE_UART_REPL (1)

// Default is 16*1024 (mpconfigport.h) -- too small for codec2_create()'s
// deep DSP init (FFT/LPC/quantiser table setup). Overflowing mp_task's
// stack there didn't fault directly (task stacks are heap-backed on
// ESP-IDF, so an overflow just scribbles into adjacent heap memory);
// instead it corrupted heap metadata that only crashed later, in an
// unrelated task's free() call, which is what a coredump caught. Doubling
// this gives codec2_create() enough headroom to stop overflowing.
//
// The BLE mesh transport's gap_advertise()/gap_scan() calls run NimBLE
// host + coexistence call chains on this same mp_task stack, on top of
// whatever the TUI is already using at that moment, and were suspected
// of overflowing it the same way codec2 did above -- silently, with no
// fault at the overflow site, surfacing later as unrelated heap
// corruption (observed as display buffer corruption and a full hang
// with no panic output) rather than a clean crash.
//
// Growing this stack turned out to be the wrong lever, though:
// esp32.idf_heap_info(esp32.HEAP_DATA) at a fresh boot with WiFi on,
// before BLE touches anything, shows this chip's largest internal-RAM
// pool (~205K total) already down to 4 bytes free / 0 largest-free-block
// from WiFi + baseline system overhead alone. mp_task's stack is
// allocated from that exact same scarce internal-RAM pool the BT
// controller's own buffers need, so raising it past 32K only worsened
// the problem: at 48K/64K, bluetooth.BLE().active(True) fails outright
// with OSError EIO whenever WiFi is also active, because there's no
// internal RAM left for the controller's buffers at all. 32K is the
// largest value confirmed to still let BT controller init succeed
// alongside WiFi -- it does NOT rule out the original slow heap
// corruption during extended BLE use, since that's a separate internal-
// RAM-pressure symptom of the same underlying scarcity, not something
// stack size alone can fix in either direction. The real fix is likely
// reducing WiFi's own internal buffer footprint (there are sdkconfig
// knobs for RX/TX buffer counts) to free headroom in this pool, rather
// than continuing to move this number.
#define MICROPY_TASK_STACK_SIZE (32 * 1024)
