# KnitOS

32-bit x86 hobby kernel (v0.2.5) with GRUB Multiboot2 boot, custom MOS filesystem, storage/network drivers, and an interactive shell.

## Requirements

Build and run on **Linux or WSL**:

- `nasm`
- `gcc` / `g++` with 32-bit multilib support (`gcc-multilib`, `g++-multilib` on Debian/Ubuntu)
- `binutils` (`ld`)
- `grub-pc-bin` or `grub2` with `grub-mkrescue`
- `xorriso` (for ISO creation)
- `qemu-system-i386` (for testing)

### Debian/Ubuntu

```bash
sudo apt install nasm gcc-multilib g++-multilib binutils grub-pc-bin xorriso qemu-system-x86
```

## Build

```bash
make
# or
make -f Makefile.wsl
```

On Windows, from the project folder:

```bat
build.bat
```

Output: `myos.iso` and `iso/boot/kernel.bin`.

## Testing (recommended)

После изменений в ядре, сети или HTTP — **автотест без ручного QEMU**:

```bat
tests\auto-test.bat
```

Без пересборки (если ISO уже свежий):

```bat
tests\auto-test.bat -SkipBuild
```

Скрипт сам: QEMU (**окно видно**) → serial COM → `autotest network` → curl → проверка логов.

Логи:

| Файл | Содержимое |
|------|------------|
| `logs/auto-test.log` | Весь вывод автотеста (шаги, curl, PASS/FAIL) |
| `logs/qemu-serial.log` | COM1 гостя (boot, `[INF][http]`, shell) |

```bat
view-auto-test-log.bat
view-log.bat
```

Скрыть окно QEMU (CI): `set MYOS_HEADLESS=1` перед запуском или `tests\auto-test.bat -Headless`

| Когда гонять | Зачем |
|--------------|--------|
| После `build.bat` и правок сети/HTTP | Регрессия Socket API, HTTP/1.1, DHCP |
| Перед коммитом крупных фич | Быстрый smoke без двух окон |
| После правок автотестов / serial | Проверка CI-цепочки |

Ручной режим (два окна, отладка в QEMU): `run-qemu-ahci.bat` + `tests\run-network-tests.bat`.  
Подробности: [docs/NETWORK_TESTS.md](docs/NETWORK_TESTS.md).

## Run in QEMU

**Windows (recommended):** after `build.bat`, start with logging:

```bat
run-qemu.bat
view-log.bat
```

For persistent disk I/O testing, use an explicit raw disk image (`myos.img`, created automatically if missing):

```bat
run-qemu-ahci.bat   REM ich9-ahci + SATA disk (default in modern QEMU)
run-qemu-ide.bat    REM legacy IDE primary master
```

After boot, verify storage: `disk`, `ls`, `write`, `cat`, `network save`, reboot and confirm FS data persists.

Serial output from the guest is saved to `logs/qemu-serial.log` in the project folder.

The log contains boot output (terminal mirror), shell commands (`[CMD] /> dhcp`), driver events (`[DRV]`), and network messages (`[INF][dhcp]`, …).

**Manual (WSL/Linux):**

```bash
qemu-system-i386 -cdrom myos.iso
```

With user networking (RTL8139, recommended for DHCP/ping/DNS):

```bash
mkdir -p logs
qemu-system-i386 -cdrom myos.iso -netdev user,id=net0 -device rtl8139,netdev=net0 -serial file:logs/qemu-serial.log
```

With AHCI disk (`myos.img`):

```bash
qemu-system-i386 -cdrom myos.iso \
  -drive file=myos.img,if=none,format=raw,id=disk0 \
  -device ich9-ahci,id=ahci0 -device ide-hd,drive=disk0,bus=ahci0.0 \
  -netdev user,id=net0 -device rtl8139,netdev=net0 -serial file:logs/qemu-serial.log
```

Legacy IDE:

```bash
qemu-system-i386 -cdrom myos.iso -drive file=myos.img,if=ide,index=0,media=disk \
  -netdev user,id=net0 -device rtl8139,netdev=net0 -serial file:logs/qemu-serial.log
```

### Debug logging in the guest shell

| Command | Effect |
|---------|--------|
| `log` | Show current log level and mirror status |
| `log mirror on` / `log mirror off` | VGA terminal → serial log (default: on) |
| `log err` | Structured logs: errors only |
| `log info` | Boot + drivers + network (less verbose) |
| `log debug` | Per-packet NIC details (default) |
| `log test` | Write test lines to serial log |
| `log off` | Disable structured `[INF]`/`[ERR]` logs (mirror stays on) |

