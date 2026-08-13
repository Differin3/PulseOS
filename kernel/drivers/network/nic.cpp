#include "nic.h"
#include "core/netif.h"
#include "core/net_rx.h"
#include "drivers/pci/pci.h"
#include "drivers/pic/pic.h"
#include "driver_manager.h"
#include "protocols/ip.h"
#include "protocols/udp.h"
#include "drivers/rtl8139/rtl8139.h"
#include "drivers/pcnet/pcnet.h"
#include "drivers/virtio_net/virtio_net.h"
#include "serial_log.h"
#include "sched/task.h"
#include <stddef.h>

static struct nic_device nic_dev = {};
static struct netif* nic_netif = 0;
static bool nic_initialized = false;

int nic_driver_read(void* device_data, void* buffer, size_t size, uint32_t offset);
int nic_driver_write(void* device_data, const void* buffer, size_t size, uint32_t offset);
int nic_driver_ioctl(void* device_data, uint32_t cmd, void* arg);

static int nic_ops_send(struct netif* nif, const void* data, size_t len) {
    (void)nif;
    if (!nic_initialized || !nic_dev.active) return -1;
    int rc = -1;
    if (nic_dev.hw_type == NIC_HW_VIRTIO) rc = virtio_net_send_packet(&nic_dev, data, len);
    else if (nic_dev.hw_type == NIC_HW_PCNET) rc = pcnet_send_packet(&nic_dev, data, len);
    else if (nic_dev.hw_type == NIC_HW_RTL8139) rc = rtl8139_send_packet(&nic_dev, data, len);
    // Drivers may return byte count; normalize to 0/-1 for netif_send
    return (rc < 0) ? -1 : 0;
}

static int nic_ops_poll(struct netif* nif) {
    if (!nif || !nic_initialized || !nic_dev.active) return 0;
    uint8_t packet_buffer[ETH_MAX_PACKET_SIZE];
    int count = 0;
    while (nic_has_packet() && count < 32) {
        int packet_len = nic_receive_packet(packet_buffer, sizeof(packet_buffer));
        if (packet_len <= 0) break;
        if (net_rx_enqueue(nif, packet_buffer, (size_t)packet_len) == 0) count++;
    }
    return count;
}

static void nic_ops_irq(struct netif* nif) {
    (void)nif;
    if (!nic_initialized) return;
    if (nic_dev.hw_type == NIC_HW_VIRTIO) virtio_net_handle_irq(&nic_dev);
    else if (nic_dev.hw_type == NIC_HW_RTL8139) rtl8139_handle_irq(&nic_dev);
    else if (nic_dev.irq_line) pic_eoi(nic_dev.irq_line);
    // Drain into queue from IRQ context (poll HW); one wake if any RX
    int n = nic_ops_poll(nif);
    if (n > 0) sched_wake_net();
}

extern "C" void nic_irq_handler_main(void) {
    if (nic_netif) netif_handle_irq(nic_netif);
    else if (nic_dev.irq_line) pic_eoi(nic_dev.irq_line);
}

static int nic_register_driver() {
    struct driver nic_driver;
    nic_driver.name[0] = 'n'; nic_driver.name[1] = 'i'; nic_driver.name[2] = 'c';
    nic_driver.name[3] = '0'; nic_driver.name[4] = 0;
    nic_driver.type = DRIVER_NETWORK;
    nic_driver.device_id = 0;
    nic_driver.device_data = &nic_dev;
    nic_driver.initialized = true;
    nic_driver.active = true;
    nic_driver.ops.init = 0;
    nic_driver.ops.read = nic_driver_read;
    nic_driver.ops.write = nic_driver_write;
    nic_driver.ops.ioctl = nic_driver_ioctl;
    nic_driver.ops.cleanup = 0;
    return driver_register(&nic_driver);
}

static int nic_attach_netif(const char* name) {
    nic_netif = netif_alloc(name);
    if (!nic_netif) return -1;
    for (int i = 0; i < 6; i++) nic_netif->mac[i] = nic_dev.mac_address[i];
    nic_netif->nic = &nic_dev;
    nic_netif->irq_line = nic_dev.irq_line;
    if (nic_dev.hw_type == NIC_HW_VIRTIO) nic_netif->hw_type = NETIF_HW_VIRTIO;
    else if (nic_dev.hw_type == NIC_HW_PCNET) nic_netif->hw_type = NETIF_HW_PCNET;
    else nic_netif->hw_type = NETIF_HW_RTL8139;
    nic_netif->ops.send = nic_ops_send;
    nic_netif->ops.poll = nic_ops_poll;
    nic_netif->ops.irq = nic_ops_irq;
    netif_register(nic_netif);
    netif_set_default(nic_netif);

    if (nic_dev.hw_type == NIC_HW_VIRTIO) virtio_net_enable_irq(&nic_dev);
    else if (nic_dev.hw_type == NIC_HW_RTL8139) rtl8139_enable_irq(&nic_dev);
    return 0;
}

static void nic_finish_init() {
    nic_initialized = true;
    if (nic_register_driver() != 0) {
        nic_initialized = false;
        return;
    }
    nic_attach_netif("eth0");
    net_stack_init();
}

int nic_driver_read(void* device_data, void* buffer, size_t size, uint32_t offset) {
    (void)device_data; (void)offset;
    return nic_receive_packet(buffer, size);
}
int nic_driver_write(void* device_data, const void* buffer, size_t size, uint32_t offset) {
    (void)device_data; (void)offset;
    return nic_send_packet(buffer, size);
}

