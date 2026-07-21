#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include <stdint.h>

void http_server_init(void);

// HTTP/1.1 сервер на Socket API (GET/HEAD/OPTIONS, Keep-Alive).
// accept_timeout_ms — таймаут ожидания каждого нового клиента.
// Возвращает число обработанных запросов или -1 при ошибке.
int http_server_run(uint16_t port, int max_requests, int accept_timeout_ms);

#endif
