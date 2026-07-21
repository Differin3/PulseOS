# Makefile для Linux/WSL
# Использует обычный gcc вместо кросс-компилятора

ASM = nasm
CC = gcc
LD = ld

ASMFLAGS = -f elf32
CFLAGS = -m32 -ffreestanding -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -Wall -Wextra -fno-pic -fno-stack-protector -nostdlib -Ikernel
LDFLAGS = -m elf_i386 -T linker.ld -nostdlib

BOOT_SRC = boot/boot.asm
INTERRUPTS_SRC = boot/interrupts.asm
KERNEL_SRC = kernel/kernel.cpp
IDT_SRC = kernel/idt.cpp
KEYBOARD_SRC = kernel/drivers/input/keyboard.cpp
TERMINAL_SRC = kernel/drivers/video/terminal.cpp
ATA_SRC = kernel/drivers/storage/ata.cpp
AHCI_SRC = kernel/drivers/storage/ahci.cpp
NVME_SRC = kernel/drivers/storage/nvme.cpp
PCI_SRC = kernel/drivers/pci/pci.cpp
FS_SRC = kernel/fs.cpp
UTILS_SRC = kernel/utils.cpp
LS_SRC = kernel/utils/ls.cpp
FIND_SRC = kernel/utils/find.cpp
NANO_SRC = kernel/utils/nano.cpp
DISK_MANAGER_SRC = kernel/drivers/storage/disk_manager.cpp
MOUNT_SRC = kernel/mount.cpp
DEV_SRC = kernel/dev.cpp
DRIVER_MANAGER_SRC = kernel/driver_manager.cpp
SYSCALL_SRC = kernel/syscall.cpp
KERNEL_API_SRC = kernel/kernel_api.cpp
NIC_SRC = kernel/drivers/network/nic.cpp
RTL8139_SRC = kernel/drivers/network/drivers/rtl8139/rtl8139.cpp
PCNET_SRC = kernel/drivers/network/drivers/pcnet/pcnet.cpp
ETHERNET_SRC = kernel/drivers/network/protocols/ethernet.cpp
ARP_SRC = kernel/drivers/network/protocols/arp.cpp
IP_SRC = kernel/drivers/network/protocols/ip.cpp
UDP_SRC = kernel/drivers/network/protocols/udp.cpp
ICMP_SRC = kernel/drivers/network/protocols/icmp.cpp
TCP_SRC = kernel/drivers/network/protocols/tcp.cpp
TCP_CONNECTION_SRC = kernel/drivers/network/protocols/tcp_connection.cpp
DNS_SRC = kernel/drivers/network/dns/dns.cpp
DHCP_SRC = kernel/drivers/network/dhcp/dhcp.cpp
NETWORK_CONFIG_SRC = kernel/drivers/network/network_config.cpp
SOCKET_SRC = kernel/drivers/network/socket.cpp
HTTP_SERVER_SRC = kernel/drivers/network/http_server.cpp
HTTP_PROTOCOL_SRC = kernel/drivers/network/http_protocol.cpp
HTTP_GZIP_SRC = kernel/drivers/network/http_gzip.cpp
SERIAL_LOG_SRC = kernel/serial_log.cpp
KERNEL_OBJ = boot/boot.o boot/interrupts.o kernel/kernel.o kernel/idt.o kernel/serial_log.o kernel/drivers/input/keyboard.o kernel/drivers/video/terminal.o kernel/drivers/storage/ata.o kernel/drivers/storage/ahci.o kernel/drivers/storage/nvme.o kernel/drivers/pci/pci.o kernel/fs.o kernel/utils.o kernel/utils/ls.o kernel/utils/find.o kernel/utils/nano.o kernel/drivers/storage/disk_manager.o kernel/mount.o kernel/dev.o kernel/driver_manager.o kernel/syscall.o kernel/kernel_api.o kernel/drivers/network/nic.o kernel/drivers/network/socket.o kernel/drivers/network/http_protocol.o kernel/drivers/network/http_gzip.o kernel/drivers/network/http_server.o kernel/drivers/network/drivers/rtl8139/rtl8139.o kernel/drivers/network/drivers/pcnet/pcnet.o kernel/drivers/network/protocols/ethernet.o kernel/drivers/network/protocols/arp.o kernel/drivers/network/protocols/ip.o kernel/drivers/network/protocols/udp.o kernel/drivers/network/protocols/icmp.o kernel/drivers/network/protocols/tcp.o kernel/drivers/network/protocols/tcp_connection.o kernel/drivers/network/dns/dns.o kernel/drivers/network/dhcp/dhcp.o kernel/drivers/network/network_config.o
KERNEL_BIN = iso/boot/kernel.bin
ISO = myos.iso

