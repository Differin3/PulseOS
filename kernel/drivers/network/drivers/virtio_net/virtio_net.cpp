#include "virtio_net.h"
#include "../../nic.h"
#include "drivers/pci/pci.h"
#include "drivers/pic/pic.h"
#include "serial_log.h"

#define VIRTIO_VENDOR 0x1AF4
#define VIRTIO_NET_DEVICE 0x1000

#define VIRTIO_PCI_HOST_FEATURES 0x00
#define VIRTIO_PCI_GUEST_FEATURES 0x04
#define VIRTIO_PCI_QUEUE_PFN 0x08
#define VIRTIO_PCI_QUEUE_NUM 0x0C
#define VIRTIO_PCI_QUEUE_SEL 0x0E
#define VIRTIO_PCI_QUEUE_NOTIFY 0x10
#define VIRTIO_PCI_STATUS 0x12
#define VIRTIO_PCI_ISR 0x13
#define VIRTIO_PCI_MAC 0x14

#define VIRTIO_STATUS_ACK 1
#define VIRTIO_STATUS_DRIVER 2
#define VIRTIO_STATUS_DRIVER_OK 4

#define VIRTIO_NET_F_MAC (1u << 5)

#define VRING_DESC_F_WRITE 2
#define VIRTQ_RX 0
#define VIRTQ_TX 1
#define VIRTQ_SIZE 256
#define VIRTQ_BUF_SIZE 1600
struct virtio_net_hdr {
    uint8_t flags;
    uint8_t gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
} __attribute__((packed));

struct vring_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

struct vring_used_elem {
    uint32_t id;
    uint32_t len;
} __attribute__((packed));

// Legacy layout: desc (4096) | avail (516) | pad (3580) | used @ 8192
#define VIRTQ_AVAIL_BYTES (4 + 2 * VIRTQ_SIZE)
#define VIRTQ_DESC_BYTES  (16 * VIRTQ_SIZE)
#define VIRTQ_PAD_BYTES   (8192 - VIRTQ_DESC_BYTES - VIRTQ_AVAIL_BYTES)

struct virtq_mem {
    struct vring_desc desc[VIRTQ_SIZE];
    uint16_t avail_flags;
    uint16_t avail_idx;
    uint16_t avail_ring[VIRTQ_SIZE];
    uint8_t pad[VIRTQ_PAD_BYTES];
    uint16_t used_flags;
    uint16_t used_idx;
    struct vring_used_elem used_ring[VIRTQ_SIZE];
} __attribute__((aligned(4096)));

struct virtq {
    struct virtq_mem* mem;
    uint16_t last_used_idx;
    uint16_t free_head;
    uint16_t num_free;
    uint16_t size;
};

static struct virtq_mem rx_mem __attribute__((aligned(4096)));
static struct virtq_mem tx_mem __attribute__((aligned(4096)));
static struct virtq rx_vq;
static struct virtq tx_vq;
static uint8_t rx_bufs[VIRTQ_SIZE][VIRTQ_BUF_SIZE] __attribute__((aligned(16)));
static uint8_t tx_bufs[VIRTQ_SIZE][VIRTQ_BUF_SIZE] __attribute__((aligned(16)));
static uint16_t virtio_io = 0;
static bool virtio_ready = false;

static inline void outb(uint16_t port, uint8_t val) { asm volatile ("outb %0, %1" : : "a"(val), "Nd"(port)); }
static inline void outw(uint16_t port, uint16_t val) { asm volatile ("outw %0, %1" : : "a"(val), "Nd"(port)); }
static inline void outl(uint16_t port, uint32_t val) { asm volatile ("outl %0, %1" : : "a"(val), "Nd"(port)); }
static inline uint8_t inb(uint16_t port) { uint8_t r; asm volatile ("inb %1, %0" : "=a"(r) : "Nd"(port)); return r; }
static inline uint16_t inw(uint16_t port) { uint16_t r; asm volatile ("inw %1, %0" : "=a"(r) : "Nd"(port)); return r; }
static inline uint32_t inl(uint16_t port) { uint32_t r; asm volatile ("inl %1, %0" : "=a"(r) : "Nd"(port)); return r; }