int nic_driver_ioctl(void* device_data, uint32_t cmd, void* arg) {
    (void)device_data;
    if (cmd == NIC_IOCTL_GET_MAC && arg) { nic_get_mac((uint8_t*)arg); return 0; }
    if (cmd == NIC_IOCTL_SET_IP && arg) { ip_set_our_ip(*(uint32_t*)arg); return 0; }
    if (cmd == NIC_IOCTL_GET_IP && arg) { *(uint32_t*)arg = ip_get_our_ip(); return 0; }
    if (cmd == NIC_IOCTL_UDP_SEND && arg) {
        struct { uint32_t dest_ip; uint16_t src_port; uint16_t dest_port; void* data; size_t len; }* a =
            (typeof(a))arg;
        return udp_send(a->dest_ip, a->src_port, a->dest_port, a->data, a->len);
    }
    return -1;
}

int nic_init() {
    if (nic_initialized) return 0;
    netif_init_subsystem();
    struct pci_device dev;

    if (find_virtio_net(&dev) == 0) {
        if (virtio_net_init_with_pci(&dev, &nic_dev) == 0) {
            nic_finish_init();
            if (nic_initialized) return 0;
        }
    }
    if (find_rtl8139(&dev) == 0) {
        if (rtl8139_init_with_pci(&dev, &nic_dev) == 0) {
            nic_finish_init();
            if (nic_initialized) return 0;
        }
    }
    for (int bus = 0; bus < 16; bus++) {
        for (uint8_t device = 0; device < 32; device++) {
            for (uint8_t function = 0; function < 8; function++) {
                uint32_t vd = pci_read_config((uint8_t)bus, device, function, 0x00);
                uint16_t vendor_id = (uint16_t)(vd & 0xFFFF);
                uint16_t device_id = (uint16_t)(vd >> 16);
                if (vendor_id == 0x1022 && device_id == 0x2000) {
                    dev.vendor_id = vendor_id; dev.device_id = device_id;
                    dev.bus = (uint8_t)bus; dev.device = device; dev.function = function;
                    for (int i = 0; i < 6; i++) {
                        dev.base_address[i] = pci_read_config((uint8_t)bus, device, function, (uint8_t)(0x10 + i * 4));
                    }
                    if (pcnet_init_with_pci(&dev, &nic_dev) == 0) {
                        nic_finish_init();
                        if (nic_initialized) return 0;
                    }
                }
            }
        }
    }
    return -1;
}

int nic_init_with_device(const struct pci_device* dev) {
    if (nic_initialized) return 0;
    if (!dev) return -1;
    netif_init_subsystem();
    if (dev->vendor_id == 0x1AF4 && dev->device_id == 0x1000) {
        if (virtio_net_init_with_pci(dev, &nic_dev) == 0) {
            nic_finish_init();
            return nic_initialized ? 0 : -1;
        }
    }
    if (dev->vendor_id == 0x10EC && dev->device_id == 0x8139) {
        if (rtl8139_init_with_pci(dev, &nic_dev) == 0) {
            nic_finish_init();
            return nic_initialized ? 0 : -1;
        }
    }
    if (dev->vendor_id == 0x1022 && dev->device_id == 0x2000) {
        if (pcnet_init_with_pci(dev, &nic_dev) == 0) {
            nic_finish_init();
            return nic_initialized ? 0 : -1;
        }
    }
    return -1;
}

int nic_send_packet(const void* data, size_t len) {
    if (!nic_initialized || !nic_dev.active) {
        log_msg(LOG_ERR, "nic", "not initialized or not active");
        return -1;
    }
    if (len > ETH_MAX_PACKET_SIZE) {
        log_msg(LOG_ERR, "nic", "packet too large");
        return -1;
    }
    if (nic_netif) return netif_send(nic_netif, data, len);
    return nic_ops_send(0, data, len);
}

int nic_receive_packet(void* buffer, size_t max_len) {
    if (!nic_initialized || !nic_dev.active) return 0;
    if (nic_dev.hw_type == NIC_HW_VIRTIO) return virtio_net_receive_packet(&nic_dev, buffer, max_len);
    if (nic_dev.hw_type == NIC_HW_PCNET) return pcnet_receive_packet(&nic_dev, buffer, max_len);
    if (nic_dev.hw_type == NIC_HW_RTL8139) return rtl8139_receive_packet(&nic_dev, buffer, max_len);
    return 0;
}

void nic_get_mac(uint8_t* mac) {
    if (!mac) return;
    if (nic_netif) { netif_get_mac(nic_netif, mac); return; }
    for (int i = 0; i < 6; i++) mac[i] = nic_dev.mac_address[i];
}

bool nic_has_packet() {
    if (!nic_initialized || !nic_dev.active) return false;
    if (nic_dev.hw_type == NIC_HW_VIRTIO) return virtio_net_has_packet(&nic_dev);
    if (nic_dev.hw_type == NIC_HW_PCNET) return pcnet_has_packet(&nic_dev);
    if (nic_dev.hw_type == NIC_HW_RTL8139) return rtl8139_has_packet(&nic_dev);
    return false;
}

void nic_debug_rx_regs(uint8_t* cmd, uint16_t* isr, uint16_t* capr, uint16_t* cbr) {
    if (nic_dev.hw_type == NIC_HW_RTL8139) {
        rtl8139_rx_regs(&nic_dev, cmd, isr, capr, cbr);
        return;
    }
    if (cmd) *cmd = 0;
    if (isr) *isr = 0;
    if (capr) *capr = 0;
    if (cbr) *cbr = 0;
}

void nic_process_packets() {
    net_process();
}

struct nic_device* nic_get_device(void) {
    return nic_initialized ? &nic_dev : 0;
}