all: $(ISO)

$(ISO): $(KERNEL_BIN)
	grub-mkrescue -o $(ISO) iso/

$(KERNEL_BIN): $(KERNEL_OBJ)
	$(LD) $(LDFLAGS) -o $(KERNEL_BIN) $(KERNEL_OBJ)

boot/boot.o: $(BOOT_SRC)
	$(ASM) $(ASMFLAGS) -o boot/boot.o $(BOOT_SRC)

boot/interrupts.o: $(INTERRUPTS_SRC)
	$(ASM) $(ASMFLAGS) -o boot/interrupts.o $(INTERRUPTS_SRC)

kernel/kernel.o: $(KERNEL_SRC) kernel/kernel.h
	$(CC) $(CFLAGS) -c -o kernel/kernel.o $(KERNEL_SRC)

kernel/idt.o: $(IDT_SRC) kernel/idt.h
	$(CC) $(CFLAGS) -c -o kernel/idt.o $(IDT_SRC)

kernel/serial_log.o: $(SERIAL_LOG_SRC) kernel/serial_log.h
	$(CC) $(CFLAGS) -c -o kernel/serial_log.o $(SERIAL_LOG_SRC)

kernel/drivers/input/keyboard.o: $(KEYBOARD_SRC) kernel/drivers/input/keyboard.h kernel/kernel.h
	$(CC) $(CFLAGS) -c -o kernel/drivers/input/keyboard.o $(KEYBOARD_SRC)

kernel/drivers/video/terminal.o: $(TERMINAL_SRC) kernel/drivers/video/terminal.h kernel/kernel.h
	$(CC) $(CFLAGS) -c -o kernel/drivers/video/terminal.o $(TERMINAL_SRC)

kernel/drivers/storage/ata.o: $(ATA_SRC) kernel/drivers/storage/ata.h
	$(CC) $(CFLAGS) -c -o kernel/drivers/storage/ata.o $(ATA_SRC)

kernel/drivers/storage/ahci.o: $(AHCI_SRC) kernel/drivers/storage/ahci.h kernel/drivers/storage/ata.h kernel/drivers/pci/pci.h
	$(CC) $(CFLAGS) -c -o kernel/drivers/storage/ahci.o $(AHCI_SRC)

kernel/drivers/storage/nvme.o: $(NVME_SRC) kernel/drivers/storage/nvme.h kernel/drivers/pci/pci.h
	$(CC) $(CFLAGS) -c -o kernel/drivers/storage/nvme.o $(NVME_SRC)

kernel/drivers/pci/pci.o: $(PCI_SRC) kernel/drivers/pci/pci.h
	$(CC) $(CFLAGS) -c -o kernel/drivers/pci/pci.o $(PCI_SRC)

kernel/fs.o: $(FS_SRC) kernel/fs.h kernel/drivers/storage/ata.h
	$(CC) $(CFLAGS) -c -o kernel/fs.o $(FS_SRC)

kernel/utils.o: $(UTILS_SRC) kernel/utils.h
	$(CC) $(CFLAGS) -c -o kernel/utils.o $(UTILS_SRC)

kernel/utils/ls.o: $(LS_SRC) kernel/utils.h kernel/fs.h kernel/drivers/video/terminal.h kernel/kernel.h
	$(CC) $(CFLAGS) -c -o kernel/utils/ls.o $(LS_SRC)

kernel/utils/find.o: $(FIND_SRC) kernel/utils.h kernel/fs.h kernel/drivers/video/terminal.h kernel/kernel.h
	$(CC) $(CFLAGS) -c -o kernel/utils/find.o $(FIND_SRC)

kernel/utils/nano.o: $(NANO_SRC) kernel/utils/nano.h kernel/fs.h kernel/drivers/video/terminal.h kernel/drivers/input/keyboard.h kernel/kernel.h
	$(CC) $(CFLAGS) -c -o kernel/utils/nano.o $(NANO_SRC)

