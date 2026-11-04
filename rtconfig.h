#ifndef RT_CONFIG_H__
#define RT_CONFIG_H__

/* Project options */

#define PRJ_DEFCONFIG_FILENAME "6K9925_TPV_Eink_KeyDecode_NAND_defconfig"

/* -- Important: If following options have been changed, you need save & rerun menuconfig before changing any other options. */

#define PRJ_CHIP "d12p"
#define PRJ_BOARD "demo88-nor"
#define PRJ_KERNEL "rt-thread"
#define PRJ_APP "helloworld"
#define PRJ_RV_TOOLCHAIN_V2_6_1
#define PRJ_TOOLCHAIN_VER "V2.6.1"
/* end of Project options */
#define PLATFORM_LUBANLITE
#define PKGS_DIR "tools/env/packages"

/* Application options */

/* Filesystem related */

#define AIC_USING_FS_IMAGE_0
#define AIC_USING_FS_IMAGE_TYPE_FATFS_FOR_0
#define AIC_FS_IMAGE_DIR_0 "rodata"
#define AIC_FS_IMAGE_NAME_0 "rodata.fatfs"
#define AIC_FATFS_AUTO_SIZE_FOR_0
#define AIC_FATFS_DEFAULT_VOLAB_0
#define AIC_USING_FS_IMAGE_1
#define AIC_USING_FS_IMAGE_TYPE_FATFS_FOR_1
#define AIC_FS_IMAGE_DIR_1 "target/d12p/demo88-nor/pack/data"
#define AIC_FS_IMAGE_NAME_1 "data.fatfs"
#define AIC_FATFS_AUTO_SIZE_FOR_1
#define AIC_FATFS_DEFAULT_VOLAB_1
#define AIC_USING_FS_IMAGE_TYPE_FATFS_CLUSTER_SIZE 8
#define AIC_USING_DIRENT_CACHE_FOR_READ_ONLY_FATFS

/* LVGL demo select related */

#define LPKG_USING_LVGL
#define LVGL_V_9
#define LPKG_LVGL_PATH "/packages/multimedia/LVGL/LVGL"
#define LPKG_LVGL_THREAD_PRIO 20
#define LPKG_LVGL_THREAD_STACK_SIZE 49152
#define LPKG_LVGL_DISP_REFR_PERIOD 5
#define AIC_LVGL_DEMO
#define AIC_LVGL_UI_BUILDER
#define LV_COLOR_DEPTH 32
#define LV_CACHE_IMG_NUM 8
#define LV_CACHE_DEF_SIZE 0x200000
#define LV_IMAGE_HEADER_CACHE_DEF_CNT 20
#define MPP_JPEG_DEC_OUT_SIZE_LIMIT_ENABLE
#define MPP_JPEG_DEC_MAX_OUT_WIDTH 1024
#define MPP_JPEG_DEC_MAX_OUT_HEIGHT 600
#define LV_USE_CONF_CUSTOM
#define LV_USE_SPI_REPLACE_MIPI_DBI
#define AIC_LVGL_SPI_WIDGET
#define LV_SPI_SCREEN_BUS_WIDTH_1
#define LV_SPI_SCREEN_BUS_WIDTH_4
#define LV_SPI_ST77916
#define LV_SPI_BUS_WIDTH_4
#define LVGL_STORAGE_PATH "/rodata/lvgl_data"
/* end of Application options */

/* Chip options */

