/*
 * ty, a collection of GUI and command-line tools to manage Teensy devices
 *
 * Distributed under the MIT license (see LICENSE.txt or http://opensource.org/licenses/MIT)
 * Copyright (c) 2015 Niels Martignène <niels.martignene@gmail.com>
 */

#include "util.h"
#include "hs/device.h"
#include "hs/hid.h"
#include "hs/serial.h"
#include "ty/board.h"
#include "board_priv.h"
#include "ty/firmware.h"
#include "ty/system.h"

struct ty_board_model {
    TY_BOARD_MODEL

    // Identifcation
    uint8_t usage;
    bool experimental;

    // Upload settings
    unsigned int halfkay_version;
    size_t block_size;
};

struct firmware_signature {
    uint64_t magic;
    const ty_board_model *model;
    unsigned int priority;
};

#define TEENSY_VID 0x16C0

#define SEREMU_TX_SIZE 32
#define SEREMU_RX_SIZE 64

enum {
    TEENSY_USAGE_PAGE_BOOTLOADER = 0xFF9C,
    TEENSY_USAGE_PAGE_RAWHID = 0xFFAB,
    TEENSY_USAGE_PAGE_SEREMU = 0xFFC9
};

const ty_board_family _ty_teensy_family;
static const struct _ty_board_interface_vtable teensy_vtable;

static const ty_board_model teensy_unknown_model = {
    .family = &_ty_teensy_family,
    .name = "Teensy"
};

static const ty_board_model teensy_pp10_model = {
    .family = &_ty_teensy_family,
    .name = "Teensy++ 1.0",
    .mcu = "at90usb646",

    .usage = 0x1A,
    .experimental = true,

    .code_size = 64512,
    .halfkay_version = 1,
    .block_size = 256
};

static const ty_board_model teensy_20_model = {
    .family = &_ty_teensy_family,
    .name = "Teensy 2.0",
    .mcu = "atmega32u4",

    .usage = 0x1B,
    .experimental = true,

    .code_size = 32256,
    .halfkay_version = 1,
    .block_size = 128
};

static const ty_board_model teensy_pp20_model = {
    .family = &_ty_teensy_family,
    .name = "Teensy++ 2.0",
    .mcu = "at90usb1286",

    .usage = 0x1C,

    .code_size = 130048,
    .halfkay_version = 2,
    .block_size = 256
};

static const ty_board_model teensy_30_model = {
    .family = &_ty_teensy_family,
    .name = "Teensy 3.0",
    .mcu = "mk20dx128",

    .usage = 0x1D,

    .code_size = 131072,
    .halfkay_version = 3,
    .block_size = 1024
};

static const ty_board_model teensy_31_model = {
    .family = &_ty_teensy_family,
    .name = "Teensy 3.1",
    .mcu = "mk20dx256",

    .usage = 0x1E,

    .code_size = 262144,
    .halfkay_version = 3,
    .block_size = 1024
};

static const ty_board_model teensy_lc_model = {
    .family = &_ty_teensy_family,
    .name = "Teensy LC",
    .mcu = "mkl26z64",

    .usage = 0x20,

    .code_size = 63488,
    .halfkay_version = 3,
    .block_size = 512
};

static const ty_board_model teensy_32_model = {
    .family = &_ty_teensy_family,
    .name = "Teensy 3.2",
    .mcu = "mk20dx256",

    .usage = 0x21,

    .code_size = 262144,
    .halfkay_version = 3,
    .block_size = 1024
};

static const ty_board_model teensy_k64_model = {
    .family = &_ty_teensy_family,
    .name = "Teensy 3.4",
    .mcu = "mk64fx512",

    .usage = 0x23,

    .code_size = 524288,
    .halfkay_version = 3,
    .block_size = 1024
};

static const ty_board_model teensy_k66_model = {
    .family = &_ty_teensy_family,
    .name = "Teensy 3.5",
    .mcu = "mk66fx1m0",

    .usage = 0x22,

    .code_size = 1048576,
    .halfkay_version = 3,
    .block_size = 1024
};