kernel/drivers/storage/disk_manager.o: $(DISK_MANAGER_SRC) kernel/drivers/storage/disk_manager.h kernel/drivers/storage/ata.h kernel/drivers/storage/ahci.h kernel/drivers/storage/nvme.h
	$(CC) $(CFLAGS) -c -o kernel/drivers/storage/disk_manager.o $(DISK_MANAGER_SRC)

kernel/mount.o: $(MOUNT_SRC) kernel/mount.h kernel/drivers/storage/disk_manager.h kernel/fs.h
	$(CC) $(CFLAGS) -c -o kernel/mount.o $(MOUNT_SRC)

kernel/dev.o: $(DEV_SRC) kernel/dev.h kernel/fs.h kernel/drivers/storage/disk_manager.h
	$(CC) $(CFLAGS) -c -o kernel/dev.o $(DEV_SRC)

kernel/driver_manager.o: $(DRIVER_MANAGER_SRC) kernel/driver_manager.h kernel/drivers/pci/pci.h kernel/drivers/storage/ahci.h kernel/drivers/storage/nvme.h kernel/drivers/storage/ata.h kernel/drivers/input/keyboard.h kernel/drivers/video/terminal.h
	$(CC) $(CFLAGS) -c -o kernel/driver_manager.o $(DRIVER_MANAGER_SRC)

kernel/syscall.o: $(SYSCALL_SRC) kernel/syscall.h kernel/driver_manager.h kernel/fs.h
	$(CC) $(CFLAGS) -c -o kernel/syscall.o $(SYSCALL_SRC)

kernel/kernel_api.o: $(KERNEL_API_SRC) kernel/kernel_api.h kernel/driver_manager.h
	$(CC) $(CFLAGS) -c -o kernel/kernel_api.o $(KERNEL_API_SRC)

kernel/drivers/network/nic.o: $(NIC_SRC) kernel/drivers/network/nic.h kernel/drivers/pci/pci.h kernel/driver_manager.h kernel/drivers/network/drivers/rtl8139/rtl8139.h kernel/drivers/network/drivers/pcnet/pcnet.h
	$(CC) $(CFLAGS) -c -o kernel/drivers/network/nic.o $(NIC_SRC)

kernel/drivers/network/drivers/rtl8139/rtl8139.o: $(RTL8139_SRC) kernel/drivers/network/drivers/rtl8139/rtl8139.h kernel/drivers/network/nic.h kernel/drivers/pci/pci.h
	$(CC) $(CFLAGS) -c -o kernel/drivers/network/drivers/rtl8139/rtl8139.o $(RTL8139_SRC)

kernel/drivers/network/drivers/pcnet/pcnet.o: $(PCNET_SRC) kernel/drivers/network/drivers/pcnet/pcnet.h kernel/drivers/network/nic.h kernel/drivers/pci/pci.h
	$(CC) $(CFLAGS) -c -o kernel/drivers/network/drivers/pcnet/pcnet.o $(PCNET_SRC)

kernel/drivers/network/protocols/ethernet.o: $(ETHERNET_SRC) kernel/drivers/network/protocols/ethernet.h
	$(CC) $(CFLAGS) -c -o kernel/drivers/network/protocols/ethernet.o $(ETHERNET_SRC)

kernel/drivers/network/protocols/arp.o: $(ARP_SRC) kernel/drivers/network/protocols/arp.h kernel/drivers/network/protocols/ethernet.h kernel/drivers/network/nic.h
	$(CC) $(CFLAGS) -c -o kernel/drivers/network/protocols/arp.o $(ARP_SRC)

kernel/drivers/network/protocols/ip.o: $(IP_SRC) kernel/drivers/network/protocols/ip.h kernel/drivers/network/protocols/arp.h kernel/drivers/network/dhcp/dhcp.h
	$(CC) $(CFLAGS) -c -o kernel/drivers/network/protocols/ip.o $(IP_SRC)

kernel/drivers/network/protocols/udp.o: $(UDP_SRC) kernel/drivers/network/protocols/udp.h kernel/drivers/network/protocols/ip.h kernel/drivers/network/protocols/ethernet.h kernel/drivers/network/protocols/arp.h kernel/drivers/network/nic.h kernel/drivers/network/socket.h
	$(CC) $(CFLAGS) -c -o kernel/drivers/network/protocols/udp.o $(UDP_SRC)