#define SOC_THEAD_SMART
#define PRJ_CUSTOM_LDS ""
#define AIC_CHIP_D12P
#define CACHE_LINE_SIZE 32
#define CPU_DCACHE_PREFETCH_EN
#define CPU_BASE 0x20000000
#define CPU_PSRAM_BASE 0x40000000
#define CPU_SRAM_BASE 0x30040000
#define AIC_CMU_DRV
#define AIC_CMU_DRV_V32
#define AIC_CMU_FRAMEWORK_V2
#define AIC_CMU_DRV_VER "32"
#define AIC_GPIO_DRV
#define AIC_GPIO_DRV_V12
#define AIC_GPIO_DRV_VER "12"
#define AIC_SYSCFG_DRV
#define AIC_SYSCFG_DRV_V16
#define AIC_SYSCFG_DRV_VER "16"
#define AIC_DMA_DRV
#define AIC_DMA_DRV_V30
#define AIC_DMA_DRV_VER "30"
#define AIC_DMA_CH_NUM 8
#define AIC_DMA_ALIGN_SIZE 4
#define AIC_UART_DRV
#define AIC_UART_DRV_V14
#define AIC_UART_DRV_VER "14"
#define AIC_UART_DEV_NUM 4
#define AIC_I2C_DRV
#define AIC_I2C_DRV_V10
#define AIC_I2C_DRV_VER "10"
#define AIC_I2C_CH_NUM 2
#define AIC_QSPI_DRV
#define AIC_QSPI_DRV_V34
#define AIC_QSPI_DRV_VER "34"
#define AIC_XSPI_DRV
#define AIC_XSPI_DRV_V11
#define AIC_USB_DEVICE_DRV
#define AIC_USB_DEVICE_DRV_V30
#define AIC_USB_DEVICE_DRV_VER "30"
#define AIC_USB_DEVICE_DEV_NUM 1
#define AIC_WRI_DRV
#define AIC_WRI_DRV_V16
#define AIC_WRI_DRV_VER "16"
#define AIC_RTC_DRV
#define AIC_RTC_DRV_V121
#define AIC_RTC_DRV_VER "121"
#define AIC_WDT_DRV
#define AIC_WDT_DRV_V10
#define AIC_WDT_DRV_VER "10"
#define AIC_SPIENC_DRV_V20
#define AIC_SPIENC_DRV_VER "20"
#define AIC_DE_DRV
#define AIC_DE_DRV_V11
#define AIC_DE_DRV_VER "11"
#define AIC_DISP_HW_RGB
#define AIC_DISP_HW_MIPI_DBI
#define AIC_DISP_HW_MIPI_DSI
#define AIC_DISP_RGB_DRV_V13
#define AIC_DISP_MIPI_DBI_DRV_V13
#define AIC_DISP_MIPI_DSI_DRV_V12
#define AIC_GE_DRV
#define AIC_GE_DRV_V11
#define AIC_GE_DRV_VER "11"
#define AIC_VE_DRV
#define AIC_VE_DRV_V31
#define AIC_VE_DRV_VER "31"
#define AIC_ADCIM_DRV
#define AIC_ADCIM_DRV_V14
#define AIC_ADCIM_DRV_VER "14"
#define AIC_TSEN_DRV
#define AIC_TSEN_DRV_V121
#define AIC_TSEN_DRV_VER "121"
#define AIC_TSEN_CH_NUM 1
#define AIC_SID_DRV
#define AIC_SID_DRV_V12
#define AIC_SID_DRV_VER "12"
#define AIC_AUDIO_DRV
#define AIC_AUDIO_DRV_V13
#define AIC_AUDIO_DRV_VER "13"
/* end of Chip options */

/* Board options */

/* Peripheral Devices: */

/* Interface Related: */

#define AIC_USING_UART0

/* UART0 Parameter */

#define AIC_CLK_UART0_FREQ 48000000
#define AIC_DEV_UART0_BAUDRATE 115200
#define AIC_DEV_UART0_DATABITS 8
#define AIC_DEV_UART0_STOPBITS 1
#define AIC_DEV_UART0_PARITY 0
#define AIC_DEV_UART0_RS232
#define AIC_DEV_UART0_MODE_RS232
#define AIC_DEV_UART0_MODE 0
#define AIC_UART0_FLAG 259
/* end of UART0 Parameter */
#define AIC_USING_I2C1

/* I2C1 Parameter */

#define AIC_DEV_I2C1_10BIT 0
#define AIC_DEV_I2C1_SPEED 400000
#define AIC_DEV_I2C1_SLAVE_MODE 0
#define AIC_DEV_I2C1_STUCK_TIME 400
/* end of I2C1 Parameter */
#define AIC_USING_USB0
#define AIC_USING_USB0_DEVICE

/* Using XPWM */

#define XPWM_FIFO_MAX 2
#define XPWM_FIFO_TH_DEFAULT_VALUE 3
/* end of Using XPWM */

