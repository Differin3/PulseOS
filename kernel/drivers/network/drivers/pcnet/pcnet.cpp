#include "pcnet.h"
#include "../../nic.h"
#include "drivers/pci/pci.h"
#include "drivers/video/terminal.h"

// Простейший поллинг-драйвер AMD PCnet-FAST III (Am79C973)
// Реализует инициализацию, передачу и прием кадров Ethernet через кольцевые буферы.

// Порты PCnet относительно BAR0
#define PCNET_APROM        0x00  // 16 байт MAC в PROM
#define PCNET_RDP          0x10  // Register Data Port
#define PCNET_RAP          0x14  // Register Address Port
#define PCNET_RESET        0x18  // Reset
#define PCNET_BDP          0x1C  // Bus Data Port

// CSR регистры
#define CSR0               0
#define CSR1               1
#define CSR2               2
#define CSR3               3
#define CSR4               4
#define CSR58              58

// Биты CSR0
#define CSR0_INIT          0x0001
#define CSR0_STRT          0x0002
#define CSR0_STOP          0x0004
#define CSR0_TXON          0x0008
#define CSR0_RXON          0x0010
#define CSR0_IENA          0x0040

// Размеры колец
#define PCNET_RX_RING_SIZE 8
#define PCNET_TX_RING_SIZE 8

// Описатель кольца приёма/передачи (32-битный режим)
struct pcnet_ring_desc {
    uint32_t buf_addr;   // физический адрес буфера
    uint16_t buf_len;    // отрицательная длина буфера (двухкомплемент)
    uint16_t status;     // флаги и длина принятого/переданного кадра
    uint32_t misc;       // дополнительные поля (не используем)
} __attribute__((packed));

// Init block в 32-битном режиме
struct pcnet_init_block {
    uint16_t mode;
    uint8_t  rlen;   // log2(RX_RING_SIZE) << 4
    uint8_t  tlen;   // log2(TX_RING_SIZE) << 4
    uint8_t  phys_addr[6];
    uint16_t reserved;
    uint32_t filter[2];
    uint32_t rx_ring;
    uint32_t tx_ring;
} __attribute__((packed));

// Статическое состояние драйвера (один адаптер)
static uint16_t pcnet_io_base = 0;
static bool pcnet_initialized = false;

static struct pcnet_init_block pcnet_init_block_data __attribute__((aligned(16)));
static struct pcnet_ring_desc pcnet_rx_ring[PCNET_RX_RING_SIZE] __attribute__((aligned(16)));
static struct pcnet_ring_desc pcnet_tx_ring[PCNET_TX_RING_SIZE] __attribute__((aligned(16)));

static uint8_t pcnet_rx_buffers[PCNET_RX_RING_SIZE][ETH_MAX_PACKET_SIZE] __attribute__((aligned(16)));
static uint8_t pcnet_tx_buffers[PCNET_TX_RING_SIZE][ETH_MAX_PACKET_SIZE] __attribute__((aligned(16)));

static int pcnet_tx_index = 0;
static int pcnet_rx_index = 0;

// Порты ввода/вывода (локальные для драйвера PCnet)
static inline void outb(uint16_t port, uint8_t val) { asm volatile ("outb %0, %1" : : "a"(val), "Nd"(port)); }
static inline uint8_t inb(uint16_t port) { uint8_t ret; asm volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port)); return ret; }
static inline void outw(uint16_t port, uint16_t val) { asm volatile ("outw %0, %1" : : "a"(val), "Nd"(port)); }
static inline uint16_t inw(uint16_t port) { uint16_t ret; asm volatile ("inw %1, %0" : "=a"(ret) : "Nd"(port)); return ret; }

// Чтение/запись CSR через RAP/RDP
static inline void pcnet_write_csr(uint16_t reg, uint16_t val) {
    outw(pcnet_io_base + PCNET_RAP, reg);
    outw(pcnet_io_base + PCNET_RDP, val);
}

static inline uint16_t pcnet_read_csr(uint16_t reg) {
    outw(pcnet_io_base + PCNET_RAP, reg);
    return inw(pcnet_io_base + PCNET_RDP);
}

// Включение 32-битного режима (BCR18 = 1) — упрощенно
static void pcnet_enable_32bit_mode() {
    // BCR18 через RAP/RDP: RAP=18, RDP - данные
    outw(pcnet_io_base + PCNET_RAP, 18);
    uint16_t bcr18 = inw(pcnet_io_base + PCNET_RDP);
    bcr18 |= 0x0001; // bit0 = 1 → 32-bit mode
    outw(pcnet_io_base + PCNET_RDP, bcr18);
}