static const ty_board_model *teensy_models[] = {
    &teensy_pp10_model,
    &teensy_20_model,
    &teensy_pp20_model,
    &teensy_30_model,
    &teensy_31_model,
    &teensy_lc_model,
    &teensy_32_model,
    &teensy_k64_model,
    &teensy_k66_model,
    NULL
};

static const struct firmware_signature signatures[] = {
    {0x0C94007EFFCFF894, &teensy_pp10_model},
    {0x0C94003FFFCFF894, &teensy_20_model},
    {0x0C9400FEFFCFF894, &teensy_pp20_model},
    {0x38800440823F0400, &teensy_30_model},
    {0x30800440823F0400, &teensy_31_model},
    {0x34800440823F0000, &teensy_lc_model},
    {0x30800440823F0400, &teensy_32_model},
    {0x0100002B88ED00E0, &teensy_k64_model, 1},
    {0x002008E003000085, &teensy_k66_model, 2}
};

static const ty_board_model *identify_model(uint16_t usage)
{
    for (const ty_board_model **cur = teensy_models; *cur; cur++) {
        const ty_board_model *model = *cur;

        if (model->usage == usage) {
            ty_log(TY_LOG_DEBUG, "Identified '%s' with usage value 0x%"PRIx16, model->name, usage);
            return *cur;
        }
    }

    ty_log(TY_LOG_DEBUG, "Unknown Teensy model with usage value 0x%"PRIx16, usage);
    return NULL;
}

static uint64_t parse_bootloader_serial(const char *s)
{
    uint64_t serial;

    // This happens for AVR Teensy boards (1.0 and 2.0)
    if (!s)
        return 12345;

    /* The bootloader returns the serial number as hexadecimal with prefixed zeros
       (which would suggest octal to stroull). */
    serial = strtoull(s, NULL, 16);

    /* In running modes, a decimal value is used but Teensyduino 1.19 added a workaround for a
       Mac OS X CDC-ADM driver bug: if the number is < 10000000, append a 0.
       See https://github.com/PaulStoffregen/cores/commit/4d8a62cf65624d2dc1d861748a9bb2e90aaf194

       It seems beta K66 boards without a programmed S/N report 00000064 (100) as the S/N and we
       need to ignore this value. */
    if (serial == 100) {
        serial = 0;
    } else if (serial < 10000000) {
        serial *= 10;
    }

    return serial;
}

static int teensy_load_interface(ty_board_interface *iface)
{
    if (hs_device_get_vid(iface->dev) != TEENSY_VID)
        return 0;

    switch (hs_device_get_pid(iface->dev)) {
    case 0x478:
    case 0x482:
    case 0x483:
    case 0x484:
    case 0x485:
    case 0x486:
    case 0x487:
    case 0x488:
        break;

    default:
        return 0;
    }

    switch (hs_device_get_type(iface->dev)) {
    case HS_DEVICE_TYPE_SERIAL:
        iface->name = "Serial";
        iface->capabilities |= 1 << TY_BOARD_CAPABILITY_RUN;
        iface->capabilities |= 1 << TY_BOARD_CAPABILITY_SERIAL;
        iface->capabilities |= 1 << TY_BOARD_CAPABILITY_REBOOT;
        break;

    case HS_DEVICE_TYPE_HID:
        switch (hs_device_get_hid_usage_page(iface->dev)) {
        case TEENSY_USAGE_PAGE_BOOTLOADER:
            iface->name = "HalfKay";
            iface->model = identify_model(hs_device_get_hid_usage(iface->dev));
            if (iface->model) {
                iface->capabilities |= 1 << TY_BOARD_CAPABILITY_UPLOAD;
                iface->capabilities |= 1 << TY_BOARD_CAPABILITY_RESET;
            }
            break;

        case TEENSY_USAGE_PAGE_RAWHID:
            iface->name = "RawHID";
            iface->capabilities |= 1 << TY_BOARD_CAPABILITY_RUN;
            break;

        case TEENSY_USAGE_PAGE_SEREMU:
            iface->name = "Seremu";
            iface->capabilities |= 1 << TY_BOARD_CAPABILITY_RUN;
            iface->capabilities |= 1 << TY_BOARD_CAPABILITY_SERIAL;
            iface->capabilities |= 1 << TY_BOARD_CAPABILITY_REBOOT;
            break;

        default:
            return 0;
        }

        break;
    }

    if (!iface->model)
        iface->model = &teensy_unknown_model;
    iface->vtable = &teensy_vtable;

    return 1;
}

