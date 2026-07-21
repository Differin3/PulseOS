#include "drivers/storage/ahci.h"
#include "drivers/pci/pci.h"
#include "drivers/video/terminal.h"
#include "driver_manager.h"
#include "serial_log.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define AHCI_VENDOR_INTEL 0x8086
#define AHCI_ICH7_SATA    0x27C3
#define AHCI_ICH8M_SATA   0x2829
#define AHCI_ICH9_SATA    0x2922
#define AHCI_ICH_PANTHER  0x1E03

struct ahci_cmd_list {
    uint16_t flags;
    uint16_t prdt_length;
    uint32_t prdt_byte_count;
    uint32_t cmd_table_base;
    uint32_t cmd_table_base_upper;
    uint32_t reserved[4];
} __attribute__((packed));

struct ahci_cmd_table {
    uint8_t cfis[64];
    uint8_t acmd[32];
    uint8_t reserved[32];
    struct {
        uint32_t data_base;
        uint32_t data_base_upper;
        uint32_t reserved;
        uint32_t byte_count;
    } prdt[1];
} __attribute__((packed));

struct ahci_received_fis {
    uint8_t dsfis[64];
    uint8_t psfis[64];
    uint8_t rfis[128];
    uint8_t sdbfis[64];
    uint8_t ufis[64];
    uint8_t reserved[96];
} __attribute__((packed));

struct ahci_port_state {
    volatile uint32_t* mmio;
    uint8_t port_num;
    bool active;
    bool lba48;
    uint32_t size_sectors;
    struct ahci_cmd_list* cl;
    struct ahci_received_fis* fis;
    struct ahci_cmd_table* tables[AHCI_CMD_SLOTS];
};

static volatile uint32_t* ahci_base = 0;
static bool ahci_initialized = false;
static bool ahci_init_failed = false;
static struct ahci_port_state ahci_ports[AHCI_MAX_PORTS];
static int ahci_active_port_count = 0;
static struct ahci_port_state* ahci_current_port = 0;

// CL 1KB + FIS 1KB + 32 cmd tables @ 256 B each (xv64 layout)
#define AHCI_CMD_TABLE_STRIDE 256
#define AHCI_PORT_MEM_SIZE (2048 + AHCI_CMD_SLOTS * AHCI_CMD_TABLE_STRIDE)
static uint8_t ahci_port_mem[AHCI_MAX_PORTS][AHCI_PORT_MEM_SIZE] __attribute__((aligned(256)));

static inline void ahci_mmio_write32(volatile uint32_t* base, uint32_t offset, uint32_t val) {
    volatile uint32_t* addr = (volatile uint32_t*)((uintptr_t)base + offset);
    *addr = val;
}

static inline uint32_t ahci_mmio_read32(volatile uint32_t* base, uint32_t offset) {
    volatile uint32_t* addr = (volatile uint32_t*)((uintptr_t)base + offset);
    return *addr;
}

static void ahci_device_hacks(const struct pci_device* dev) {
    if (!dev || dev->vendor_id != AHCI_VENDOR_INTEL) return;
    if (dev->device_id == AHCI_ICH7_SATA ||
        dev->device_id == AHCI_ICH8M_SATA ||
        dev->device_id == AHCI_ICH9_SATA ||
        dev->device_id == AHCI_ICH_PANTHER) {
        uint32_t val = pci_read_config(dev->bus, dev->device, dev->function, 0x90);
        pci_write_config(dev->bus, dev->device, dev->function, 0x90, val | 0x40);
        log_msg(LOG_DBG, "ahci", "Intel AHCI mode hack applied");
    }
}

static int ahci_setup_pci(const struct pci_device* dev, volatile uint32_t** out_base) {
    if (!dev || !out_base) return -1;

    ahci_device_hacks(dev);

    uint32_t cmd = pci_read_config(dev->bus, dev->device, dev->function, 0x04);
    uint32_t new_cmd = cmd | (1u << 1) | (1u << 2);
    if (new_cmd != cmd) {
        pci_write_config(dev->bus, dev->device, dev->function, 0x04, new_cmd);
    }

    volatile uint32_t* base_addr = 0;
    if (dev->base_address[5] != 0) {
        base_addr = (volatile uint32_t*)(uintptr_t)dev->base_address[5];
    } else {
        for (int i = 0; i < 6; i++) {
            if (dev->base_address[i] != 0) {
                base_addr = (volatile uint32_t*)(uintptr_t)dev->base_address[i];
                break;
            }
        }
    }
    if (!base_addr) return -1;
    *out_base = base_addr;
    return 0;
}