Example `logs/qemu-serial.log`:

```
[INF][serial] mirror enabled -> logs/qemu-serial.log
[INF][boot] VGA initialized
[INF][boot] Driver manager initialized
[CMD] / > dhcp
[INF][dhcp] DISCOVER sent ...
[INF][dhcp] poll cmd=... isr=... capr=...
[INF][rtl8139] rx ok size=...
[INF][nic] eth type=0x0800 len=...
[INF][nic] udp dhcp sport=67 dport=68 len=...
[INF][dhcp] OFFER received ip=10.0.2.15
[INF][dhcp] ACK applied ip=10.0.2.15
```

### Typical network workflow in QEMU user net

QEMU defaults: guest `10.0.2.0/24`, DHCP offers `10.0.2.15`, gateway `10.0.2.2`, DNS `10.0.2.3`.

In the shell (`/ >`):

```
dhcp
network
ping 10.0.2.2
dns example.com
arp
netstat
route
```

UDP/TCP echo test (from host, while guest runs listen):

```
udplisten 9000
tcp listen 9000
socktest tcp 8080
socktest udp 9000
```

HTTP server (HTTP/1.1, Keep-Alive, static files from `/www`):

```
httpserver
httpserver 8080 16
```

From Windows host (guest IP usually `10.0.2.15` after `dhcp`):

```bat
build.bat
tests\auto-test.bat
```

Manual workflow (QEMU window + second cmd):

```bat
tests\run-network-tests.bat
```

See [docs/NETWORK_TESTS.md](docs/NETWORK_TESTS.md) for the full checklist.

```bat
curl http://10.0.2.15/
curl http://10.0.2.15:8080/
```

Manual config (without DHCP):

```
network static 10.0.2.15 10.0.2.2 10.0.2.3 255.255.255.0
ping 10.0.2.2
```

## Windows

**Важно:** команды `sudo apt ...` выполняются **внутри Ubuntu (WSL)**, а не в `cmd` Windows.

### Быстрая настройка (после `wsl --install -d Ubuntu`)

1. Откройте **Ubuntu** из меню Пуск (первый раз — создайте имя пользователя и пароль).
2. В папке проекта в **cmd**:

```bat
setup-wsl.bat
build.bat
```

`setup-wsl.bat` установит `nasm`, `gcc-multilib`, `grub-pc-bin` и т.д. внутри Ubuntu.

### Вручную

Войти в Ubuntu из cmd:

```bat
wsl -l -v
wsl -d Ubuntu
```

В появившейся Linux-консоли:

```bash
sudo apt update
sudo apt install -y nasm gcc-multilib g++-multilib binutils grub-pc-bin xorriso make
exit
```

Сборка из cmd:

```bat
build.bat
```

## Shell commands

| Command | Description |
|---------|-------------|
| `help` | List commands |
| `network` | Show MAC, IP, mask, gateway, DNS |
| `network static <ip> [gw] [dns] [mask]` | Manual network config |
| `dhcp` | Obtain IP via DHCP |
| `ip` / `ip <addr>` | Show or set IP |
| `ping <ip> [count]` | ICMP echo |
| `dns` / `dns <hostname>` | Show DNS server or resolve A record |
| `arp` | ARP table |
| `netstat` | TCP connections |
| `route` / `route <ip>` | Routing info / next-hop |
| `udp <ip> <port> <data>` | Send UDP |
| `udplisten <port>` | UDP echo server (30s) |
| `tcp <ip> <port> <data>` | TCP connect and send |
| `tcp listen <port>` | TCP echo server (one connection) |
| `socktest tcp\|udp <port>` | Socket API echo test |
| `httpserver [port] [max]` | HTTP/1.1 static server (`/www`, Keep-Alive) |
| `httpget <host> [path]` | HTTP/1.1 client (GET) |
| `log [off\|err\|info\|debug\|test]` | Serial debug log level |
| `ls`, `cd`, `cat`, `write`, `rm`, `disk`, … | Filesystem and system |

## Documentation

- [docs/DRIVER_MANAGER.md](docs/DRIVER_MANAGER.md)
- [docs/KERNEL_API.md](docs/KERNEL_API.md)
- [docs/SYSCALL_API.md](docs/SYSCALL_API.md)
- [docs/NETWORK_TESTS.md](docs/NETWORK_TESTS.md)
