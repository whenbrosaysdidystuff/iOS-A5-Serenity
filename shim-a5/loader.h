#pragma once
#include <stdint.h>
const char *loader_boot(uintptr_t payload, uint64_t payload_size, uint64_t dtb);