static volatile uint32_t* find_ahci_controller() {
    struct pci_device ahci_dev;
    if (pci_find_device(0x01, 0x06, &ahci_dev) != 0) return 0;
    volatile uint32_t* base = 0;
    if (ahci_setup_pci(&ahci_dev, &base) != 0) return 0;
    return base;
}

static int ahci_stop_port(struct ahci_port_state* port) {
    if (!port || !port->mmio) return -1;
    volatile uint32_t* p = port->mmio;

    uint32_t cmd = ahci_mmio_read32(p, AHCI_PORT_CMD);
    cmd &= ~AHCI_PORT_CMD_ST;
    cmd &= ~AHCI_PORT_CMD_FRE;
    ahci_mmio_write32(p, AHCI_PORT_CMD, cmd);

    int count = 0;
    while ((ahci_mmio_read32(p, AHCI_PORT_CMD) & (AHCI_PORT_CMD_CR | AHCI_PORT_CMD_FR)) &&
           count < 1000) {
        count++;
    }
    if (count >= 1000) {
        log_msg(LOG_ERR, "ahci", "port stop timeout");
        return -1;
    }
    return 0;
}

static void ahci_port_clear_mem(uint8_t* mem, size_t size) {
    for (size_t i = 0; i < size; i++) mem[i] = 0;
}

static int ahci_port_rebase(struct ahci_port_state* port, uint8_t* mem) {
    if (!port || !port->mmio || !mem) return -1;

    port->cl = (struct ahci_cmd_list*)mem;
    port->fis = (struct ahci_received_fis*)(mem + 1024);
    uint8_t* tbl_base = mem + 2048;

    for (int i = 0; i < AHCI_CMD_SLOTS; i++) {
        port->tables[i] = (struct ahci_cmd_table*)(tbl_base + (size_t)i * AHCI_CMD_TABLE_STRIDE);
    }

    ahci_port_clear_mem(mem, AHCI_PORT_MEM_SIZE);

    uint32_t cl_phys = (uint32_t)(uintptr_t)port->cl;
    uint32_t fis_phys = (uint32_t)(uintptr_t)port->fis;

    ahci_mmio_write32(port->mmio, AHCI_PORT_LST_ADDR, cl_phys);
    ahci_mmio_write32(port->mmio, AHCI_PORT_LST_ADDR_UPPER, 0);
    ahci_mmio_write32(port->mmio, AHCI_PORT_FIS_ADDR, fis_phys);
    ahci_mmio_write32(port->mmio, AHCI_PORT_FIS_ADDR_UPPER, 0);

    for (int i = 0; i < AHCI_CMD_SLOTS; i++) {
        uint32_t ct_phys = (uint32_t)(uintptr_t)port->tables[i];
        port->cl[i].cmd_table_base = ct_phys;
        port->cl[i].cmd_table_base_upper = 0;
        port->cl[i].prdt_length = 0;
        port->cl[i].flags = 0;
    }

    ahci_mmio_write32(port->mmio, AHCI_PORT_SERR, 0xFFFFFFFF);
    return 0;
}

static void ahci_port_spinup(volatile uint32_t* hba, struct ahci_port_state* port) {
    if (!hba || !port || !port->mmio) return;

    uint32_t cap = ahci_mmio_read32(hba, AHCI_CAP);
    if (!(cap & AHCI_CAP_SSS)) return;

    uint32_t cmd = ahci_mmio_read32(port->mmio, AHCI_PORT_CMD);
    ahci_mmio_write32(port->mmio, AHCI_PORT_CMD, cmd | AHCI_PORT_CMD_SUD);

    for (int i = 0; i < 5000; i++) {
        uint32_t ssts = ahci_mmio_read32(port->mmio, AHCI_PORT_SSTS);
        uint8_t det = (uint8_t)(ssts & 0x0F);
        if (det == 3) break;
    }
}

