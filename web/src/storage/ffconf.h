// Project-local FatFs configuration (R0.16). One single-threaded USB volume,
// FAT12/16/32, UTF-8 long names, and variable sector sizes up to 4096 bytes.
#define FFCONF_DEF       80386
#define FF_FS_READONLY   0
#define FF_FS_MINIMIZE   0
#define FF_USE_FIND      0
#define FF_USE_MKFS      1  // native RAM-disk fixtures; not exposed to the page
#define FF_USE_FASTSEEK  0
#define FF_USE_EXPAND    0
#define FF_USE_CHMOD     0
#define FF_USE_LABEL     0
#define FF_USE_FORWARD   0
#define FF_USE_STRFUNC   0
#define FF_PRINT_LLI     0
#define FF_PRINT_FLOAT   0
#define FF_STRF_ENCODE   0

#define FF_CODE_PAGE     437
#define FF_USE_LFN       3
#define FF_MAX_LFN       255
#define FF_LFN_UNICODE   2
#define FF_LFN_BUF       255
#define FF_SFN_BUF       12
#define FF_FS_RPATH      0
#define FF_PATH_DEPTH    10

#define FF_VOLUMES       1
#define FF_STR_VOLUME_ID 0
#define FF_VOLUME_STRS   "USB"
#define FF_MULTI_PARTITION 1
#define FF_MIN_SS        512
#define FF_MAX_SS        4096
#define FF_LBA64         0
#define FF_MIN_GPT       0x10000000
#define FF_USE_TRIM      0

#define FF_FS_TINY       0
#define FF_FS_EXFAT      0
#define FF_FS_NORTC      1
#define FF_NORTC_MON     1
#define FF_NORTC_MDAY    1
#define FF_NORTC_YEAR    2026
#define FF_FS_CRTIME     0
#define FF_FS_NOFSINFO   0
#define FF_FS_LOCK       0
#define FF_FS_REENTRANT  0
#define FF_FS_TIMEOUT    1000
