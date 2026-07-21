#include "rtl8139.h"
#include "../../nic.h"
#include "drivers/pci/pci.h"
#include "serial_log.h"

#define ETH_MAX_PACKET_SIZE 1514
#define ETH_ZLEN              60

#define RTL8139_MAC0        0x00
#define RTL8139_MAC4        0x04
#define RTL8139_TXSTATUS0   0x10
#define RTL8139_TXADDR0     0x20
#define RTL8139_RBSTART     0x30
#define RTL8139_CMD         0x37
#define RTL8139_CAPR        0x38
#define RTL8139_CBR         0x3A
#define RTL8139_INTR_MASK   0x3C
#define RTL8139_ISR         0x3E
#define RTL8139_TX_CONFIG   0x40
#define RTL8139_RX_CONFIG   0x44

#define RTL8139_TX_OWN      (1 << 13)
#define RTL8139_TX_TOK      (1 << 15)
#define RTL8139_ISR_TOK     0x04
#define RTL8139_ISR_ROK     0x01
#define RTL8139_CMD_BUFE    0x01

#define RTL8139_CMD_RESET   (1 << 4)
#define RTL8139_CMD_RX_EN   (1 << 3)
#define RTL8139_CMD_TX_EN   (1 << 2)

#define RTL8139_RX_STATUS_OK   0x0001
#define RTL8139_RX_STATUS_BAD  0x003E

#define RTL8139_RX_BUF_SIZE 8192
#define RTL8139_RX_BUF_PAD  16
#define RTL8139_TX_BUF_SIZE 2048

static uint8_t rtl8139_rx_buffer[RTL8139_RX_BUF_SIZE + RTL8139_RX_BUF_PAD] __attribute__((aligned(256)));
static uint8_t rtl8139_tx_buffers[4][RTL8139_TX_BUF_SIZE] __attribute__((aligned(256)));
static int rtl8139_tx_desc = 0;
static uint32_t rtl8139_cur_rx = 0;

static inline void outb(uint16_t port, uint8_t val) { asm volatile ("outb %0, %1" : : "a"(val), "Nd"(port)); }
static inline void outw(uint16_t port, uint16_t val) { asm volatile ("outw %0, %1" : : "a"(val), "Nd"(port)); }
static inline void outl(uint16_t port, uint32_t val) { asm volatile ("outl %0, %1" : : "a"(val), "Nd"(port)); }
static inline uint8_t inb(uint16_t port) { uint8_t ret; asm volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port)); return ret; }
static inline uint16_t inw(uint16_t port) { uint16_t ret; asm volatile ("inw %1, %0" : "=a"(ret) : "Nd"(port)); return ret; }
static inline uint32_t inl(uint16_t port) { uint32_t ret; asm volatile ("inl %1, %0" : "=a"(ret) : "Nd"(port)); return ret; }

static void rtl8139_delay() {
    for (volatile int i = 0; i < 1000; i++);
}

static inline uint32_t rtl8139_ring_offset(uint32_t cur_rx) {
    return cur_rx % RTL8139_RX_BUF_SIZE;
}

static void rtl8139_update_capr(uint16_t io, uint32_t cur_rx) {
    uint16_t capr = (uint16_t)(cur_rx - 16);
    outw(io + RTL8139_CAPR, capr);
}

static void rtl8139_ack_rx(uint16_t io) {
    outw(io + RTL8139_ISR, RTL8139_ISR_ROK);
}

static void rtl8139_rx_reset(struct nic_device* nic) {
    if (!nic) return;
    uint16_t io = nic->io_base;

    rtl8139_cur_rx = 0;
    nic->rx_current = 0;
    rtl8139_update_capr(io, 0);
    rtl8139_ack_rx(io);

    log_msg(LOG_ERR, "rtl8139", "rx ring reset");
}