static int ahci_start_port(struct ahci_port_state* port) {
    if (!port || !port->mmio) return -1;

    while (ahci_mmio_read32(port->mmio, AHCI_PORT_CMD) & AHCI_PORT_CMD_CR) {}

    uint32_t cmd = ahci_mmio_read32(port->mmio, AHCI_PORT_CMD);
    cmd |= AHCI_PORT_CMD_FRE;
    ahci_mmio_write32(port->mmio, AHCI_PORT_CMD, cmd);
    cmd |= AHCI_PORT_CMD_ST;
    ahci_mmio_write32(port->mmio, AHCI_PORT_CMD, cmd);

    ahci_mmio_write32(port->mmio, AHCI_PORT_IS, 0);
    ahci_mmio_write32(port->mmio, AHCI_PORT_IE, 0);
    return 0;
}

static int ahci_wait_tfd(struct ahci_port_state* port) {
    if (!port || !port->mmio) return -1;
    for (int i = 0; i < AHCI_IO_MAX_WAIT; i++) {
        uint32_t tfd = ahci_mmio_read32(port->mmio, AHCI_PORT_TFD);
        if (!(tfd & (AHCI_TFD_BSY | AHCI_TFD_DRQ))) return 0;
    }
    log_msg(LOG_ERR, "ahci", "TFD wait timeout");
    return -1;
}

static int ahci_find_cmdslot(struct ahci_port_state* port) {
    if (!port || !port->mmio) return -1;
    uint32_t slots = ahci_mmio_read32(port->mmio, AHCI_PORT_SACT) |
                     ahci_mmio_read32(port->mmio, AHCI_PORT_CI);
    for (int i = 0; i < AHCI_CMD_SLOTS; i++) {
        if (!(slots & (1u << i))) return i;
    }
    return -1;
}

static void ahci_log_port_diag(const char* tag, volatile uint32_t* port_mmio, uint8_t port_num, int level) {
    if (!port_mmio) return;
    uint32_t sig = ahci_mmio_read32(port_mmio, AHCI_PORT_SIG);
    uint32_t ssts = ahci_mmio_read32(port_mmio, AHCI_PORT_SSTS);
    uint32_t tfd = ahci_mmio_read32(port_mmio, AHCI_PORT_TFD);
    uint32_t serr = ahci_mmio_read32(port_mmio, AHCI_PORT_SERR);
    log_fmt3(level, "ahci", tag, "port", (uint32_t)port_num, "sig", sig, "ssts", ssts);
    log_fmt3(level, "ahci", tag, "tfd", tfd, "serr", serr, "det", (uint32_t)(ssts & 0x0F));
}

static int ahci_wait_cmd(struct ahci_port_state* port, int slot) {
    if (!port || !port->mmio || slot < 0) return -1;
    for (int i = 0; i < AHCI_IO_MAX_WAIT; i++) {
        if (!(ahci_mmio_read32(port->mmio, AHCI_PORT_CI) & (1u << slot))) {
            uint32_t is = ahci_mmio_read32(port->mmio, AHCI_PORT_IS);
            if (is & (AHCI_PxIS_TFES | AHCI_PxIS_HBFS | AHCI_PxIS_HBDS)) {
                log_fmt3(LOG_ERR, "ahci", "cmd err", "port", port->port_num, "is", is, "slot", (uint32_t)slot);
                ahci_log_port_diag("cmd err", port->mmio, port->port_num, LOG_ERR);
                return -1;
            }
            ahci_mmio_write32(port->mmio, AHCI_PORT_IS, is);
            return 0;
        }
    }
    log_msg(LOG_ERR, "ahci", "command timeout");
    return -1;
}

static void ahci_build_fis_identify(uint8_t* cfis) {
    for (int i = 0; i < 64; i++) cfis[i] = 0;
    cfis[0] = FIS_TYPE_REG_H2D;
    cfis[1] = 0x80;
    cfis[2] = ATA_CMD_IDENTIFY;
    cfis[7] = 0x00;
}

static void ahci_zero_cmd_table(struct ahci_cmd_table* tbl) {
    uint8_t* p = (uint8_t*)tbl;
    for (int i = 0; i < AHCI_CMD_TABLE_STRIDE; i++) p[i] = 0;
}

