#ifndef __NANOPI_NEO4_H
#define __NANOPI_NEO4_H

#include <configs/rk3399_common.h>

#pragma message "NanoPi NEO4"

#ifndef CONFIG_SPL_BUILD
#undef CONFIG_BOOTCOMMAND
#define CONFIG_BOOTCOMMAND \
	"bootrkp;" \
	"run distro_bootcmd;"
#endif

#define CONFIG_MMC_SDHCI_SDMA

#define CONFIG_BMP_16BPP
#define CONFIG_BMP_24BPP
#define CONFIG_BMP_32BPP

#endif
