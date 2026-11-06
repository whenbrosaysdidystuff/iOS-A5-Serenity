#pragma once
#include <stdint.h>
enum { PMIC_WDT_STOPPED, PMIC_WDT_NOT_RUNNING, PMIC_WDT_NOT_VERIFIED, PMIC_WDT_STILL_RUNNING };
int pmic_wdt_stop(uint32_t *ctrl);