static void ahci_build_fis_dma_ext(uint8_t* cfis, uint8_t cmd, uint32_t lba, uint16_t count) {
    for (int i = 0; i < 64; i++) cfis[i] = 0;
    cfis[0] = FIS_TYPE_REG_H2D;
    cfis[1] = 0x80;
    cfis[2] = cmd;
    cfis[4] = (uint8_t)(lba & 0xFF);
    cfis[5] = (uint8_t)((lba >> 8) & 0xFF);
    cfis[6] = (uint8_t)((lba >> 16) & 0xFF);
    cfis[7] = 0x40;
    cfis[8] = (uint8_t)((lba >> 24) & 0xFF);
    cfis[9] = 0;
    cfis[10] = 0;
    cfis[12] = (uint8_t)(count & 0xFF);
    cfis[13] = (uint8_t)((count >> 8) & 0xFF);
}

static void ahci_setup_prdt(struct ahci_cmd_table* tbl, void* buffer, uint32_t bytes) {
    tbl->prdt[0].data_base = (uint32_t)(uintptr_t)buffer;
    tbl->prdt[0].data_base_upper = 0;
    tbl->prdt[0].byte_count = (bytes - 1u) | (1u << 31);
    tbl->prdt[0].reserved = 0;
}

static int ahci_sata_transfer(struct ahci_port_state* port, uint32_t lba, uint16_t count,
                                void* buffer, bool write) {
    if (!port || !port->active || !buffer || count == 0) return -1;

    int slot = ahci_find_cmdslot(port);
    if (slot < 0) {
        log_msg(LOG_ERR, "ahci", "no cmd slot");
        return -1;
    }

    struct ahci_cmd_table* tbl = port->tables[slot];
    struct ahci_cmd_list* hdr = &port->cl[slot];

    ahci_zero_cmd_table(tbl);
    uint8_t cmd = write ? ATA_CMD_WRITE_DMA_EXT : ATA_CMD_READ_DMA_EXT;
    ahci_build_fis_dma_ext(tbl->cfis, cmd, lba, count);
    ahci_setup_prdt(tbl, buffer, (uint32_t)count * 512u);

    hdr->prdt_length = 1;
    hdr->flags = (uint16_t)((1u << 8) | 5u);
    if (write) hdr->flags |= (1u << 6);

    ahci_mmio_write32(port->mmio, AHCI_PORT_IS, 0xFFFFFFFF);

    if (ahci_wait_tfd(port) != 0) return -1;

    ahci_mmio_write32(port->mmio, AHCI_PORT_CI, 1u << slot);
    return ahci_wait_cmd(port, slot);
}

static uint32_t ahci_parse_identify(uint16_t* identify) {
    if (identify[0] == 0x0000 && identify[1] == 0x0000) return 0;
    if (identify[0] == 0xFFFF && identify[1] == 0xFFFF) return 0;

    if (identify[83] & 0x0400) {
        uint32_t low = identify[100] | ((uint32_t)identify[101] << 16);
        uint32_t high = identify[102] | ((uint32_t)identify[103] << 16);
        if (high == 0 && low > 0) return low;
    }

    uint32_t lba28 = identify[60] | ((uint32_t)identify[61] << 16);
    return lba28;
}

static int ahci_port_identify(struct ahci_port_state* port) {
    static uint8_t identify_buf[512] __attribute__((aligned(4)));
    for (int i = 0; i < 512; i++) identify_buf[i] = 0;

    int slot = ahci_find_cmdslot(port);
    if (slot < 0) return -1;

    struct ahci_cmd_table* tbl = port->tables[slot];
    struct ahci_cmd_list* hdr = &port->cl[slot];

    ahci_zero_cmd_table(tbl);
    ahci_build_fis_identify(tbl->cfis);
    ahci_setup_prdt(tbl, identify_buf, 512);
    hdr->prdt_length = 1;
    hdr->flags = (uint16_t)((1u << 8) | 5u);

    ahci_mmio_write32(port->mmio, AHCI_PORT_IS, 0xFFFFFFFF);
    if (ahci_wait_tfd(port) != 0) return -1;
    ahci_mmio_write32(port->mmio, AHCI_PORT_CI, 1u << slot);
    if (ahci_wait_cmd(port, slot) != 0) return -1;

    uint16_t* identify = (uint16_t*)identify_buf;
    port->lba48 = (identify[83] & 0x0400) != 0;
    port->size_sectors = ahci_parse_identify(identify);
    return port->size_sectors > 0 ? 0 : -1;
}