static int teensy_update_board(ty_board_interface *iface, ty_board *board)
{
    const char *serial_string, *product_string;
    uint64_t serial = 0;

    serial_string = hs_device_get_serial_number_string(iface->dev);
    product_string = hs_device_get_product_string(iface->dev);

    if (iface->model->code_size) {
        if (board->model && board->model->code_size && board->model != iface->model)
            return 0;
        board->model = iface->model;

        if (serial_string) {
            serial = parse_bootloader_serial(serial_string);

            if (!board->serial) {
                board->serial = serial;
            } else if (serial != board->serial) {
                /* Let boards using an old Teensyduino (before 1.19) firmware pass with a warning
                   because there is no way to interpret the serial number correctly, and the board
                   will show up as a different board if it is first plugged in bootloader mode.
                   The only way to fix this is to use Teensyduino >= 1.19. */
                if (serial * 10 == board->serial) {
                    ty_log(TY_LOG_WARNING, "Upgrade board '%s' to use a recent Teensyduino version",
                           board->tag);
                } else {
                    return 0;
                }
            }
        }

        if (!board->description) {
            board->description = strdup("Teensy (HalfKay)");
            if (!board->description)
                return ty_error(TY_ERROR_MEMORY, NULL);
        }
    } else {
        if (!board->model)
            board->model = iface->model;

        if (serial_string) {
            serial = strtoull(serial_string, NULL, 10);

            if (!board->serial) {
                board->serial = serial;
            } else if (serial != board->serial) {
                return 0;
            }
        }

        free(board->description);
        board->description = strdup(product_string ? product_string : "Teensy");
        if (!board->description)
            return ty_error(TY_ERROR_MEMORY, NULL);
    }

    /* We cannot uniquely identify AVR Teensy boards because the S/N is always 12345,
       or custom ARM Teensy boards without a valid MAC address. */
    if (serial && serial != 12345 && serial != UINT32_MAX)
        iface->capabilities |= 1 << TY_BOARD_CAPABILITY_UNIQUE;

    return 1;
}

static int change_baudrate(hs_handle *h, unsigned int baudrate)
{
    hs_serial_config config = {
        .baudrate = baudrate
    };
    return ty_libhs_translate_error(hs_serial_set_config(h, &config));
}

int teensy_open_interface(ty_board_interface *iface)
{
    int r;

    r = hs_handle_open(iface->dev, HS_HANDLE_MODE_RW, &iface->h);
    if (r < 0)
        return ty_libhs_translate_error(r);

    /* Restore sane baudrate, because some systems (such as Linux) may keep tty settings
       around and reuse them. The device will keep rebooting if 134 is what stays around,
       so try to break the loop here. */
    if (hs_device_get_type(iface->dev) == HS_DEVICE_TYPE_SERIAL)
        change_baudrate(iface->h, 115200);

    return 0;
}

void teensy_close_interface(ty_board_interface *iface)
{
    hs_handle_close(iface->h);
    iface->h = NULL;
}