kernel/drivers/network/protocols/icmp.o: $(ICMP_SRC) kernel/drivers/network/protocols/icmp.h kernel/drivers/network/protocols/ip.h kernel/drivers/network/protocols/ethernet.h kernel/drivers/network/protocols/arp.h kernel/drivers/network/protocols/tcp_connection.h kernel/drivers/network/nic.h
	$(CC) $(CFLAGS) -c -o kernel/drivers/network/protocols/icmp.o $(ICMP_SRC)

kernel/drivers/network/protocols/tcp.o: $(TCP_SRC) kernel/drivers/network/protocols/tcp.h kernel/drivers/network/protocols/tcp_connection.h kernel/drivers/network/protocols/ip.h kernel/drivers/network/protocols/ethernet.h kernel/drivers/network/protocols/arp.h kernel/drivers/network/nic.h
	$(CC) $(CFLAGS) -c -o kernel/drivers/network/protocols/tcp.o $(TCP_SRC)

kernel/drivers/network/protocols/tcp_connection.o: $(TCP_CONNECTION_SRC) kernel/drivers/network/protocols/tcp_connection.h kernel/drivers/network/protocols/tcp.h
	$(CC) $(CFLAGS) -c -o kernel/drivers/network/protocols/tcp_connection.o $(TCP_CONNECTION_SRC)

kernel/drivers/network/dns/dns.o: $(DNS_SRC) kernel/drivers/network/dns/dns.h kernel/drivers/network/protocols/udp.h kernel/drivers/network/protocols/ip.h kernel/drivers/network/protocols/tcp_connection.h kernel/serial_log.h
	$(CC) $(CFLAGS) -c -o kernel/drivers/network/dns/dns.o $(DNS_SRC)

kernel/drivers/network/dhcp/dhcp.o: $(DHCP_SRC) kernel/drivers/network/dhcp/dhcp.h kernel/drivers/network/protocols/udp.h kernel/drivers/network/protocols/ip.h kernel/drivers/network/nic.h kernel/drivers/network/protocols/ethernet.h kernel/drivers/network/protocols/arp.h kernel/drivers/network/dns/dns.h kernel/drivers/network/network_config.h
	$(CC) $(CFLAGS) -c -o kernel/drivers/network/dhcp/dhcp.o $(DHCP_SRC)

kernel/drivers/network/network_config.o: $(NETWORK_CONFIG_SRC) kernel/drivers/network/network_config.h kernel/drivers/network/dhcp/dhcp.h kernel/drivers/network/protocols/ip.h kernel/drivers/network/dns/dns.h kernel/fs.h
	$(CC) $(CFLAGS) -c -o kernel/drivers/network/network_config.o $(NETWORK_CONFIG_SRC)

kernel/drivers/network/socket.o: $(SOCKET_SRC) kernel/drivers/network/socket.h kernel/drivers/network/protocols/tcp.h kernel/drivers/network/protocols/udp.h kernel/drivers/network/nic.h
	$(CC) $(CFLAGS) -c -o kernel/drivers/network/socket.o $(SOCKET_SRC)

kernel/drivers/network/http_server.o: $(HTTP_SERVER_SRC) kernel/drivers/network/http_server.h kernel/drivers/network/http_protocol.h kernel/drivers/network/socket.h kernel/fs.h kernel/serial_log.h
	$(CC) $(CFLAGS) -c -o kernel/drivers/network/http_server.o $(HTTP_SERVER_SRC)

kernel/drivers/network/http_protocol.o: $(HTTP_PROTOCOL_SRC) kernel/drivers/network/http_protocol.h
	$(CC) $(CFLAGS) -c -o kernel/drivers/network/http_protocol.o $(HTTP_PROTOCOL_SRC)

kernel/drivers/network/http_gzip.o: $(HTTP_GZIP_SRC) kernel/drivers/network/http_gzip.h
	$(CC) $(CFLAGS) -c -o kernel/drivers/network/http_gzip.o $(HTTP_GZIP_SRC)

clean:
	rm -f $(KERNEL_OBJ) $(KERNEL_BIN) $(ISO)

.PHONY: all clean