static bool ahci_port_is_sata(volatile uint32_t* port_mmio, uint8_t port_num) {
    uint32_t sig = ahci_mmio_read32(port_mmio, AHCI_PORT_SIG);
    uint32_t ssts = ahci_mmio_read32(port_mmio, AHCI_PORT_SSTS);
    if (sig == 0xFFFFFFFF || ssts == 0xFFFFFFFF) return false;
    uint8_t det = (uint8_t)(ssts & 0x0F);
    bool ok = sig == SATA_SIG_ATA && det == 3;
    if (!ok && sig == SATA_SIG_ATA && det != 0) {
        log_fmt3(LOG_DBG, "ahci", "port skip", "port", (uint32_t)port_num, "sig", sig, "det", (uint32_t)det);
    }
    return ok;
}

static int ahci_port_init(volatile uint32_t* hba, int port_num, struct ahci_port_state* port,
                            uint8_t* mem) {
    volatile uint32_t* port_mmio =
        (volatile uint32_t*)((uintptr_t)hba + 0x100 + (uintptr_t)port_num * 0x80);

    if (!ahci_port_is_sata(port_mmio, (uint8_t)port_num)) return -1;

    port->mmio = port_mmio;
    port->port_num = (uint8_t)port_num;
    port->active = false;
    port->lba48 = false;
    port->size_sectors = 0;

    ahci_port_spinup(hba, port);
    if (ahci_stop_port(port) != 0) return -1;
    if (ahci_port_rebase(port, mem) != 0) return -1;
    if (ahci_start_port(port) != 0) return -1;

    if (ahci_port_identify(port) != 0) {
        ahci_log_port_diag("identify fail", port_mmio, (uint8_t)port_num, LOG_ERR);
        return -1;
    }

    port->active = true;
    log_fmt3(LOG_INFO, "ahci", "port ready", "port", (uint32_t)port_num,
             "sectors", port->size_sectors, "lba48", port->lba48 ? 1u : 0u);
    return 0;
}

static int ahci_driver_read(void* device_data, void* buffer, size_t size, uint32_t offset) {
    struct ahci_port_state* port = (struct ahci_port_state*)device_data;
    if (!port) port = ahci_current_port;
    if (!port) return -1;
    ahci_select_port(port);
    uint32_t lba = offset;
    size_t sectors = (size + 511) / 512;
    return ahci_read_sectors(lba, sectors, buffer);
}

static int ahci_driver_write(void* device_data, const void* buffer, size_t size, uint32_t offset) {
    struct ahci_port_state* port = (struct ahci_port_state*)device_data;
    if (!port) port = ahci_current_port;
    if (!port) return -1;
    ahci_select_port(port);
    uint32_t lba = offset;
    size_t sectors = (size + 511) / 512;
    return ahci_write_sectors(lba, sectors, buffer);
}

static int ahci_driver_ioctl(void* device_data, uint32_t cmd, void* arg) {
    struct ahci_port_state* port = (struct ahci_port_state*)device_data;
    if (!port) port = ahci_current_port;
    if (cmd == 0 && arg && port) {
        uint32_t* size = (uint32_t*)arg;
        *size = port->size_sectors;
        return 0;
    }
    return -1;
}

static int ahci_register_port_driver(struct ahci_port_state* port, int index) {
    struct driver ahci_driver;
    ahci_driver.name[0] = 'a';
    ahci_driver.name[1] = 'h';
    ahci_driver.name[2] = 'c';
    ahci_driver.name[3] = 'i';
    ahci_driver.name[4] = '0' + (char)index;
    ahci_driver.name[5] = 0;
    ahci_driver.type = DRIVER_STORAGE;
    ahci_driver.device_id = 0;
    ahci_driver.device_data = port;
    ahci_driver.initialized = true;
    ahci_driver.active = (index == 0);
    ahci_driver.ops.init = 0;
    ahci_driver.ops.read = ahci_driver_read;
    ahci_driver.ops.write = ahci_driver_write;
    ahci_driver.ops.ioctl = ahci_driver_ioctl;
    ahci_driver.ops.cleanup = 0;

    if (driver_register(&ahci_driver) != 0) {
        log_msg(LOG_ERR, "ahci", "driver register failed");
        return -1;
    }
    if (index == 0) ahci_current_port = port;
    return 0;
}