/* Using QEP */

/* end of Using QEP */

/* Using EPWM */

/* end of Using EPWM */

/* Using CAP */

/* end of Using CAP */

/* Using HRTIMER */

/* end of Using HRTIMER */

/* Storage Related: */

#define AIC_USING_QSPI0

/* SPI0 Parameter */

#define AIC_DEV_QSPI0_MAX_SRC_FREQ_HZ 200000000
#define AIC_QSPI0_BUS_WIDTH_4
#define AIC_QSPI0_BUS_WIDTH 4
#define AIC_DEV_QSPI0_DELAY_MODE 2
/* end of SPI0 Parameter */
#define AIC_USING_QSPI1

/* SPI1 Parameter */

#define AIC_DEV_QSPI1_MAX_SRC_FREQ_HZ 200000000
#define AIC_QSPI1_BUS_WIDTH_4
#define AIC_QSPI1_BUS_WIDTH 4
#define AIC_DEV_QSPI1_DELAY_MODE 2
/* end of SPI1 Parameter */
#define AIC_USING_QSPI3

/* SPI3 Parameter */

#define AIC_DEV_QSPI3_MAX_SRC_FREQ_HZ 200000000
#define AIC_QSPI3_BUS_WIDTH_1
#define AIC_QSPI3_BUS_WIDTH 1
#define AIC_DEV_QSPI3_DELAY_MODE 2
#define AIC_QSPI3_DYNAMIC_DMA
#define AIC_QSPI3_BUS_SPI
/* end of SPI3 Parameter */
#define AIC_QSPI0_DEVICE_SPINAND
#define AIC_QSPI0_DEVICE_SPINAND_FREQ 50000000
#define AIC_SPINAND_BLOCK_CACHE
#define AIC_SPINAND_BLOCK_CACHE_SIZE 512
#define AIC_SPINAND_BLOCK_CACHE_NUM 4

/* WLAN Related: */


/* Analog Related: */

#define AIC_USING_TSEN

/* TSENS options */

#define AIC_USING_TSEN_CPU

/* TSEN_CPU Parameter */

#define AIC_TSEN0_SOFT_MODE_SINGLE_INTER
#define AIC_TSEN0_SOFT_MODE 0
#define AIC_TSEN0_MODE 0
/* end of TSEN_CPU Parameter */
/* end of TSENS options */

/* MutiMedia Related: */

#define AIC_USING_DE

/* Display Parameter */

#define AIC_DISPLAY_DRV
#define AIC_DISP_DE_DRV
#define AIC_DISP_MIPI_DBI
#define AIC_DI_TYPE 4
#define AICFB_RGB565
#define AICFB_FORMAT 0x0e
#define AICFB_ROTATE_0
#define AIC_FB_ROTATE_DEGREE 0

/* Display Panels */

#define AIC_PANEL_DBI_ZC_T1D8BP
#define AIC_EPD_P118F04
/* end of Display Panels */
#define AIC_GPIO_BACKLIGHT
#define AIC_PANEL_ENABLE_GPIO "PF.0"
/* end of Display Parameter */
#define AIC_USING_GE

/* Graphics Engine Parameter */

#define AIC_GE_CMDQ
#define AIC_GE_DITHER
#define AIC_GE_CMDQ_BUF_LENGTH 2048
/* end of Graphics Engine Parameter */
#define AIC_USING_VE

/* System Related: */

#define AIC_USING_DMA
#define AIC_USING_WRI
#define AIC_USING_RTC

/* RTC Parameter */

#define AIC_RTC_CLK_RATE 3276800
/* end of RTC Parameter */
#define AIC_USING_WDT

/* Mem Options */

/* SRAM parameter */

#define AIC_SRAM_SIZE 0x5000
#define AIC_SRAM_CMA_EN
#define AIC_SRAM_SW_SIZE 0x0
/* end of SRAM parameter */

/* PSRAM parameter */

#define AIC_PSRAM_SIZE 0x1000000
#define AIC_PSRAM_CMA_EN
#define AIC_PSRAM_SW_SIZE 0x200000
/* end of PSRAM parameter */
#define AIC_INTERRUPTSTACK_SIZE 4096

