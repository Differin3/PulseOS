# Сетевые тесты MyOS

## Полностью автоматический тест

```bat
build.bat
tests\auto-test.bat
```

Работает через **WSL bash** (обходит блокировку PowerShell execution policy в корпоративной Windows).

Опции:

```bat
tests\auto-test.bat -SkipBuild
```

WSL напрямую:

```bash
cd "/mnt/c/Users/.../my os c++"
bash tests/network/auto-network-test.sh
```

PowerShell-версия (если политика разрешает):

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests\network\AutoNetworkTest.ps1
```

Интерактивный COM в реальном времени:

```bat
run-qemu-test.bat
powershell -ExecutionPolicy Bypass -File tests\network\SerialConsole.ps1
```

---

Полный **ручной** прогон: сборка → QEMU → команды в госте → автотесты с хоста.

## Важно: порядок запуска (ручной режим)

`httpserver` **не завершается** после таймаута — ждёт клиентов, пока не обслужит `max` запросов.

1. В QEMU: `httpserver 8080 32` — shell **заблокирован**, это нормально
2. **Сразу** во втором окне: `tests\network\host-tests.bat`
3. Не ждите `Served 0 request(s)` — раньше сервер выходил по таймауту (исправлено)

Текст тестового файла: `write /www/test.txt hello-from-guest` (именно так).


```bat
build.bat
run-qemu-ahci.bat
```

В окне QEMU (guest):

```
dhcp
network
write /www/test.txt hello-from-guest
httpserver 8080 32
```

Во **втором** окне cmd (host):

```bat
tests\run-network-tests.bat
```

Или по отдельности:

```bat
tests\network\host-tests.bat
tests\network\host-tests.bat 10.0.2.15 8080
tests\network\check-serial-log.bat
powershell -File tests\network\tcp-echo-test.ps1 -Port 9001
```

Из WSL:

```bash
chmod +x tests/network/host-tests.sh
./tests/network/host-tests.sh 10.0.2.15 8080
```

## Файлы

| Файл | Назначение |
|------|------------|
| `tests/network/guest-commands.txt` | Команды для shell MyOS |
| `tests/network/host-tests.bat` | HTTP-тесты с Windows (curl) |
| `tests/network/host-tests.sh` | HTTP-тесты из WSL/Linux |
| `tests/network/tcp-echo-test.ps1` | TCP echo для `socktest` |
| `tests/network/check-serial-log.bat` | Проверка `logs/qemu-serial.log` |
| `tests/auto-test.bat` | **Полный автотест** (QEMU + serial + curl) |
| `tests/network/SerialConsole.ps1` | Интерактивный COM-терминал к гостю |
| `run-qemu-test.bat` | QEMU headless + serial TCP :4444 |
| `tests/run-network-tests.bat` | Мастер-скрипт (host + log) |

## Чеклист: фаза 1 (Socket API)

| # | Действие (guest) | Ожидание |
|---|------------------|----------|
| 1 | `dhcp` | IP `10.0.2.15`, gateway `10.0.2.2` |
| 2 | `network` | MAC, mask, DNS отображаются |
| 3 | `ping 10.0.2.2` | Replies from gateway |
| 4 | `socktest udp 9002` | В другом окне: UDP пакет echo |
| 5 | `socktest tcp 9001` | `tcp-echo-test.ps1 -Port 9001` → PASS |
| 6 | `netstat` | Видны TCP-соединения после тестов |

## Чеклист: фаза 2 (HTTP/1.1)

| # | Host-команда | Ожидание |
|---|--------------|----------|
| 1 | `curl -v http://10.0.2.15:8080/` | `HTTP/1.1 200`, тело с `MyOS` |
| 2 | `curl http://10.0.2.15:8080/test.txt` | `hello-from-guest` |
| 3 | `curl -I http://10.0.2.15:8080/` | `200`, `Content-Length`, `Server: MyOS-HTTP/1.1` |
| 4 | `curl -X OPTIONS -I http://10.0.2.15:8080/` | `204`, `Allow: GET, HEAD, OPTIONS` |
| 5 | `curl -o /dev/null -w "%{http_code}" http://10.0.2.15:8080/nope` | `404` |
| 6 | `curl --http1.1 http://10.0.2.15:8080/ http://10.0.2.15:8080/` | Оба запроса OK (Keep-Alive) |

## Чеклист: гостевой HTTP-клиент

| # | Команда (guest) | Ожидание |
|---|-----------------|----------|
| 1 | `httpget example.com` | HTML-ответ в терминале |
| 2 | `dns example.com` | IP-адрес |

## Serial log

После тестов в `logs/qemu-serial.log` должны быть строки:

```
[INF][dhcp] ...
[INF][http] listen ...
[INF][http] sent ...
```

Проверка: `tests\network\check-serial-log.bat`

## Устранение проблем

| Симптом | Решение |
|---------|---------|
| Host tests all fail | В госте выполнен `dhcp`? Запущен `httpserver`? |
| `curl: Connection refused` | Порт занят или сервер не слушает; попробуйте `httpserver 8080 32` |
| `/test.txt` SKIP | В госте: `write /www/test.txt hello-from-guest` |
| Нет `[INF][http]` в логе | `log info` в госте перед тестами |
| Другой IP | `host-tests.bat <ip> <port>` |

## Параметры QEMU

- Сеть: `user` netdev, RTL8139
- Guest IP по умолчанию: **10.0.2.15**
- Gateway: **10.0.2.2**
- DNS: **10.0.2.3**
