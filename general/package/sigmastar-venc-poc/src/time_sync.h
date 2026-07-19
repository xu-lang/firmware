#pragma once

#include <stdint.h>

int time_sync_addr(const char *addr);
int64_t time_sync_offset_us(void);
int time_sync_local_port(void);
int time_sync_make_request(void *buf, int len, uint64_t *t1_us);
int time_sync_process_response(const void *buf, int len, uint64_t t1_us, uint64_t t4_us);