int find_rtl8139(struct pci_device* dev) {
    for (int bus = 0; bus < 16; bus++) {
        for (uint8_t device = 0; device < 32; device++) {
            for (uint8_t function = 0; function < 8; function++) {
                uint32_t vd = pci_read_config((uint8_t)bus, device, function, 0x00);
                uint16_t vendor_id = vd & 0xFFFF;
                uint16_t device_id = (vd >> 16) & 0xFFFF;
                if (vendor_id == 0x10EC && device_id == 0x8139) {
                    dev->vendor_id = vendor_id;
                    dev->device_id = device_id;
                    dev->bus = (uint8_t)bus;
                    dev->device = device;
                    dev->function = function;
                    for (int i = 0; i < 6; i++) {
                        dev->base_address[i] = pci_read_config((uint8_t)bus, device, function, 0x10 + i * 4);
                    }
                    return 0;
                }
            }
        }
    }
    return -1;
}

static int rtl8139_wait_reset(uint16_t io_base) {
    for (int i = 0; i < 10000; i++) {
        if (!(inb(io_base + RTL8139_CMD) & RTL8139_CMD_RESET)) {
            return 0;
        }
        rtl8139_delay();
    }
    return -1;
}

static int rtl8139_init_common(struct nic_device* nic) {
    nic->rx_buffer = rtl8139_rx_buffer;
    nic->tx_buffer = rtl8139_tx_buffers[0];
    nic->rx_current = 0;
    rtl8139_cur_rx = 0;
    rtl8139_tx_desc = 0;

    uint16_t io = nic->io_base;

    outb(io + RTL8139_CMD, inb(io + RTL8139_CMD) | RTL8139_CMD_RESET);
    if (rtl8139_wait_reset(io) != 0) {
        log_msg(LOG_ERR, "rtl8139", "reset timeout");
        return -1;
    }

    for (size_t i = 0; i < sizeof(rtl8139_rx_buffer); i++) {
        rtl8139_rx_buffer[i] = 0;
    }

    uint32_t rx_phys = (uint32_t)(uintptr_t)rtl8139_rx_buffer;
    outl(io + RTL8139_RBSTART, rx_phys);

    for (int i = 0; i < 4; i++) {
        uint32_t tx_phys = (uint32_t)(uintptr_t)rtl8139_tx_buffers[i];
        outl(io + RTL8139_TXADDR0 + (uint16_t)(i * 4), tx_phys);
        outl(io + RTL8139_TXSTATUS0 + (uint16_t)(i * 4), RTL8139_TX_OWN);
    }

    for (int i = 0; i < 8; i++) {
        outl(io + 0x08 + (uint16_t)(i * 4), 0xFFFFFFFFu);
    }

    outw(io + RTL8139_INTR_MASK, 0x0000);
    outl(io + RTL8139_RX_CONFIG, 0x0F | (1 << 7));
    outl(io + RTL8139_TX_CONFIG, 0x03000700);
    outw(io + RTL8139_ISR, 0xFFFF);

    uint32_t mac_low = inl(io + RTL8139_MAC0);
    uint16_t mac_high = inw(io + RTL8139_MAC4);
    nic->mac_address[0] = mac_low & 0xFF;
    nic->mac_address[1] = (mac_low >> 8) & 0xFF;
    nic->mac_address[2] = (mac_low >> 16) & 0xFF;
    nic->mac_address[3] = (mac_low >> 24) & 0xFF;
    nic->mac_address[4] = mac_high & 0xFF;
    nic->mac_address[5] = (mac_high >> 8) & 0xFF;

    bool mac_valid = false;
    for (int i = 0; i < 6; i++) {
        if (nic->mac_address[i] != 0) {
            mac_valid = true;
            break;
        }
    }
    if (!mac_valid) {
        log_msg(LOG_ERR, "rtl8139", "invalid MAC after init");
        return -1;
    }

    outb(io + RTL8139_CMD, RTL8139_CMD_RX_EN | RTL8139_CMD_TX_EN);
    nic->rx_current = 0;
    rtl8139_cur_rx = 0;
    rtl8139_update_capr(io, 0);
    outw(io + RTL8139_ISR, 0xFFFF);

    nic->initialized = true;
    nic->active = true;
    nic->hw_type = NIC_HW_RTL8139;
    log_fmt3(LOG_INFO, "rtl8139", "initialized", "io", (uint32_t)io,
             "rx_phys", rx_phys, "tx0", (uint32_t)(uintptr_t)rtl8139_tx_buffers[0]);
    log_fmt3(LOG_INFO, "rtl8139", "rx ring init", "cur_rx", 0u,
             "capr", (uint32_t)inw(io + RTL8139_CAPR),
             "bufempty", (uint32_t)((inb(io + RTL8139_CMD) & RTL8139_CMD_BUFE) ? 1u : 0u));
    return 0;
}