/* Support another Flash */

#define AIC_FLASH_NUM_ONE
/* end of Support another Flash */
/* end of Mem Options */

/* Clocks options */

#define AIC_CLK_PLL_INT0_FREQ 480000000
#define AIC_CLK_PLL_INT1_FREQ 1200000000
#define AIC_CLK_PLL_FRA0_FREQ 396000000
#define AIC_CLK_PLL_FRA1_FREQ 396000000
#define AIC_CLK_PLL_FRA2_FREQ 1188000000
#define AIC_CLK_CPU_FREQ 480000000
/* end of Clocks options */

/* Security Related: */

#define AIC_USING_SID

/* SID Parameter */

#define EFUSE_MAX_WORD 64
#define EFUSE_TIMING_VALUE 0x04021FF1
/* end of SID Parameter */
#define AIC_USING_SYSCFG

/* Syscfg Parameter */

#define AIC_SYSCFG_LDO1X_ENABLE
#define AIC_SYSCFG_LDO1X_VOL_VAL 6
/* end of Syscfg Parameter */
/* end of Board options */

/* Rt-Thread options */

#define KERNEL_RTTHREAD
#define DRIVER_HAL_EN
#define DRIVER_DRV_EN
#define RTT_DIR "kernel/rt-thread"

/* RT-Thread Kernel */

#define RT_NAME_MAX 8
#define RT_ALIGN_SIZE 4
#define RT_THREAD_PRIORITY_32
#define RT_THREAD_PRIORITY_MAX 32
#define RT_TICK_PER_SECOND 1000
#define RT_USING_OVERFLOW_CHECK
#define RT_USING_HOOK
#define RT_HOOK_USING_FUNC_PTR
#define RT_USING_IDLE_HOOK
#define RT_IDLE_HOOK_LIST_SIZE 4
#define IDLE_THREAD_STACK_SIZE 1024

/* kservice optimization */

/* end of kservice optimization */

/* Inter-Thread communication */

#define RT_USING_SEMAPHORE
#define RT_USING_MUTEX
#define RT_USING_EVENT
#define RT_USING_MAILBOX
#define RT_USING_MESSAGEQUEUE
/* end of Inter-Thread communication */

/* Memory Management */

#define RT_USING_MEMPOOL
#define RT_USING_MEMHEAP
#define RT_MEMHEAP_BSET_MODE
#define RT_USING_MEMHEAP_AS_HEAP
#define RT_USING_MEMTRACE
#define RT_USING_HEAP
/* end of Memory Management */

/* Kernel Device Object */

#define RT_USING_DEVICE
#define RT_USING_CONSOLE
#define RT_CONSOLEBUF_SIZE 128
#define RT_CONSOLE_DEVICE_NAME "uart0"
/* end of Kernel Device Object */
#define RT_VER_NUM 0x40101
/* end of RT-Thread Kernel */
#define ARCH_RISCV
#define ARCH_RISCV_FPU
#define ARCH_RISCV_FPU_S
#define ARCH_RISCV32

/* RT-Thread Components */

#define RT_USING_COMPONENTS_INIT
#define RT_USING_USER_MAIN
#define RT_MAIN_THREAD_STACK_SIZE 8192
#define RT_MAIN_THREAD_PRIORITY 10
#define RT_USING_LEGACY
#define RT_USING_MSH
#define RT_USING_FINSH
#define FINSH_USING_MSH
#define FINSH_THREAD_NAME "tshell"
#define FINSH_PROMPT_NAME "aic"
#define FINSH_THREAD_PRIORITY 20
#define FINSH_THREAD_STACK_SIZE 8192
#define FINSH_USING_HISTORY
#define FINSH_HISTORY_LINES 1
#define FINSH_USING_SYMTAB
#define FINSH_CMD_SIZE 80
#define MSH_USING_BUILT_IN_COMMANDS
#define FINSH_USING_DESCRIPTION
#define FINSH_ARG_MAX 10
#define RT_USING_DFS
#define DFS_USING_POSIX
#define DFS_USING_WORKDIR
#define DFS_FILESYSTEMS_MAX 8
#define DFS_FILESYSTEM_TYPES_MAX 4
#define DFS_FD_MAX 16
#define RT_USING_DFS_MNTTABLE
#define RT_USING_DFS_ELMFAT