static void virtq_reset(struct virtq* vq, struct virtq_mem* mem, uint16_t size) {
    vq->mem = mem;
    vq->size = size;
    vq->last_used_idx = 0;
    vq->free_head = 0;
    vq->num_free = size;
    for (uint16_t i = 0; i < size; i++) {
        mem->desc[i].addr = 0;
        mem->desc[i].len = 0;
        mem->desc[i].flags = 0;
        mem->desc[i].next = (uint16_t)(i + 1);
        mem->avail_ring[i] = 0;
        mem->used_ring[i].id = 0;
        mem->used_ring[i].len = 0;
    }
    mem->desc[size - 1].next = 0xFFFF;
    mem->avail_flags = 0;
    mem->avail_idx = 0;
    mem->used_flags = 0;
    mem->used_idx = 0;
}

static int virtq_alloc_desc(struct virtq* vq) {
    if (vq->num_free == 0) return -1;
    uint16_t d = vq->free_head;
    vq->free_head = vq->mem->desc[d].next;
    vq->num_free--;
    vq->mem->desc[d].next = 0;
    vq->mem->desc[d].flags = 0;
    return (int)d;
}

static void virtq_free_desc(struct virtq* vq, uint16_t d) {
    vq->mem->desc[d].next = vq->free_head;
    vq->free_head = d;
    vq->num_free++;
}

static void virtq_setup_device(uint16_t io, uint16_t queue_sel, struct virtq* vq) {
    outw(io + VIRTIO_PCI_QUEUE_SEL, queue_sel);
    uint32_t pfn = ((uint32_t)(uintptr_t)vq->mem) >> 12;
    outl(io + VIRTIO_PCI_QUEUE_PFN, pfn);
}

static void virtio_rx_refill(uint16_t io) {
    while (rx_vq.num_free > 0) {
        int d = virtq_alloc_desc(&rx_vq);
        if (d < 0) break;
        rx_vq.mem->desc[d].addr = (uint64_t)(uintptr_t)rx_bufs[d];
        rx_vq.mem->desc[d].len = VIRTQ_BUF_SIZE;
        rx_vq.mem->desc[d].flags = VRING_DESC_F_WRITE;
        rx_vq.mem->desc[d].next = 0;
        uint16_t avail_idx = rx_vq.mem->avail_idx;
        rx_vq.mem->avail_ring[avail_idx % rx_vq.size] = (uint16_t)d;
        asm volatile("" ::: "memory");
        rx_vq.mem->avail_idx = (uint16_t)(avail_idx + 1);
        outw(io + VIRTIO_PCI_QUEUE_NOTIFY, VIRTQ_RX);
    }
}

int find_virtio_net(struct pci_device* dev) {
    if (!dev) return -1;
    for (int bus = 0; bus < 16; bus++) {
        for (uint8_t device = 0; device < 32; device++) {
            for (uint8_t function = 0; function < 8; function++) {
                uint32_t vd = pci_read_config((uint8_t)bus, device, function, 0x00);
                uint16_t vendor_id = (uint16_t)(vd & 0xFFFF);
                uint16_t device_id = (uint16_t)(vd >> 16);
                if (vendor_id != VIRTIO_VENDOR || device_id != VIRTIO_NET_DEVICE) continue;
                uint32_t bar0 = pci_read_config((uint8_t)bus, device, function, 0x10);
                if (!(bar0 & 1)) continue; // need legacy I/O BAR
                // Subsystem device id 1 = net (transitional)
                uint32_t subsys = pci_read_config((uint8_t)bus, device, function, 0x2C);
                uint16_t subsys_id = (uint16_t)(subsys >> 16);
                if (subsys_id != 0 && subsys_id != 1) continue;
                dev->vendor_id = vendor_id;
                dev->device_id = device_id;
                dev->bus = (uint8_t)bus;
                dev->device = device;
                dev->function = function;
                for (int i = 0; i < 6; i++) {
                    dev->base_address[i] = pci_read_config((uint8_t)bus, device, function, (uint8_t)(0x10 + i * 4));
                }
                return 0;
            }
        }
    }
    return -1;
}

