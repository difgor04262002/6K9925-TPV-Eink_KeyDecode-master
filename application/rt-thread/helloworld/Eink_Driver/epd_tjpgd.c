/*
 * LVGL already compiles TJpgDec, so this file stays empty.
 * Including tjpgd.c here defines jd_prepare/jd_decomp twice.
 */
#if 0
#ifdef AIC_EPD_P118F04
#define EPD_TJPGD_STANDALONE
#include "tjpgd.c"
#endif
#endif
