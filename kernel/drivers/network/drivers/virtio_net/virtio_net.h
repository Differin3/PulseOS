#ifndef VIRTIO_NET_H
#define VIRTIO_NET_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

struct pci_device;
struct nic_device;

int find_virtio_net(struct pci_device* dev);
int virtio_net_init_with_pci(const struct pci_device* dev, struct nic_device* nic);
int virtio_net_send_packet(struct nic_device* nic, const void* data, size_t len);
int virtio_net_receive_packet(struct nic_device* nic, void* buffer, size_t max_len);
bool virtio_net_has_packet(struct nic_device* nic);
void virtio_net_handle_irq(struct nic_device* nic);
void virtio_net_enable_irq(struct nic_device* nic);

#endif
