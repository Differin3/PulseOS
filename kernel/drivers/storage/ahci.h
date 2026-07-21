#ifndef AHCI_H
#define AHCI_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

struct pci_device;

// AHCI host registers
#define AHCI_CAP 0x00
#define AHCI_GHC 0x04
#define AHCI_IS  0x08
#define AHCI_PI  0x0C

// Port registers (offsets from port base)
#define AHCI_PORT_LST_ADDR 0x00
#define AHCI_PORT_LST_ADDR_UPPER 0x04
#define AHCI_PORT_FIS_ADDR 0x08
#define AHCI_PORT_FIS_ADDR_UPPER 0x0C
#define AHCI_PORT_IS 0x10
#define AHCI_PORT_IE 0x14
#define AHCI_PORT_CMD 0x18
#define AHCI_PORT_TFD 0x20
#define AHCI_PORT_SIG 0x24
#define AHCI_PORT_SSTS 0x28
#define AHCI_PORT_SCTL 0x2C
#define AHCI_PORT_SERR 0x30
#define AHCI_PORT_SACT 0x34
#define AHCI_PORT_CI 0x38

#define AHCI_PORT_CMD_ST  (1u << 0)
#define AHCI_PORT_CMD_SUD (1u << 1)
#define AHCI_PORT_CMD_FRE (1u << 4)
#define AHCI_PORT_CMD_CR  (1u << 14)
#define AHCI_PORT_CMD_FR  (1u << 15)

#define AHCI_CAP_SSS      (1u << 27)
#define AHCI_TFD_BSY      0x80
#define AHCI_TFD_DRQ      0x08
#define AHCI_PxIS_TFES    (1u << 30)
#define AHCI_PxIS_HBFS    (1u << 29)
#define AHCI_PxIS_HBDS    (1u << 28)

#define SATA_SIG_ATA      0x00000101

#define FIS_TYPE_REG_H2D 0x27

#define ATA_CMD_READ_SECTORS     0x20
#define ATA_CMD_WRITE_SECTORS    0x30
#define ATA_CMD_READ_DMA_EXT     0x25
#define ATA_CMD_WRITE_DMA_EXT    0x35
#define ATA_CMD_IDENTIFY         0xEC

#define AHCI_MAX_PORTS 2
#define AHCI_CMD_SLOTS 32
#define AHCI_IO_MAX_WAIT 10000

struct ahci_port_state;

int ahci_init();
int ahci_init_with_device(const struct pci_device* dev);

int ahci_read_sector(uint32_t lba, void* buffer);
int ahci_write_sector(uint32_t lba, const void* buffer);
int ahci_read_sectors(uint32_t lba, size_t count, void* buffer);
int ahci_write_sectors(uint32_t lba, size_t count, const void* buffer);
uint32_t ahci_get_disk_size();

void ahci_select_port(struct ahci_port_state* port);
struct ahci_port_state* ahci_get_port(int index);
int ahci_port_count();
uint32_t ahci_port_disk_size(struct ahci_port_state* port);
uint8_t ahci_port_number(struct ahci_port_state* port);

#endif