int virtio_net_init_with_pci(const struct pci_device* dev, struct nic_device* nic) {
    if (!dev || !nic) return -1;
    if (!(dev->base_address[0] & 1)) return -1;

    uint32_t cmd = pci_read_config(dev->bus, dev->device, dev->function, 0x04);
    pci_write_config(dev->bus, dev->device, dev->function, 0x04, cmd | 0x07);

    uint16_t io = (uint16_t)(dev->base_address[0] & 0xFFFFFFFC);
    virtio_io = io;

    outb(io + VIRTIO_PCI_STATUS, 0);
    outb(io + VIRTIO_PCI_STATUS, VIRTIO_STATUS_ACK);
    outb(io + VIRTIO_PCI_STATUS, (uint8_t)(VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER));

    uint32_t host_features = inl(io + VIRTIO_PCI_HOST_FEATURES);
    uint32_t guest_features = 0;
    if (host_features & VIRTIO_NET_F_MAC) guest_features |= VIRTIO_NET_F_MAC;
    outl(io + VIRTIO_PCI_GUEST_FEATURES, guest_features);

    outw(io + VIRTIO_PCI_QUEUE_SEL, VIRTQ_RX);
    uint16_t qsz = inw(io + VIRTIO_PCI_QUEUE_NUM);
    if (qsz == 0 || qsz > VIRTQ_SIZE) {
        log_fmt3(LOG_ERR, "virtio", "bad rx qsz", "qsz", (uint32_t)qsz, "max", (uint32_t)VIRTQ_SIZE, "io", (uint32_t)io);
        outb(io + VIRTIO_PCI_STATUS, 0x80);
        return -1;
    }

    virtq_reset(&rx_vq, &rx_mem, qsz);
    virtq_reset(&tx_vq, &tx_mem, qsz);
    virtq_setup_device(io, VIRTQ_RX, &rx_vq);
    virtq_setup_device(io, VIRTQ_TX, &tx_vq);
    virtio_rx_refill(io);

    if (guest_features & VIRTIO_NET_F_MAC) {
        for (int i = 0; i < 6; i++) nic->mac_address[i] = inb((uint16_t)(io + VIRTIO_PCI_MAC + i));
    } else {
        nic->mac_address[0] = 0x52; nic->mac_address[1] = 0x54; nic->mac_address[2] = 0x00;
        nic->mac_address[3] = 0x12; nic->mac_address[4] = 0x34; nic->mac_address[5] = 0x56;
    }

    uint32_t irq_reg = pci_read_config(dev->bus, dev->device, dev->function, 0x3C);
    nic->irq_line = (uint8_t)(irq_reg & 0xFF);
    nic->pci_bus = dev->bus;
    nic->pci_device = dev->device;
    nic->pci_function = dev->function;
    nic->io_base = io;
    nic->mem_base = 0;
    nic->hw_type = NIC_HW_VIRTIO;
    nic->initialized = true;
    nic->active = true;
    virtio_ready = true;

    outb(io + VIRTIO_PCI_STATUS, (uint8_t)(VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_DRIVER_OK));
    log_fmt3(LOG_INFO, "virtio", "initialized", "io", (uint32_t)io, "irq", (uint32_t)nic->irq_line, "qsz", (uint32_t)qsz);
    return 0;
}

