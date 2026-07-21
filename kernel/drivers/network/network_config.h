#ifndef NETWORK_CONFIG_H
#define NETWORK_CONFIG_H

#include <stdint.h>

#define NETWORK_CONFIG_PATH "/etc/network/interfaces"
#define NETWORK_RESOLV_PATH "/etc/resolv.conf"

// Применить конфиг при загрузке (читает файл или DHCP по умолчанию)
int network_config_apply_boot(void);

// Перечитать конфиг с диска и применить
int network_config_reload(void);

// Записать DHCP-конфиг и получить адрес (для autotest / сброс static)
int network_config_acquire_dhcp(void);

// Сохранить текущие настройки на диск
int network_config_save(void);

#endif