// FIXME: don't search beyond code_size, and even less on Teensy 3.0 (size of .startup = 0x400)
static unsigned int teensy_guess_models(const ty_firmware *fw,
                                        const ty_board_model **rguesses, unsigned int max)
{
    const uint8_t *image;
    size_t size;
    unsigned int priority = 0, count = 0;

    image = ty_firmware_get_image(fw);
    size = ty_firmware_get_size(fw);

    if (size < sizeof(uint64_t))
        return 0;

    /* Naive search with each board's signature, not pretty but unless
       thousands of models appear this is good enough. */
    for (size_t i = 0; i < size - sizeof(uint64_t); i++) {
        uint64_t value8 = ((uint64_t)image[i] << 56) |
                          ((uint64_t)image[i + 1] << 48) |
                          ((uint64_t)image[i + 2] << 40) |
                          ((uint64_t)image[i + 3] << 32) |
                          ((uint64_t)image[i + 4] << 24) |
                          ((uint64_t)image[i + 5] << 16) |
                          ((uint64_t)image[i + 6] << 8) |
                          image[i + 7];

        for (unsigned int j = 0; j < TY_COUNTOF(signatures); j++) {
            const struct firmware_signature *sig = &signatures[j];

            if (value8 == sig->magic && sig->priority >= priority) {
                if (sig->priority > priority) {
                    priority = sig->priority;
                    count = 0;
                }

                /* We need to continue, even if we reach max because a higher priority
                   signature may clear the current guess list. */
                if (count < max)
                    rguesses[count++] = sig->model;
            }
        }
    }

    return count;
}

static ssize_t teensy_serial_read(ty_board_interface *iface, char *buf, size_t size, int timeout)
{
    uint8_t hid_buf[SEREMU_RX_SIZE + 1];
    ssize_t r;

    switch (hs_device_get_type(iface->dev)) {
    case HS_DEVICE_TYPE_SERIAL:
        r = hs_serial_read(iface->h, (uint8_t *)buf, size, timeout);
        if (r < 0)
            return ty_libhs_translate_error((int)r);
        return r;

    case HS_DEVICE_TYPE_HID:
        r = hs_hid_read(iface->h, hid_buf, sizeof(hid_buf), timeout);
        if (r < 0)
            return ty_libhs_translate_error((int)r);
        if (r < 2)
            return 0;

        r = (ssize_t)strnlen((char *)hid_buf + 1, (size_t)(r - 1));
        memcpy(buf, hid_buf + 1, (size_t)r);
        return r;
    }

    assert(false);
    return 0;
}

static ssize_t teensy_serial_write(ty_board_interface *iface, const char *buf, size_t size)
{
    uint8_t report[SEREMU_TX_SIZE + 1];
    size_t total = 0;
    ssize_t r;

    switch (hs_device_get_type(iface->dev)) {
    case HS_DEVICE_TYPE_SERIAL:
        r = hs_serial_write(iface->h, (uint8_t *)buf, (ssize_t)size);
        if (r < 0)
            return ty_libhs_translate_error((int)r);
        return r;

    case HS_DEVICE_TYPE_HID:
        /* SEREMU expects packets of 32 bytes. The terminating NUL marks the end, so no binary
           transfers. */
        for (size_t i = 0; i < size;) {
            memset(report, 0, sizeof(report));
            memcpy(report + 1, buf + i, TY_MIN(SEREMU_TX_SIZE, size - i));

            r = hs_hid_write(iface->h, report, sizeof(report));
            if (r < 0)
                return ty_libhs_translate_error((int)r);
            if (!r)
                break;

            i += (size_t)r - 1;
            total += (size_t)r - 1;
        }
        return (ssize_t)total;
    }

    assert(false);
    return 0;
}

static int halfkay_send(ty_board_interface *iface, size_t addr, const void *data, size_t size, unsigned int timeout)
{
    uint8_t buf[2048] = {0};
    uint64_t start;

    ssize_t r;

    // Update if header gets bigger than 64 bytes
    assert(size < sizeof(buf) - 65);

    switch (iface->model->halfkay_version) {
    case 1:
        buf[1] = addr & 255;
        buf[2] = (addr >> 8) & 255;

        if (size)
            memcpy(buf + 3, data, size);
        size = iface->model->block_size + 3;
        break;

    case 2:
        buf[1] = (addr >> 8) & 255;
        buf[2] = (addr >> 16) & 255;

        if (size)
            memcpy(buf + 3, data, size);
        size = iface->model->block_size + 3;
        break;

    case 3:
        buf[1] = addr & 255;
        buf[2] = (addr >> 8) & 255;
        buf[3] = (addr >> 16) & 255;

        if (size)
            memcpy(buf + 65, data, size);
        size = iface->model->block_size + 65;
        break;

    default:
        assert(false);
        break;
    }

    /* We may get errors along the way (while the bootloader works) so try again
       until timeout expires. */
    start = ty_millis();
    hs_error_mask(HS_ERROR_IO);
restart:
    r = hs_hid_write(iface->h, buf, size);
    if (r == HS_ERROR_IO && ty_millis() - start < timeout) {
        ty_delay(10);
        goto restart;
    }
    hs_error_unmask();
    if (r < 0) {
        if (r == HS_ERROR_IO)
            return ty_error(TY_ERROR_IO, "%s", hs_error_last_message());
        return ty_libhs_translate_error((int)r);
    }

    return 0;
}