static int ahci_init_common() {
    uint32_t ghc = ahci_mmio_read32(ahci_base, AHCI_GHC);
    ahci_mmio_write32(ahci_base, AHCI_GHC, ghc | (1u << 31));
    for (int i = 0; i < 1000; i++) {
        if (ahci_mmio_read32(ahci_base, AHCI_GHC) & (1u << 31)) break;
    }

    uint32_t pi = ahci_mmio_read32(ahci_base, AHCI_PI);
    if (pi == 0xFFFFFFFF) {
        log_msg(LOG_ERR, "ahci", "MMIO not accessible");
        return -1;
    }
    if (pi == 0) {
        log_msg(LOG_ERR, "ahci", "no active ports");
        return -1;
    }

    ahci_active_port_count = 0;
    for (int p = 0; p < 32 && ahci_active_port_count < AHCI_MAX_PORTS; p++) {
        if (!(pi & (1u << p))) continue;
        struct ahci_port_state* port = &ahci_ports[ahci_active_port_count];
        if (ahci_port_init(ahci_base, p, port, ahci_port_mem[ahci_active_port_count]) != 0) {
            continue;
        }
        if (ahci_register_port_driver(port, ahci_active_port_count) != 0) {
            port->active = false;
            continue;
        }
        ahci_active_port_count++;
    }

    if (ahci_active_port_count == 0) {
        log_msg(LOG_ERR, "ahci", "no SATA device (use run-qemu-ahci.bat + myos.img)");
        terminal_writestring("[ERROR] AHCI: No SATA device found\n");
        ahci_init_failed = true;
        return -1;
    }

    ahci_initialized = true;
    return 0;
}

int ahci_init() {
    if (ahci_initialized) return 0;
    if (ahci_init_failed) return -1;
    volatile uint32_t* base_addr = find_ahci_controller();
    if (!base_addr) return -1;
    ahci_base = base_addr;
    int rc = ahci_init_common();
    if (rc != 0) ahci_init_failed = true;
    return rc;
}

int ahci_init_with_device(const struct pci_device* dev) {
    if (ahci_initialized) return 0;
    if (ahci_init_failed) return -1;
    if (!dev) return -1;
    volatile uint32_t* base_addr = 0;
    if (ahci_setup_pci(dev, &base_addr) != 0) return -1;
    ahci_base = base_addr;
    int rc = ahci_init_common();
    if (rc != 0) ahci_init_failed = true;
    return rc;
}

void ahci_select_port(struct ahci_port_state* port) {
    ahci_current_port = port;
}

struct ahci_port_state* ahci_get_port(int index) {
    if (index < 0 || index >= ahci_active_port_count) return 0;
    return &ahci_ports[index];
}

int ahci_port_count() {
    return ahci_active_port_count;
}

uint32_t ahci_port_disk_size(struct ahci_port_state* port) {
    return port ? port->size_sectors : 0;
}

uint8_t ahci_port_number(struct ahci_port_state* port) {
    return port ? port->port_num : 0xFF;
}

static struct ahci_port_state* ahci_resolve_port() {
    if (ahci_current_port) return ahci_current_port;
    if (ahci_active_port_count > 0) return &ahci_ports[0];
    return 0;
}

int ahci_read_sector(uint32_t lba, void* buffer) {
    return ahci_read_sectors(lba, 1, buffer);
}

int ahci_write_sector(uint32_t lba, const void* buffer) {
    return ahci_write_sectors(lba, 1, buffer);
}

int ahci_read_sectors(uint32_t lba, size_t count, void* buffer) {
    struct ahci_port_state* port = ahci_resolve_port();
    if (!ahci_initialized || !port || !buffer) return -1;

    uint8_t* buf = (uint8_t*)buffer;
    for (size_t i = 0; i < count; i++) {
        if (ahci_sata_transfer(port, lba + (uint32_t)i, 1, buf + i * 512, false) != 0) {
            return -1;
        }
    }
    return 0;
}

int ahci_write_sectors(uint32_t lba, size_t count, const void* buffer) {
    struct ahci_port_state* port = ahci_resolve_port();
    if (!ahci_initialized || !port || !buffer) return -1;

    const uint8_t* buf = (const uint8_t*)buffer;
    for (size_t i = 0; i < count; i++) {
        if (ahci_sata_transfer(port, lba + (uint32_t)i, 1, (void*)(buf + i * 512), true) != 0) {
            return -1;
        }
    }
    return 0;
}

uint32_t ahci_get_disk_size() {
    struct ahci_port_state* port = ahci_resolve_port();
    return port ? port->size_sectors : 0;
}