void virtio_net_enable_irq(struct nic_device* nic) {
    if (!nic || !nic->active) return;
    if (nic->irq_line > 0 && nic->irq_line < 16) pic_unmask(nic->irq_line);
}

void virtio_net_handle_irq(struct nic_device* nic) {
    if (!nic || !virtio_ready) return;
    (void)inb(virtio_io + VIRTIO_PCI_ISR);
    if (nic->irq_line) pic_eoi(nic->irq_line);
}

bool virtio_net_has_packet(struct nic_device* nic) {
    if (!nic || !virtio_ready) return false;
    return rx_vq.last_used_idx != rx_vq.mem->used_idx;
}

int virtio_net_receive_packet(struct nic_device* nic, void* buffer, size_t max_len) {
    if (!nic || !virtio_ready || !buffer || max_len == 0) return 0;
    if (rx_vq.last_used_idx == rx_vq.mem->used_idx) return 0;

    uint16_t slot = (uint16_t)(rx_vq.last_used_idx % rx_vq.size);
    uint16_t desc_id = (uint16_t)rx_vq.mem->used_ring[slot].id;
    uint32_t pkt_len = rx_vq.mem->used_ring[slot].len;
    rx_vq.last_used_idx++;

    if (pkt_len <= sizeof(struct virtio_net_hdr)) {
        virtq_free_desc(&rx_vq, desc_id);
        virtio_rx_refill(virtio_io);
        return 0;
    }
    size_t data_len = pkt_len - sizeof(struct virtio_net_hdr);
    if (data_len > max_len) data_len = max_len;
    const uint8_t* src = rx_bufs[desc_id] + sizeof(struct virtio_net_hdr);
    uint8_t* dst = (uint8_t*)buffer;
    for (size_t i = 0; i < data_len; i++) dst[i] = src[i];

    virtq_free_desc(&rx_vq, desc_id);
    virtio_rx_refill(virtio_io);
    return (int)data_len;
}

int virtio_net_send_packet(struct nic_device* nic, const void* data, size_t len) {
    if (!nic || !virtio_ready || !data || len == 0 || len + sizeof(struct virtio_net_hdr) > VIRTQ_BUF_SIZE) return -1;

    // Reclaim completed TX
    while (tx_vq.last_used_idx != tx_vq.mem->used_idx) {
        uint16_t slot = (uint16_t)(tx_vq.last_used_idx % tx_vq.size);
        uint16_t id = (uint16_t)tx_vq.mem->used_ring[slot].id;
        tx_vq.last_used_idx++;
        virtq_free_desc(&tx_vq, id);
    }

    int d = virtq_alloc_desc(&tx_vq);
    if (d < 0) return -1;

    struct virtio_net_hdr* hdr = (struct virtio_net_hdr*)tx_bufs[d];
    hdr->flags = 0; hdr->gso_type = 0; hdr->hdr_len = 0;
    hdr->gso_size = 0; hdr->csum_start = 0; hdr->csum_offset = 0;
    const uint8_t* src = (const uint8_t*)data;
    for (size_t i = 0; i < len; i++) tx_bufs[d][sizeof(struct virtio_net_hdr) + i] = src[i];

    tx_vq.mem->desc[d].addr = (uint64_t)(uintptr_t)tx_bufs[d];
    tx_vq.mem->desc[d].len = (uint32_t)(sizeof(struct virtio_net_hdr) + len);
    tx_vq.mem->desc[d].flags = 0;
    tx_vq.mem->desc[d].next = 0;

    uint16_t avail_idx = tx_vq.mem->avail_idx;
    tx_vq.mem->avail_ring[avail_idx % tx_vq.size] = (uint16_t)d;
    asm volatile("" ::: "memory");
    tx_vq.mem->avail_idx = (uint16_t)(avail_idx + 1);
    outw(virtio_io + VIRTIO_PCI_QUEUE_NOTIFY, VIRTQ_TX);
    return 0;
}