static int test_bootloader_support(const ty_board_model *model)
{
    if (model->experimental && !getenv("TY_EXPERIMENTAL_BOARDS"))
        return ty_error(TY_ERROR_UNSUPPORTED, "Support for %s boards is experimental, set "
                                              "environment variable TY_EXPERIMENTAL_BOARDS to any "
                                              "value to enable support for them", model->name);

    return 0;
}

static int teensy_upload(ty_board_interface *iface, ty_firmware *fw, ty_board_upload_progress_func *pf, void *udata)
{
    const uint8_t *image;
    size_t size;
    int r;

    r = test_bootloader_support(iface->model);
    if (r < 0)
        return r;

    image = ty_firmware_get_image(fw);
    size = ty_firmware_get_size(fw);

    if (pf) {
        r = (*pf)(iface->board, fw, 0, udata);
        if (r)
            return r;
    }

    for (size_t addr = 0; addr < size; addr += iface->model->block_size) {
        size_t block_size = TY_MIN(iface->model->block_size, (size_t)(size - addr));

        r = halfkay_send(iface, addr, image + addr, block_size, 3000);
        if (r < 0)
            return r;

        /* HalfKay generates STALL if you go too fast (translates to EPIPE on Linux), and the
           first write takes longer because it triggers a complete erase of all blocks. */
        ty_delay(addr ? 20 : 200);

        if (pf) {
            r = (*pf)(iface->board, fw, addr + block_size, udata);
            if (r)
                return r;
        }
    }

    return 0;
}

static int teensy_reset(ty_board_interface *iface)
{
    int r = test_bootloader_support(iface->model);
    if (r < 0)
        return r;

    return halfkay_send(iface, 0xFFFFFF, NULL, 0, 250);
}

static int teensy_reboot(ty_board_interface *iface)
{
    static const unsigned int serial_magic = 134;
    static const unsigned char seremu_magic[] = {0, 0xA9, 0x45, 0xC2, 0x6B};

    int r;

    r = TY_ERROR_UNSUPPORTED;
    switch (hs_device_get_type(iface->dev)) {
    case HS_DEVICE_TYPE_SERIAL:
        r = change_baudrate(iface->h, serial_magic);
        if (!r) {
            /* Don't keep these settings, some systems (such as Linux) may reuse them and
               the device will keep rebooting when opened. */
            ty_error_mask(TY_ERROR_SYSTEM);
            change_baudrate(iface->h, 115200);
            ty_error_unmask();
        }
        break;

    case HS_DEVICE_TYPE_HID:
        r = (int)hs_hid_send_feature_report(iface->h, seremu_magic, sizeof(seremu_magic));
        if (r < 0) {
            r = ty_libhs_translate_error(r);
        } else {
            assert(r == sizeof(seremu_magic));
            r = 0;
        }
        break;
    }

    return r;
}

const ty_board_family _ty_teensy_family = {
    .name = "Teensy",
    .models = teensy_models,

    .load_interface = teensy_load_interface,
    .update_board = teensy_update_board,

    .guess_models = teensy_guess_models
};

static const struct _ty_board_interface_vtable teensy_vtable = {
    .open_interface = teensy_open_interface,
    .close_interface = teensy_close_interface,

    .serial_read = teensy_serial_read,
    .serial_write = teensy_serial_write,

    .upload = teensy_upload,
    .reset = teensy_reset,

    .reboot = teensy_reboot
};