int rtl8139_init_with_pci(const struct pci_device* dev, struct nic_device* nic) {
    if (!dev || !nic) return -1;
    if (dev->vendor_id != 0x10EC || dev->device_id != 0x8139) return -1;
    if (dev->base_address[0] == 0 || !(dev->base_address[0] & 1)) return -1;

    uint32_t cmd = pci_read_config(dev->bus, dev->device, dev->function, 0x04);
    uint32_t new_cmd = cmd | 0x07;
    if (new_cmd != cmd) {
        pci_write_config(dev->bus, dev->device, dev->function, 0x04, new_cmd);
    }

    nic->io_base = (uint16_t)(dev->base_address[0] & 0xFFFFFFFC);
    nic->mem_base = 0;
    return rtl8139_init_common(nic);
}

static bool rtl8139_tx_complete(uint16_t io, uint16_t tx_status_reg, uint32_t* status_out) {
    uint32_t st = inl(io + tx_status_reg);
    if (status_out) *status_out = st;
    if (st & RTL8139_TX_TOK) {
        outw(io + RTL8139_ISR, RTL8139_ISR_TOK);
        return true;
    }
    uint16_t isr = inw(io + RTL8139_ISR);
    if (isr & RTL8139_ISR_TOK) {
        outw(io + RTL8139_ISR, RTL8139_ISR_TOK);
        return true;
    }
    return false;
}

int rtl8139_send_packet(struct nic_device* nic, const void* data, size_t len) {
    if (!nic || !nic->active || !data || len == 0) return -1;
    if (len > 1792) return -1;

    uint16_t io = nic->io_base;
    int desc = rtl8139_tx_desc & 3;
    uint16_t tx_status_reg = RTL8139_TXSTATUS0 + (uint16_t)(desc * 4);
    uint16_t tx_addr_reg = RTL8139_TXADDR0 + (uint16_t)(desc * 4);

    bool ready = false;
    uint32_t st_before = 0;
    for (int i = 0; i < 1000000; i++) {
        st_before = inl(io + tx_status_reg);
        if (st_before & RTL8139_TX_OWN) {
            ready = true;
            break;
        }
        rtl8139_delay();
    }
    if (!ready) {
        outl(io + tx_status_reg, RTL8139_TX_OWN);
        log_fmt3(LOG_ERR, "rtl8139", "tx busy (OWN=0)", "tsd", st_before, "desc", (uint32_t)desc, "len", (uint32_t)len);
        return -1;
    }

    uint8_t* tx = rtl8139_tx_buffers[desc];
    const uint8_t* src = (const uint8_t*)data;
    for (size_t i = 0; i < len; i++) {
        tx[i] = src[i];
    }
    size_t tx_len = len;
    while (tx_len < ETH_ZLEN && tx_len < RTL8139_TX_BUF_SIZE) {
        tx[tx_len++] = 0;
    }

    uint32_t tx_phys = (uint32_t)(uintptr_t)tx;
    outl(io + tx_addr_reg, tx_phys);
    asm volatile("" ::: "memory");

    log_fmt3(LOG_DBG, "rtl8139", "tx start", "desc", (uint32_t)desc, "phys", tx_phys, "len", (uint32_t)tx_len);

    outl(io + tx_status_reg, (uint32_t)(tx_len & 0x1FFF));

    uint32_t last_status = 0;
    for (int i = 0; i < 1000000; i++) {
        if (rtl8139_tx_complete(io, tx_status_reg, &last_status)) {
            rtl8139_tx_desc = (desc + 1) & 3;
            log_fmt3(LOG_DBG, "rtl8139", "tx ok", "tsd", last_status, "desc", (uint32_t)desc, "len", (uint32_t)len);
            return (int)len;
        }
        rtl8139_delay();
    }

    uint16_t isr = inw(io + RTL8139_ISR);
    log_fmt3(LOG_ERR, "rtl8139", "tx timeout", "tsd", last_status, "isr", (uint32_t)isr, "len", (uint32_t)len);
    outl(io + tx_status_reg, RTL8139_TX_OWN);
    return -1;
}

