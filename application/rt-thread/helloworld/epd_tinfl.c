/*
 * Inflate for the e-ink PNG path. miniz is not otherwise linked.
 */
#include <rtconfig.h>
#ifdef AIC_EPD_P118F04
#define MINIZ_NO_STDIO
#define MINIZ_NO_TIME
#define MINIZ_NO_MALLOC
#define MINIZ_NO_ARCHIVE_APIS
#define MINIZ_NO_ARCHIVE_WRITING_APIS
#define MINIZ_NO_ZLIB_APIS
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#define MINIZ_LITTLE_ENDIAN 1
#define MINIZ_HAS_64BIT_REGISTERS 0
#define MINIZ_USE_UNALIGNED_LOADS_AND_STORES 0
#include "miniz_tinfl.c"
#endif
