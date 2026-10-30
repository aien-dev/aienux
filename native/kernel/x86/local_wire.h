#ifndef CK_LOCAL_WIRE_H
#define CK_LOCAL_WIRE_H
#include "../svc/courier.h"
#include <stdint.h>
void ck_local_wire_begin(uint32_t request);
int ck_local_wire_line(const char *line);
void ck_local_wire_step(const ck_courier *courier);
#endif