void rtl8139_rx_regs(struct nic_device* nic, uint8_t* cmd, uint16_t* isr, uint16_t* capr, uint16_t* cbr) {
    if (!nic || !nic->active) {
        if (cmd) *cmd = 0;
        if (isr) *isr = 0;
        if (capr) *capr = 0;
        if (cbr) *cbr = 0;
        return;
    }
    uint16_t io = nic->io_base;
    if (cmd) *cmd = inb(io + RTL8139_CMD);
    if (isr) *isr = inw(io + RTL8139_ISR);
    if (capr) *capr = inw(io + RTL8139_CAPR);
    if (cbr) *cbr = inw(io + RTL8139_CBR);
}

bool rtl8139_has_packet(struct nic_device* nic) {
    if (!nic || !nic->active) return false;
    uint8_t cmd = inb(nic->io_base + RTL8139_CMD);
    return (cmd & RTL8139_CMD_BUFE) == 0;
}

int rtl8139_receive_packet(struct nic_device* nic, void* buffer, size_t max_len) {
    if (!nic || !nic->active || !buffer || max_len == 0) return 0;
    if (!rtl8139_has_packet(nic)) return 0;

    uint16_t io = nic->io_base;
    uint32_t cur_rx = rtl8139_cur_rx;
    uint32_t ring_offs = rtl8139_ring_offset(cur_rx);

    uint32_t header = *(volatile uint32_t*)(rtl8139_rx_buffer + ring_offs);
    uint16_t status = (uint16_t)(header & 0xFFFF);
    uint16_t rx_size = (uint16_t)((header >> 16) & 0xFFFF);

    if (rx_size == 0xFFF0) {
        return 0;
    }

    if ((status & RTL8139_RX_STATUS_OK) == 0
        || (status & RTL8139_RX_STATUS_BAD) != 0
        || rx_size < ETH_ZLEN
        || rx_size > (ETH_MAX_PACKET_SIZE + 4)) {
        log_fmt3(LOG_ERR, "rtl8139", "rx error", "status", (uint32_t)status, "size", (uint32_t)rx_size,
                 "off", ring_offs);
        rtl8139_rx_reset(nic);
        return 0;
    }

    uint16_t pkt_len = (uint16_t)(rx_size - 4);
    if (pkt_len > max_len) {
        pkt_len = (uint16_t)max_len;
    }

    uint8_t* dst = (uint8_t*)buffer;
    for (uint16_t i = 0; i < pkt_len; i++) {
        uint32_t src_off = rtl8139_ring_offset(cur_rx + 4 + i);
        dst[i] = rtl8139_rx_buffer[src_off];
    }

    cur_rx = (cur_rx + rx_size + 4 + 3) & ~3u;
    rtl8139_cur_rx = cur_rx;
    nic->rx_current = (uint16_t)rtl8139_ring_offset(cur_rx);
    rtl8139_update_capr(io, cur_rx);
    rtl8139_ack_rx(io);

    log_fmt3(LOG_INFO, "rtl8139", "rx ok", "size", (uint32_t)pkt_len, "off", ring_offs,
             "cur_rx", cur_rx);
    return (int)pkt_len;
}