/* elm-chan's FatFs, Generic FAT Filesystem Module */

#define RT_DFS_ELM_CODE_PAGE 437
#define RT_DFS_ELM_WORD_ACCESS
#define RT_DFS_ELM_USE_LFN_3
#define RT_DFS_ELM_USE_LFN 3
#define RT_DFS_ELM_LFN_UNICODE_2
#define RT_DFS_ELM_LFN_UNICODE 2
#define RT_DFS_ELM_USE_EXFAT
#define RT_DFS_ELM_MAX_LFN 255
#define RT_DFS_ELM_DRIVES 4
#define RT_DFS_ELM_MAX_SECTOR_SIZE 4096
#define RT_DFS_ELM_REENTRANT
#define RT_DFS_ELM_MUTEX_TIMEOUT 3000
/* end of elm-chan's FatFs, Generic FAT Filesystem Module */
#define RT_USING_DFS_ROMFS

/* Device Drivers */

#define RT_USING_DEVICE_IPC
#define RT_USING_SERIAL
#define RT_USING_SERIAL_V1
#define RT_SERIAL_USING_DMA
#define RT_SERIAL_RB_BUFSZ 64
#define RT_USING_I2C
#define RT_USING_I2C_BITOPS
#define RT_USING_PIN
#define RT_USING_ADC
#define RT_USING_MTD_NAND
#define RT_USING_RTC
#define RT_USING_ALARM
#define RT_USING_SDIO
#define RT_SDIO_STACK_SIZE 512
#define RT_SDIO_THREAD_PRIORITY 15
#define RT_MMCSD_STACK_SIZE 8192
#define RT_MMCSD_THREAD_PREORITY 22
#define RT_MMCSD_MAX_PARTITION 16
#define RT_USING_SPI
#define RT_USING_QSPI
#define RT_USING_WDT
#define RT_USING_AUDIO
#define RT_AUDIO_REPLAY_MP_BLOCK_SIZE 2048
#define RT_AUDIO_REPLAY_MP_BLOCK_COUNT 2
#define RT_AUDIO_RECORD_PIPE_SIZE 2048
#define RT_USING_SENSOR
#define RT_USING_SENSOR_CMD
#define RT_USING_TOUCH
#define RT_TOUCH_PIN_IRQ

/* Using USB */

/* end of Using USB */
/* end of Device Drivers */

/* C/C++ and POSIX layer */

#define RT_LIBC_DEFAULT_TIMEZONE 8

/* POSIX (Portable Operating System Interface) layer */

#define RT_USING_POSIX_FS
#define RT_USING_POSIX_DELAY
#define RT_USING_POSIX_CLOCK
#define RT_USING_PTHREADS
#define PTHREAD_NUM_MAX 8

/* Interprocess Communication (IPC) */


/* Socket is in the 'Network' category */

/* end of Interprocess Communication (IPC) */
/* end of POSIX (Portable Operating System Interface) layer */
#define RT_USING_CPLUSPLUS
/* end of C/C++ and POSIX layer */

/* Network */

/* end of Network */

/* Utilities */

#define RT_USING_ULOG
#define ULOG_OUTPUT_LVL_I
#define ULOG_OUTPUT_LVL 6
#define ULOG_ASSERT_ENABLE
#define ULOG_LINE_BUF_SIZE 128

/* log format */

#define ULOG_USING_COLOR
#define ULOG_OUTPUT_TIME
#define ULOG_TIME_USING_TIMESTAMP
#define ULOG_OUTPUT_LEVEL
#define ULOG_OUTPUT_TAG
/* end of log format */
#define ULOG_BACKEND_USING_CONSOLE
/* end of Utilities */
/* end of RT-Thread Components */

/* RT-Thread Utestcases */

/* end of RT-Thread Utestcases */
/* end of Rt-Thread options */

/* RT-Thread online packages */

/* IoT - internet of things */


/* Wi-Fi */

/* Marvell WiFi */

/* end of Marvell WiFi */

/* Wiced WiFi */