int pcnet_init_with_pci(const struct pci_device* dev, struct nic_device* nic) {
    if (!dev || !nic) return -1;
    if (pcnet_initialized) return 0;
    if (dev->vendor_id != 0x1022 || dev->device_id != 0x2000) return -1; // не PCnet

    // BAR0: I/O space
    if (dev->base_address[0] == 0 || !(dev->base_address[0] & 1)) {
        return -1;
    }
    pcnet_io_base = (uint16_t)(dev->base_address[0] & 0xFFFE);

    // Сброс устройства — чтение порта RESET
    (void)inw(pcnet_io_base + PCNET_RESET);

    // Включаем 32-битный режим
    pcnet_enable_32bit_mode();

    // Читаем MAC адрес из PROM (16 байт, первые 6 — MAC)
    for (int i = 0; i < 6; i++) {
        nic->mac_address[i] = inb(pcnet_io_base + PCNET_APROM + i);
    }

    // Заполняем init block
    for (int i = 0; i < 6; i++) {
        pcnet_init_block_data.phys_addr[i] = nic->mac_address[i];
    }
    pcnet_init_block_data.mode = 0x0000; // normal mode
    pcnet_init_block_data.rlen = (3 << 4); // 2^3 = 8 RX descriptors
    pcnet_init_block_data.tlen = (3 << 4); // 2^3 = 8 TX descriptors
    pcnet_init_block_data.filter[0] = 0;
    pcnet_init_block_data.filter[1] = 0;

    // Инициализируем RX кольцо
    for (int i = 0; i < PCNET_RX_RING_SIZE; i++) {
        pcnet_rx_ring[i].buf_addr = (uint32_t)(uintptr_t)pcnet_rx_buffers[i];
        pcnet_rx_ring[i].buf_len = (uint16_t)(-(int)ETH_MAX_PACKET_SIZE);
        pcnet_rx_ring[i].status = 0x8000; // owned by card
        pcnet_rx_ring[i].misc = 0;
    }

    // Инициализируем TX кольцо
    for (int i = 0; i < PCNET_TX_RING_SIZE; i++) {
        pcnet_tx_ring[i].buf_addr = (uint32_t)(uintptr_t)pcnet_tx_buffers[i];
        pcnet_tx_ring[i].buf_len = 0;
        pcnet_tx_ring[i].status = 0x0000; // owned by CPU, пусто
        pcnet_tx_ring[i].misc = 0;
    }

    pcnet_init_block_data.rx_ring = (uint32_t)(uintptr_t)pcnet_rx_ring;
    pcnet_init_block_data.tx_ring = (uint32_t)(uintptr_t)pcnet_tx_ring;

    // Указываем init block в CSR1/CSR2
    uint32_t init_block_addr = (uint32_t)(uintptr_t)&pcnet_init_block_data;
    pcnet_write_csr(CSR1, (uint16_t)(init_block_addr & 0xFFFF));
    pcnet_write_csr(CSR2, (uint16_t)(init_block_addr >> 16));

    // Разрешаем прерывания и запускаем INIT
    pcnet_write_csr(CSR3, 0x0000); // маска прерываний (минимальная)

    pcnet_write_csr(CSR0, CSR0_STOP);
    pcnet_write_csr(CSR0, CSR0_INIT | CSR0_IENA);

    // Ждем окончания INIT (CSR0 bit8 IDON)
    for (int i = 0; i < 100000; i++) {
        uint16_t csr0 = pcnet_read_csr(CSR0);
        if (csr0 & 0x0100) { // IDON
            break;
        }
    }

    // Старт контроллера (RX+TX)
    pcnet_write_csr(CSR0, CSR0_STRT | CSR0_IENA | CSR0_RXON | CSR0_TXON);

    // Обновляем структуру nic_device
    nic->io_base = pcnet_io_base;
    nic->mem_base = 0;
    nic->initialized = true;
    nic->active = true;
    nic->hw_type = NIC_HW_PCNET; // устанавливаем тип аппаратного NIC

    pcnet_initialized = true;
    return 0;
}

int pcnet_send_packet(struct nic_device* nic, const void* data, size_t len) {
    if (!pcnet_initialized || !nic || !nic->active) return -1;
    if (len > ETH_MAX_PACKET_SIZE) return -1;

    int idx = pcnet_tx_index;
    struct pcnet_ring_desc* desc = &pcnet_tx_ring[idx];

    // Ждём, пока дескриптор освободится (бит OWN=1 значит принадлежит карте)
    if (desc->status & 0x8000) {
        // Кольцо занято, пакет потерян
        return -1;
    }

    // Копируем данные в буфер
    uint8_t* buf = pcnet_tx_buffers[idx];
    const uint8_t* src = (const uint8_t*)data;
    for (size_t i = 0; i < len; i++) buf[i] = src[i];

    desc->buf_len = (uint16_t)(-(int)len);
    desc->misc = 0;
    desc->status = 0x8300; // OWN=1, STP|ENP

    // Уведомляем контроллер: запись в CSR0 с битом TDMD
    uint16_t csr0 = pcnet_read_csr(CSR0);
    pcnet_write_csr(CSR0, csr0 | 0x0008); // TDMD

    // Переходим к следующему дескриптору
    pcnet_tx_index = (pcnet_tx_index + 1) % PCNET_TX_RING_SIZE;
    return (int)len;
}

bool pcnet_has_packet(struct nic_device* nic) {
    (void)nic;
    if (!pcnet_initialized) return false;

    struct pcnet_ring_desc* desc = &pcnet_rx_ring[pcnet_rx_index];
    // OWN=0 → кадр принадлежит CPU
    return !(desc->status & 0x8000);
}

int pcnet_receive_packet(struct nic_device* nic, void* buffer, size_t max_len) {
    (void)nic;
    if (!pcnet_initialized) return 0;

    struct pcnet_ring_desc* desc = &pcnet_rx_ring[pcnet_rx_index];
    if (desc->status & 0x8000) {
        return 0; // нет принятого кадра
    }

    // В статусе длина кадра в младших 12 битах misc (упрощенно используем buf_len)
    uint16_t msg_len = (uint16_t)(-desc->buf_len);
    if (msg_len > max_len) msg_len = (uint16_t)max_len;

    uint8_t* dst = (uint8_t*)buffer;
    uint8_t* src = pcnet_rx_buffers[pcnet_rx_index];
    for (uint16_t i = 0; i < msg_len; i++) dst[i] = src[i];

    // Возвращаем дескриптор карте
    desc->buf_len = (uint16_t)(-(int)ETH_MAX_PACKET_SIZE);
    desc->status = 0x8000; // OWN=1

    pcnet_rx_index = (pcnet_rx_index + 1) % PCNET_RX_RING_SIZE;
    return msg_len;
}
