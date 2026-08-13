#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include <stdint.h>

void http_server_init(void);

// HTTP/1.1 сервер на Socket API (GET/HEAD/OPTIONS, Keep-Alive).
// accept_timeout_ms — таймаут ожидания каждого нового клиента.
// Возвращает число обработанных запросов или -1 при ошибке.
int http_server_run(uint16_t port, int max_requests, int accept_timeout_ms);

/* Spawn httpd kthread; returns pid (>=1) or <0. Wait with http_server_wait. */
int http_server_start(uint16_t port, int max_requests, int accept_timeout_ms);
int http_server_wait(int timeout_ms);          /* 0=done, 1=still running, -1=error */
int http_server_last_served(void);
int http_server_pid(void);
int http_server_ready(void);
void http_server_clear_state(void);

#endif