/* end of Wiced WiFi */
/* end of Wi-Fi */

/* IoT Cloud */

/* end of IoT Cloud */
/* end of IoT - internet of things */

/* security packages */

/* end of security packages */

/* language packages */

/* JSON: JavaScript Object Notation, a lightweight data-interchange format */

/* end of JSON: JavaScript Object Notation, a lightweight data-interchange format */

/* XML: Extensible Markup Language */

/* end of XML: Extensible Markup Language */
/* end of language packages */

/* multimedia packages */

/* u8g2: a monochrome graphic library */

/* end of u8g2: a monochrome graphic library */

/* PainterEngine: A cross-platform graphics application framework written in C language */

/* end of PainterEngine: A cross-platform graphics application framework written in C language */
/* end of multimedia packages */

/* tools packages */

/* end of tools packages */

/* system packages */

/* enhanced kernel services */

/* end of enhanced kernel services */

/* acceleration: Assembly language or algorithmic acceleration packages */

/* end of acceleration: Assembly language or algorithmic acceleration packages */

/* CMSIS: ARM Cortex-M Microcontroller Software Interface Standard */

/* end of CMSIS: ARM Cortex-M Microcontroller Software Interface Standard */

/* Micrium: Micrium software products porting for RT-Thread */

/* end of Micrium: Micrium software products porting for RT-Thread */
/* end of system packages */

/* peripheral libraries and drivers */


/* Kendryte SDK */

/* end of Kendryte SDK */
/* end of peripheral libraries and drivers */

/* AI packages */

/* end of AI packages */

/* miscellaneous packages */

/* project laboratory */

/* end of project laboratory */

/* samples: kernel and components samples */

/* end of samples: kernel and components samples */

/* entertainment: terminal games and other interesting software packages */

/* end of entertainment: terminal games and other interesting software packages */
/* end of miscellaneous packages */

/* Arduino libraries */


/* Projects */

/* end of Projects */

/* Sensors */

/* end of Sensors */

/* Display */

/* end of Display */

/* Timing */

/* end of Timing */

/* Data Processing */

/* end of Data Processing */

/* Data Storage */

/* Communication */

/* Device Control */

/* end of Device Control */

/* Other */

/* Signal IO */

/* end of Signal IO */

/* Uncategorized */

/* end of Arduino libraries */
/* end of RT-Thread online packages */

/* Local packages options */

/* Third-party packages options */

#define LPKG_USING_CPU_USAGE
#define LPKG_USING_CHERRYUSB_V1_5_0
#define LPKG_USING_CHERRYUSB
#define LPKG_CHERRYUSB_DEVICE
#define LPKG_CHERRYUSB_DEVICE_HS
#define LPKG_CHERRYUSB_DEVICE_AIC
#define LPKG_CHERRYUSB_DEVICE_AIC_DMA
#define LPKG_CHERRYUSB_DEVICE_COMPOSITE
#define LPKG_CHERRYUSB_DEVICE_COMPOSITE_TEMPLATE
#define LPKG_CHERRYUSB_DEVICE_COMPOSITE_SERIALNUMBER
#define LPKG_ENE_USB_SKU_COMBO
#define LPKG_ENE_USB_PID_B310

/* Composite Device function table1 */

#define LPKG_USING_COMP_VENDER_TABLE1
/* end of Composite Device function table1 */

/* Composite Device function table2 */

#define LPKG_USING_COMP_VENDER_TABLE2
#define LPKG_USING_COMP_DISP_TABLE2
/* end of Composite Device function table2 */
#define LPKG_CHERRYUSB_DYNAMIC_REGISTRATION_MODE
#define LPKG_CHERRYUSB_DEVICE_COMPOSITE_MULTIPLE_TABLES
#define LPKG_CHERRYUSB_DEVICE_DISPLAY
#define LPKG_CHERRYUSB_DEVICE_DISPLAY_TEMPLATE
#define LPKG_CHERRYUSB_USB_TRANSFER_JPEG
#define AIC_USB_DISP_MONITOR_NAME "USB Display"
#define AIC_USB_DISP_JPEG_QUALITY_LOWTHRESH 0
#define AIC_USB_DISP_INIT_DELAY_MS 0
#define AIC_USB_DISP_CALIBRATION_DEGREE 0
#define LPKG_CHERRYUSB_DEVICE_VENDER
#define LPKG_CHERRYUSB_DEVICE_VENDER_TEMPLATE

