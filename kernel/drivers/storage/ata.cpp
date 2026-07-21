#include "drivers/storage/ata.h"
#include "drivers/storage/ahci.h"
#include "drivers/storage/nvme.h"
#include "driver_manager.h"
#include "serial_log.h"
#include <stdint.h>
#include <stddef.h>

static enum disk_controller_type current_controller = DISK_CONTROLLER_NONE;

static const struct ata_channel ata_channels[2] = {
    { ATA_PRIMARY_DATA, ATA_PRIMARY_CONTROL },
    { ATA_SECONDARY_DATA, ATA_SECONDARY_CONTROL }
};

static struct ata_device_id current_ata_device = {
    0, ATA_SELECT_MASTER, false, false, 0
};
static bool current_ata_device_valid = false;

static inline void outb(uint16_t port, uint8_t val) {
    asm volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline void outw(uint16_t port, uint16_t val) {
    asm volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
static inline uint16_t inw(uint16_t port) {
    uint16_t ret;
    asm volatile("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static const struct ata_channel* ata_get_channel(uint8_t ch) {
    if (ch > 1) return 0;
    return &ata_channels[ch];
}

static void ata_soft_reset(const struct ata_channel* channel) {
    if (!channel) return;
    outb(channel->ctrl_base, 0x04);
    for (volatile int i = 0; i < 10000; i++) {}
    outb(channel->ctrl_base, 0x00);
    for (volatile int i = 0; i < 10000; i++) {}
}

static int ata_wait_ready_port(uint16_t status_port) {
    for (int i = 0; i < 10000; i++) {
        uint8_t status = inb(status_port);
        if (!(status & ATA_STATUS_BSY)) return 0;
    }
    return -1;
}

static int ata_wait_data_port(uint16_t status_port) {
    for (int i = 0; i < 100000; i++) {
        uint8_t status = inb(status_port);
        if (status & ATA_STATUS_DRQ) return 0;
        if (status & ATA_STATUS_ERR) return -1;
    }
    return -1;
}

static int ata_flush_cache(const struct ata_channel* channel, uint8_t select) {
    if (!channel) return -1;
    uint16_t base = channel->io_base;
    if (ata_wait_ready_port(base + 7) != 0) return -1;
    outb(base + 6, select);
    outb(base + 7, ATA_CMD_FLUSH_CACHE);
    if (ata_wait_ready_port(base + 7) != 0) return -1;
    uint8_t status = inb(base + 7);
    if (status & ATA_STATUS_ERR) {
        uint8_t err = inb(base + 1);
        log_fmt3(LOG_ERR, "ata", "flush error", "err", err, "ch", channel->io_base, "sel", select);
        return -1;
    }
    return 0;
}

static bool ata_is_atapi(uint16_t base, uint8_t select) {
    outb(base + 6, select);
    for (int i = 0; i < 1000; i++) {
        if (!(inb(base + 7) & ATA_STATUS_BSY)) break;
    }
    for (volatile int i = 0; i < 100; i++) {}
    uint8_t mid = inb(base + 4);
    uint8_t high = inb(base + 5);
    return mid == 0x14 && high == 0xEB;
}

static bool ata_check_device(uint16_t base, uint8_t select) {
    outb(base + 6, select);
    for (int i = 0; i < 1000; i++) {
        if (!(inb(base + 7) & ATA_STATUS_BSY)) break;
    }
    uint8_t status = inb(base + 7);
    if (status == 0xFF) return false;
    for (volatile int i = 0; i < 100; i++) {}
    if (ata_is_atapi(base, select)) return true;
    outb(base + 7, ATA_CMD_IDENTIFY);
    for (int i = 0; i < 10000; i++) {
        status = inb(base + 7);
        if (!(status & ATA_STATUS_BSY)) break;
    }
    if (status & ATA_STATUS_ERR) {
        return ata_is_atapi(base, select);
    }
    return (status & ATA_STATUS_DRQ) != 0;
}

static uint32_t ata_parse_identify(uint16_t* identify, bool* out_lba48) {
    if (out_lba48) *out_lba48 = false;
    if (identify[0] == 0x0000 && identify[1] == 0x0000) return 0;
    if (identify[0] == 0xFFFF && identify[1] == 0xFFFF) return 0;
    if (identify[83] & 0x0400) {
        if (out_lba48) *out_lba48 = true;
        uint32_t low = identify[100] | ((uint32_t)identify[101] << 16);
        uint32_t high = identify[102] | ((uint32_t)identify[103] << 16);
        if (high == 0 && low > 0) return low;
    }
    return identify[60] | ((uint32_t)identify[61] << 16);
}

static int ata_identify_dev(const struct ata_channel* channel, uint8_t select,
                            uint32_t* size_out, bool* lba48_out, bool* atapi_out) {
    if (!channel) return -1;
    uint16_t base = channel->io_base;
    if (ata_is_atapi(base, select)) {
        if (atapi_out) *atapi_out = true;
        if (size_out) *size_out = 0;
        return 0;
    }
    if (atapi_out) *atapi_out = false;
    outb(base + 6, select);
    if (ata_wait_ready_port(base + 7) != 0) return -1;
    outb(base + 2, 0);
    outb(base + 3, 0);
    outb(base + 4, 0);
    outb(base + 5, 0);
    outb(base + 7, ATA_CMD_IDENTIFY);
    if (ata_wait_ready_port(base + 7) != 0) return -1;
    if (ata_wait_data_port(base + 7) != 0) return -1;
    uint8_t status = inb(base + 7);
    if (status & ATA_STATUS_ERR) return -1;
    uint16_t identify[256];
    for (int i = 0; i < 256; i++) identify[i] = inw(base);
    bool lba48 = false;
    uint32_t sz = ata_parse_identify(identify, &lba48);
    if (size_out) *size_out = sz;
    if (lba48_out) *lba48_out = lba48;
    return sz > 0 ? 0 : -1;
}

static int ata_transfer(const struct ata_channel* channel, uint8_t select,
                        uint32_t lba, uint16_t count, void* buffer, bool write, bool use_lba48) {
    if (!channel || !buffer || count == 0) return -1;
    uint16_t base = channel->io_base;

    if (ata_wait_ready_port(base + 7) != 0) {
        ata_soft_reset(channel);
        if (ata_wait_ready_port(base + 7) != 0) {
            log_msg(LOG_ERR, "ata", "ready timeout");
            return -1;
        }
    }

    bool lba48 = use_lba48 || lba >= 0x0FFFFFFFu;
    uint8_t cmd;
    if (lba48) {
        cmd = write ? ATA_CMD_WRITE_PIO_EXT : ATA_CMD_READ_PIO_EXT;
        outb(base + 2, (uint8_t)(count & 0xFF));
        outb(base + 2, (uint8_t)((count >> 8) & 0xFF));
        outb(base + 3, (uint8_t)(lba & 0xFF));
        outb(base + 4, (uint8_t)((lba >> 8) & 0xFF));
        outb(base + 5, (uint8_t)((lba >> 16) & 0xFF));
        outb(base + 6, (uint8_t)(0x40 | (select & 0xF0) | ((lba >> 24) & 0x0F)));
        outb(base + 1, (uint8_t)((lba >> 24) & 0xFF));
        outb(base + 2, 0);
    } else {
        cmd = write ? ATA_CMD_WRITE_SECTORS : ATA_CMD_READ_SECTORS;
        outb(base + 6, select | (uint8_t)((lba >> 24) & 0x0F));
        outb(base + 2, (uint8_t)(count & 0xFF));
        outb(base + 3, (uint8_t)(lba & 0xFF));
        outb(base + 4, (uint8_t)((lba >> 8) & 0xFF));
        outb(base + 5, (uint8_t)((lba >> 16) & 0xFF));
    }
    outb(base + 7, cmd);

    if (ata_wait_data_port(base + 7) != 0) {
        uint8_t err = inb(base + 1);
        log_fmt3(LOG_ERR, "ata", "data wait", "err", err, "lba", lba, "w", write ? 1u : 0u);
        return -1;
    }

    uint16_t* buf16 = (uint16_t*)buffer;
    if (write) {
        for (int i = 0; i < 256 * (int)count; i++) outw(base, buf16[i]);
        if (ata_wait_ready_port(base + 7) != 0) return -1;
        if (ata_flush_cache(channel, select) != 0) return -1;
    } else {
        for (int i = 0; i < 256 * (int)count; i++) buf16[i] = inw(base);
    }

    uint8_t status = inb(base + 7);
    if (status & ATA_STATUS_ERR) {
        uint8_t err = inb(base + 1);
        log_fmt3(LOG_ERR, "ata", "transfer err", "err", err, "lba", lba, "w", write ? 1u : 0u);
        return -1;
    }
    return 0;
}

enum disk_controller_type disk_get_controller_type() {
    return current_controller;
}

const char* disk_get_controller_name() {
    switch (current_controller) {
        case DISK_CONTROLLER_ATA: return "ATA/IDE";
        case DISK_CONTROLLER_AHCI: return "AHCI (SATA)";
        case DISK_CONTROLLER_NVME: return "NVMe";
        default: return "None";
    }
}

void disk_set_controller(enum disk_controller_type type) {
    current_controller = type;
}

void ata_select_device(const struct ata_device_id* dev) {
    if (!dev) return;
    current_ata_device = *dev;
    current_ata_device_valid = true;
}

const struct ata_device_id* ata_get_selected_device() {
    return current_ata_device_valid ? &current_ata_device : 0;
}

int ata_init_device(const struct ata_device_id* dev) {
    if (!dev) return -1;
    const struct ata_channel* ch = ata_get_channel(dev->channel);
    if (!ch) return -1;
    outb(ch->io_base + 6, dev->select);
    if (ata_wait_ready_port(ch->io_base + 7) != 0) return -1;
    ata_select_device(dev);
    return 0;
}

int ata_init() {
    struct ata_device_id dev;
    dev.channel = 0;
    dev.select = ATA_SELECT_MASTER;
    dev.lba48 = false;
    dev.is_atapi = false;
    dev.size_sectors = 0;
    bool atapi = false;
    uint32_t sz = 0;
    bool lba48 = false;
    if (ata_identify_dev(ata_get_channel(0), ATA_SELECT_MASTER, &sz, &lba48, &atapi) != 0 &&
        !atapi) {
        return -1;
    }
    dev.lba48 = lba48;
    dev.is_atapi = atapi;
    dev.size_sectors = sz;
    ata_select_device(&dev);
    return atapi ? -1 : 0;
}

int ata_probe_all(struct ata_probed_device* out, int max_count) {
    if (!out || max_count <= 0) return 0;
    int found = 0;
    static const uint8_t selects[2] = { ATA_SELECT_MASTER, ATA_SELECT_SLAVE };
    for (uint8_t ch = 0; ch < 2; ch++) {
        const struct ata_channel* channel = ata_get_channel(ch);
        if (!channel) continue;
        for (int s = 0; s < 2; s++) {
            uint8_t sel = selects[s];
            if (!ata_check_device(channel->io_base, sel)) continue;
            bool atapi = ata_is_atapi(channel->io_base, sel);
            bool lba48 = false;
            uint32_t sz = 0;
            if (!atapi) {
                ata_identify_dev(channel, sel, &sz, &lba48, &atapi);
            }
            if (found >= max_count) return found;
            out[found].channel = ch;
            out[found].select = sel;
            out[found].is_atapi = atapi;
            out[found].lba48 = lba48;
            out[found].size_sectors = sz;
            found++;
        }
    }
    return found;
}

static int ata_driver_read(void* device_data, void* buffer, size_t size, uint32_t offset) {
    (void)device_data;
    uint32_t lba = offset;
    size_t sectors = (size + 511) / 512;
    return disk_read_sectors(lba, sectors, buffer);
}

static int ata_driver_write(void* device_data, const void* buffer, size_t size, uint32_t offset) {
    (void)device_data;
    uint32_t lba = offset;
    size_t sectors = (size + 511) / 512;
    return disk_write_sectors(lba, sectors, buffer);
}

static int ata_driver_ioctl(void* device_data, uint32_t cmd, void* arg) {
    (void)device_data;
    if (cmd == 0 && arg) {
        uint32_t* size = (uint32_t*)arg;
        *size = disk_get_size_sectors();
        return 0;
    }
    return -1;
}

int disk_init() {
    if (nvme_init() == 0) {
        current_controller = DISK_CONTROLLER_NVME;
        return 0;
    }
    if (ahci_init() == 0) {
        current_controller = DISK_CONTROLLER_AHCI;
        return 0;
    }
    if (ata_init() == 0) {
        current_controller = DISK_CONTROLLER_ATA;
        struct driver ata_driver;
        ata_driver.name[0] = 'a';
        ata_driver.name[1] = 't';
        ata_driver.name[2] = 'a';
        ata_driver.name[3] = '0';
        ata_driver.name[4] = 0;
        ata_driver.type = DRIVER_STORAGE;
        ata_driver.device_id = 0;
        ata_driver.device_data = 0;
        ata_driver.initialized = true;
        ata_driver.active = true;
        ata_driver.ops.init = 0;
        ata_driver.ops.read = ata_driver_read;
        ata_driver.ops.write = ata_driver_write;
        ata_driver.ops.ioctl = ata_driver_ioctl;
        ata_driver.ops.cleanup = 0;
        driver_register(&ata_driver);
        return 0;
    }
    return -1;
}

int disk_read_sector(uint32_t lba, void* buffer) {
    return disk_read_sectors(lba, 1, buffer);
}

int disk_write_sector(uint32_t lba, const void* buffer) {
    return disk_write_sectors(lba, 1, buffer);
}

int disk_read_sectors(uint32_t lba, size_t count, void* buffer) {
    if (current_controller == DISK_CONTROLLER_NVME) {
        for (size_t i = 0; i < count; i++) {
            if (nvme_read_sector(lba + (uint32_t)i, (uint8_t*)buffer + i * 512) != 0) return -1;
        }
        return 0;
    }
    if (current_controller == DISK_CONTROLLER_AHCI) return ahci_read_sectors(lba, count, buffer);
    if (current_controller == DISK_CONTROLLER_ATA) return ata_read_sectors(lba, count, buffer);
    return -1;
}

int disk_write_sectors(uint32_t lba, size_t count, const void* buffer) {
    if (current_controller == DISK_CONTROLLER_NVME) {
        for (size_t i = 0; i < count; i++) {
            if (nvme_write_sector(lba + (uint32_t)i, (uint8_t*)buffer + i * 512) != 0) return -1;
        }
        return 0;
    }
    if (current_controller == DISK_CONTROLLER_AHCI) return ahci_write_sectors(lba, count, buffer);
    if (current_controller == DISK_CONTROLLER_ATA) return ata_write_sectors(lba, count, buffer);
    return -1;
}

int ata_read_sectors_dev(const struct ata_device_id* dev, uint32_t lba, size_t count, void* buf) {
    if (!dev || !buf) return -1;
    const struct ata_channel* ch = ata_get_channel(dev->channel);
    if (!ch) return -1;
    uint8_t* b = (uint8_t*)buf;
    for (size_t i = 0; i < count; i++) {
        if (ata_transfer(ch, dev->select, lba + (uint32_t)i, 1, b + i * 512, false, dev->lba48) != 0)
            return -1;
    }
    return 0;
}

int ata_write_sectors_dev(const struct ata_device_id* dev, uint32_t lba, size_t count, const void* buf) {
    if (!dev || !buf) return -1;
    const struct ata_channel* ch = ata_get_channel(dev->channel);
    if (!ch) return -1;
    const uint8_t* b = (const uint8_t*)buf;
    for (size_t i = 0; i < count; i++) {
        if (ata_transfer(ch, dev->select, lba + (uint32_t)i, 1, (void*)(b + i * 512), true, dev->lba48) != 0)
            return -1;
    }
    return 0;
}

int ata_read_sector(uint32_t lba, void* buffer) {
    return ata_read_sectors(lba, 1, buffer);
}

int ata_write_sector(uint32_t lba, const void* buffer) {
    return ata_write_sectors(lba, 1, buffer);
}

int ata_read_sectors(uint32_t lba, size_t count, void* buffer) {
    if (!current_ata_device_valid) return -1;
    return ata_read_sectors_dev(&current_ata_device, lba, count, buffer);
}

int ata_write_sectors(uint32_t lba, size_t count, const void* buffer) {
    if (!current_ata_device_valid) return -1;
    return ata_write_sectors_dev(&current_ata_device, lba, count, buffer);
}

uint32_t ata_device_size_sectors(const struct ata_device_id* dev) {
    return dev ? dev->size_sectors : 0;
}

uint32_t disk_get_size_sectors() {
    if (current_controller == DISK_CONTROLLER_NVME) return nvme_get_disk_size();
    if (current_controller == DISK_CONTROLLER_AHCI) return ahci_get_disk_size();
    if (current_controller == DISK_CONTROLLER_ATA && current_ata_device_valid)
        return current_ata_device.size_sectors;
    return 0;
}
