#include <rtconfig.h>

#if defined(AIC_PANEL_DBI_ZC_T1D8BP)
__asm__(
    ".section .rodata\n"
    ".global factory_boot_gif\n"
    ".global factory_boot_gif_end\n"
    ".align 4\n"
    "factory_boot_gif:\n"
    ".incbin \"target/d12p/demo88-nor/pack/data/Panel_0/Panel_000.gif\"\n"
    "factory_boot_gif_end:\n"
    ".section .text\n"
);
#endif