/* BenchMark Tests */

#define AIC_PRINT_FLOAT_CUSTOM
/* end of BenchMark Tests */
#define LPKG_USING_I2C_TOOLS
#define LPKG_I2C_TOOLS_PATH "/packages/peripherals/i2c-tools"
#define LPKG_USING_MHZ
#define LPKG_USING_FREETYPE
#define LPKG_USING_MINIMP3
#define LPKG_USING_FDTLIB
#define LPKG_USING_FDTLIB_CRC32_VERIFY
/* end of Third-party packages options */

/* ArtInChip packages options */

#define LPKG_MPP
#define AIC_MPP_PLAYER_INTERFACE
#define AIC_MPP_VE_TEST
#define AIC_MPP_AICP_DEC_ENABLE
#define LPKG_USING_ENV
#define AIC_ENV_INTERFACE
#define AIC_SYS_REDUNDAND_ENVIRONMENT
#define AIC_ENV_PART_NAME "env"
#define AIC_ENV_REDUNDAND_PART_NAME "env_r"
#define AIC_ENV_SIZE 4096
#define LPKG_USING_PINMUX_CHECHK
#define AIC_PINMUX_CHECK
#define LPKG_USING_AIC_SPIENC_AUTHORIZATION
#define AIC_USING_THREAD_STACK_TRACE
/* end of ArtInChip packages options */
/* end of Local packages options */

/* Drivers options */

#define AIC_GPIO_IRQ_DRV_EN
#define AIC_SPINAND_DRV

/* Peripheral */

#define LPKG_USING_SPINAND
#define SPI_NAND_WINBOND
#define AIC_NFTL_SUPPORT
#define AIC_NFLT_USED_LIB
#define AIC_NFTL_DISABLE_OOB_VERIFY
#define AIC_NFTL_OOB_VERIFY 0
#define AIC_NFTL_MINI_RESERVED_BLOCK 50
#define AIC_USING_TOUCH
#define AIC_USING_CTP
#define AIC_TOUCH_PANEL_GT911
#define AIC_TOUCH_PANEL_I2C_CHAN "i2c1"
#define AIC_TOUCH_PANEL_RST_PIN "PD.0"
#define AIC_TOUCH_PANEL_INT_PIN "PD.1"
#define AIC_TOUCH_X_COORDINATE_RANGE 1024
#define AIC_TOUCH_Y_COORDINATE_RANGE 600
#define AIC_SCREEN_REAL_X_RESOLUTION 1024
#define AIC_SCREEN_REAL_Y_RESOLUTION 600
#define AIC_SCREEN_RELATIVE_ROTATION_DEGREE 0
#define AIC_TOUCH_REPORT_X_COORDINATE 1024
#define AIC_TOUCH_REPORT_Y_COORDINATE 600
#define AIC_TOUCH_PANEL_NAME "gt911"

/* External Audio Codec Support */

#define AIC_I2S_CODEC_SELECT
#define AIC_I2S_CODEC_JY6311
#define AIC_I2S_CODEC_MCLK_NFS 256
#define AIC_I2S_CODEC_SCLK_NFS 64

/* JY6311 codec options */

#define AIC_I2S_CODEC_JY6311_I2C "i2c1"
/* end of JY6311 codec options */
#define AIC_I2S_CODEC_PA_PIN "PE.17"
#define AIC_I2S_CODEC_PA_ENABLE_HIGH
/* end of External Audio Codec Support */
/* end of Peripheral */

/* Drivers debug */

#define SHOW_PRINT_TIME
/* end of Drivers debug */

/* Drivers examples */

#define AIC_CMU_DRV_TEST
#define AIC_QSPI_LCD_INTERFACE
#define AIC_QSPI_LCD_ST77916
#define AIC_TP_DRV_TEST
#define AIC_WDT_DRV_TEST
/* end of Drivers examples */
/* end of Drivers options */

#endif
