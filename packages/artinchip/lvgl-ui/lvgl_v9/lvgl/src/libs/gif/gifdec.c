#include "gifdec.h"
#include "../../misc/lv_log.h"
#include "../../stdlib/lv_mem.h"
#include "../../misc/lv_color.h"
#if LV_USE_GIF

#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/stat.h>

#include <aic_core.h>

#define MIN(A, B) ((A) < (B) ? (A) : (B))
#define MAX(A, B) ((A) > (B) ? (A) : (B))

typedef struct Entry {
    uint16_t length;
    uint16_t prefix;
    uint8_t  suffix;
} Entry;

typedef struct Table {
    int bulk;
    int nentries;
    Entry * entries;
} Table;

#if LV_GIF_CACHE_DECODE_DATA
#define LZW_MAXBITS                 12
#define LZW_TABLE_SIZE              (1 << LZW_MAXBITS)
#define LZW_CACHE_SIZE              (LZW_TABLE_SIZE * 4)
#endif

static gd_GIF  * gif_open(gd_GIF * gif);
static bool f_gif_open(gd_GIF * gif, const void * path, bool is_file);
static void f_gif_read(gd_GIF * gif, void * buf, size_t len);
static int f_gif_seek(gd_GIF * gif, size_t pos, int k);
static void f_gif_close(gd_GIF * gif);
static void f_gif_rio_drop(gd_GIF * gif);
static void f_gif_rio_arm(void);
static void f_gif_rio_prefetch(gd_GIF * gif);
extern void custom_gif_decode_begin(void);
extern uint8_t custom_gif_decode_abort(void);
extern int custom_gif_io_prepare(void);
extern void custom_gif_io_done(void);

#if LV_USE_DRAW_SW_ASM == LV_DRAW_SW_ASM_HELIUM
    #include "gifdec_mve.h"
#endif

static uint16_t
read_num(gd_GIF * gif)
{
    uint8_t bytes[2];

    f_gif_read(gif, bytes, 2);
    return bytes[0] + (((uint16_t) bytes[1]) << 8);
}

gd_GIF *
gd_open_gif_file(const char * fname)
{
    gd_GIF gif_base;
    memset(&gif_base, 0, sizeof(gif_base));

    bool res = f_gif_open(&gif_base, fname, true);
    if(!res) return NULL;

    return gif_open(&gif_base);
}

gd_GIF *
gd_open_gif_data(const void * data)
{
    gd_GIF gif_base;
    memset(&gif_base, 0, sizeof(gif_base));

    bool res = f_gif_open(&gif_base, data, false);
    if(!res) return NULL;

    return gif_open(&gif_base);
}

static gd_GIF * gif_open(gd_GIF * gif_base)
{
    uint8_t sigver[3];
    uint16_t width, height, depth;
    uint8_t fdsz, bgidx, aspect;
    uint8_t * bgcolor;
    int gct_sz;
    gd_GIF * gif = NULL;

    /* Header */
    f_gif_read(gif_base, sigver, 3);
    if(memcmp(sigver, "GIF", 3) != 0) {
        LV_LOG_WARN("invalid signature\n");
        goto fail;
    }
    /* Version */
    f_gif_read(gif_base, sigver, 3);
    /* 87a has no graphic-control extension; the decoder already treats a
     * missing GCE as opaque and delay 0. Rejecting it dropped whole panels. */
    if(memcmp(sigver, "89a", 3) != 0 && memcmp(sigver, "87a", 3) != 0) {
        LV_LOG_WARN("invalid version\n");
        goto fail;
    }
    /* Width x Height */
    width  = read_num(gif_base);
    height = read_num(gif_base);
    /* FDSZ */
    f_gif_read(gif_base, &fdsz, 1);
    /* Presence of GCT */
    if(!(fdsz & 0x80)) {
        LV_LOG_WARN("no global color table\n");
        goto fail;
    }
    /* Color Space's Depth */
    depth = ((fdsz >> 4) & 7) + 1;
    /* Ignore Sort Flag. */
    /* GCT Size */
    gct_sz = 1 << ((fdsz & 0x07) + 1);
    /* Background Color Index */
    f_gif_read(gif_base, &bgidx, 1);
    /* Aspect Ratio */
    f_gif_read(gif_base, &aspect, 1);
    /* Create gd_GIF Structure. */
    {
        uint32_t pixels = (uint32_t)width * (uint32_t)height;
        uint32_t bytes;

        /* A failed alloc used to cache-clean a NULL pointer and never
         * returned. 720x480 is the largest frame this glass plays. */
        if(pixels == 0 || pixels > 720U * 480U)
            goto fail;
#if LV_GIF_CACHE_DECODE_DATA
        bytes = (uint32_t)sizeof(gd_GIF) + 5U * pixels + LZW_CACHE_SIZE;
#else
        bytes = (uint32_t)sizeof(gd_GIF) + 5U * pixels;
#endif
        gif = (gd_GIF *)aicos_malloc_try_cma(bytes);
        if(!gif) goto fail;
        aicos_dcache_clean_invalid_range((ulong *)gif,
                                         (ulong)ALIGN_UP(bytes, CACHE_LINE_SIZE));
    }
    memcpy(gif, gif_base, sizeof(gd_GIF));
    gif->width  = width;
    gif->height = height;
    gif->depth  = depth;
    /* Read GCT */
    gif->gct.size = gct_sz;
    f_gif_read(gif, gif->gct.colors, 3 * gif->gct.size);
    gif->palette = &gif->gct;
    gif->bgindex = bgidx;
    gif->canvas = (uint8_t *) &gif[1];
    gif->frame = &gif->canvas[4 * width * height];
    /* Always seed the index buffer. bgindex==0 used to leave garbage,
     * and the first gd_get_frame() dispose() would paint it onto canvas. */
    memset(gif->frame, gif->bgindex, (size_t)width * height);
    bgcolor = &gif->palette->colors[gif->bgindex * 3];
    #if LV_GIF_CACHE_DECODE_DATA
    gif->lzw_cache = gif->frame + width * height;
    #endif

#ifdef GIFDEC_FILL_BG
    GIFDEC_FILL_BG(gif->canvas, gif->width * gif->height, 1, gif->width * gif->height, bgcolor, 0xff);
#else
    for(int i = 0; i < gif->width * gif->height; i++) {
        gif->canvas[i * 4 + 0] = *(bgcolor + 2);
        gif->canvas[i * 4 + 1] = *(bgcolor + 1);
        gif->canvas[i * 4 + 2] = *(bgcolor + 0);
        gif->canvas[i * 4 + 3] = 0xff;
    }
#endif
    gif->anim_start = f_gif_seek(gif, 0, LV_FS_SEEK_CUR);
    gif->loop_count = -1;
    goto ok;
fail:
    f_gif_close(gif_base);
ok:
    return gif;
}

static void
discard_sub_blocks(gd_GIF * gif)
{
    uint8_t size;
    int guard = 0;

    do {
        /* A RAM GIF that has walked off its buffer reads 0x00 forever.
         * Cap the walk so the boot GIF returns and the NOR writer runs. */
        if (++guard > 4096)
            return;
        f_gif_read(gif, &size, 1);
        f_gif_seek(gif, size, LV_FS_SEEK_CUR);
    } while(size);
}

static void
read_plain_text_ext(gd_GIF * gif)
{
    if(gif->plain_text) {
        uint16_t tx, ty, tw, th;
        uint8_t cw, ch, fg, bg;
        size_t sub_block;
        f_gif_seek(gif, 1, LV_FS_SEEK_CUR); /* block size = 12 */
        tx = read_num(gif);
        ty = read_num(gif);
        tw = read_num(gif);
        th = read_num(gif);
        f_gif_read(gif, &cw, 1);
        f_gif_read(gif, &ch, 1);
        f_gif_read(gif, &fg, 1);
        f_gif_read(gif, &bg, 1);
        sub_block = f_gif_seek(gif, 0, LV_FS_SEEK_CUR);
        gif->plain_text(gif, tx, ty, tw, th, cw, ch, fg, bg);
        f_gif_seek(gif, sub_block, LV_FS_SEEK_SET);
    }
    else {
        /* Discard plain text metadata. */
        f_gif_seek(gif, 13, LV_FS_SEEK_CUR);
    }
    /* Discard plain text sub-blocks. */
    discard_sub_blocks(gif);
}

static void
read_graphic_control_ext(gd_GIF * gif)
{
    uint8_t rdit;

    /* Discard block size (always 0x04). */
    f_gif_seek(gif, 1, LV_FS_SEEK_CUR);
    f_gif_read(gif, &rdit, 1);
    gif->gce.disposal = (rdit >> 2) & 3;
    gif->gce.input = rdit & 2;
    gif->gce.transparency = rdit & 1;
    gif->gce.delay = read_num(gif);
    f_gif_read(gif, &gif->gce.tindex, 1);
    /* Skip block terminator. */
    f_gif_seek(gif, 1, LV_FS_SEEK_CUR);
}

static void
read_comment_ext(gd_GIF * gif)
{
    if(gif->comment) {
        size_t sub_block = f_gif_seek(gif, 0, LV_FS_SEEK_CUR);
        gif->comment(gif);
        f_gif_seek(gif, sub_block, LV_FS_SEEK_SET);
    }
    /* Discard comment sub-blocks. */
    discard_sub_blocks(gif);
}

static void
read_application_ext(gd_GIF * gif)
{
    char app_id[8];
    char app_auth_code[3];
    uint16_t loop_count;

    /* Discard block size (always 0x0B). */
    f_gif_seek(gif, 1, LV_FS_SEEK_CUR);
    /* Application Identifier. */
    f_gif_read(gif, app_id, 8);
    /* Application Authentication Code. */
    f_gif_read(gif, app_auth_code, 3);
    if(!strncmp(app_id, "NETSCAPE", sizeof(app_id))) {
        /* Discard block size (0x03) and constant byte (0x01). */
        f_gif_seek(gif, 2, LV_FS_SEEK_CUR);
        loop_count = read_num(gif);
        if(gif->loop_count < 0) {
            if(loop_count == 0) {
                gif->loop_count = 0;
            }
            else {
                gif->loop_count = loop_count + 1;
            }
        }
        /* Skip block terminator. */
        f_gif_seek(gif, 1, LV_FS_SEEK_CUR);
    }
    else if(gif->application) {
        size_t sub_block = f_gif_seek(gif, 0, LV_FS_SEEK_CUR);
        gif->application(gif, app_id, app_auth_code);
        f_gif_seek(gif, sub_block, LV_FS_SEEK_SET);
        discard_sub_blocks(gif);
    }
    else {
        discard_sub_blocks(gif);
    }
}

static void
read_ext(gd_GIF * gif)
{
    uint8_t label;

    f_gif_read(gif, &label, 1);
    switch(label) {
        case 0x01:
            read_plain_text_ext(gif);
            break;
        case 0xF9:
            read_graphic_control_ext(gif);
            break;
        case 0xFE:
            read_comment_ext(gif);
            break;
        case 0xFF:
            read_application_ext(gif);
            break;
        default:
            LV_LOG_WARN("unknown extension: %02X\n", label);
            discard_sub_blocks(gif);
    }
}

static uint16_t
get_key(gd_GIF *gif, int key_size, uint8_t *sub_len, uint8_t *shift, uint8_t *byte)
{
    int bits_read;
    int rpad;
    int frag_size;
    uint16_t key;

    key = 0;
    for (bits_read = 0; bits_read < key_size; bits_read += frag_size) {
        rpad = (*shift + bits_read) % 8;
        if (rpad == 0) {
            /* Update byte. */
            if (*sub_len == 0) {
                f_gif_read(gif, sub_len, 1); /* Must be nonzero! */
                if (*sub_len == 0) return 0x1000;
            }
            f_gif_read(gif, byte, 1);
            (*sub_len)--;
        }
        frag_size = MIN(key_size - bits_read, 8 - rpad);
        key |= ((uint16_t) ((*byte) >> rpad)) << bits_read;
    }
    /* Clear extra bits to the left. */
    key &= (1 << key_size) - 1;
    *shift = (*shift + key_size) % 8;
    return key;
}

#if LV_GIF_CACHE_DECODE_DATA
/* One sub-block in hand, bits packed in a register. get_key() was a call
 * plus a 1-byte read per code and held four 300x300 GIFs around 7 FPS. */
typedef struct {
    const uint8_t *p;
    const uint8_t *block;
    int left;
    int ram;
    gd_GIF *gif;
    uint32_t acc;
    int nbits;
    uint8_t file_buf[256];
} gif_br_t;

static inline __attribute__((always_inline)) int gif_br_byte(gif_br_t *br, uint8_t *out)
{
    if(br->left <= 0) {
        unsigned n;

        if(br->ram) {
            n = *br->p++;
            if(n == 0) return 0;
            br->block = br->p;
            br->p += n;
            br->left = (int)n;
        }
        else {
            uint8_t nb;

            f_gif_read(br->gif, &nb, 1);
            n = nb;
            if(n == 0) return 0;
            f_gif_read(br->gif, br->file_buf, n);
            br->block = br->file_buf;
            br->left = (int)n;
        }
    }
    *out = *br->block++;
    br->left--;
    return 1;
}

static inline __attribute__((always_inline)) uint16_t gif_br_bits(gif_br_t *br, int n)
{
    while(br->nbits < n) {
        uint8_t b;

        if(!gif_br_byte(br, &b)) return 0x1000;
        br->acc |= (uint32_t)b << br->nbits;
        br->nbits += 8;
    }
    {
        uint16_t k = (uint16_t)(br->acc & ((1u << n) - 1u));

        br->acc >>= n;
        br->nbits -= n;
        return k;
    }
}

static int
read_image_data(gd_GIF *gif, int interlace)
{
    uint8_t byte;
    int ret = 0;
    int key_size;
    int y, pass, linesize;
    uint8_t *ptr = NULL;
    uint8_t *ptr_row_start = NULL;
    uint8_t *ptr_base = NULL;
    uint16_t key, clear_code, stop_code, curr_code;
    int frm_off, frm_size,curr_size,top_slot,new_codes,slot;
    gif_br_t br;
    /* The first value of the value sequence corresponding to key */
    int first_value;
    int last_key;
    uint8_t *sp = NULL;
    uint8_t *p_stack = NULL;
    uint8_t *p_suffix = NULL;
    uint16_t *p_prefix = NULL;

    /* get initial key size and clear code, stop code */
    f_gif_read(gif, &byte, 1);
    key_size = (int) byte;
    clear_code = 1 << key_size;
    stop_code = clear_code + 1;
    key = 0;

    /* Do not walk the compressed frame once to find its end and again
     * to decode. That second NAND pass held a 350-wide file at 15 fps.
     * The tail after the stop code is drained below. */
    linesize = gif->width;
    ptr_base = &gif->frame[gif->fy * linesize + gif->fx];
    ptr_row_start = ptr_base;
    ptr = ptr_row_start;
    memset(&br, 0, sizeof(br));
    br.gif = gif;
    br.ram = !gif->is_file;
    if(br.ram) br.p = (const uint8_t *)gif->data + gif->f_rw_p;
    /* decoder */
    pass = 0;
    y = 0;
    p_stack = gif->lzw_cache;
    p_suffix = gif->lzw_cache + LZW_TABLE_SIZE;
    p_prefix = (uint16_t*)(gif->lzw_cache + LZW_TABLE_SIZE * 2);

    frm_off = 0;
    frm_size = gif->fw * gif->fh;
    curr_size = key_size + 1;
    top_slot = 1 << curr_size;
    new_codes = clear_code + 2;
    slot = new_codes;
    first_value = -1;
    last_key = -1;
    sp = p_stack;

    {
    int stuck_off = -1;
    int stuck = 0;

    while (frm_off < frm_size) {
        if (custom_gif_decode_abort()) {
            ret = -1;
            break;
        }
        /* Clear codes that never emit a pixel used to spin here. The
         * boot GIF then never returned and the next 2 MB chunk was not
         * written. */
        if (frm_off == stuck_off) {
            if (++stuck > 4096)
                break;
        } else {
            stuck = 0;
            stuck_off = frm_off;
        }
        /* copy data to frame buffer */
        while (sp > p_stack) {
            if (((frm_off & 255) == 0) && custom_gif_decode_abort()) {
                ret = -1;
                frm_off = frm_size;
                break;
            }
            *ptr++ = *(--sp);
            frm_off += 1;
            /* read one line */
            if ((ptr - ptr_row_start) == gif->fw) {
                if (interlace) {
                    switch(pass) {
                    case 0:
                    case 1:
                        y += 8;
                        ptr_row_start += linesize * 8;
                        break;
                    case 2:
                        y += 4;
                        ptr_row_start += linesize * 4;
                        break;
                    case 3:
                        y += 2;
                        ptr_row_start += linesize * 2;
                        break;
                    default:
                        break;
                    }
                    while (y >= gif->fh) {
                        if (pass >= 4)
                            break;
                        y  = 4 >> pass;
                        ptr_row_start = ptr_base + linesize * y;
                        pass++;
                    }
                } else {
                    ptr_row_start += linesize;
                }
                ptr = ptr_row_start;
            }
        }

        key = gif_br_bits(&br, curr_size);

        if (key == stop_code || key >= LZW_TABLE_SIZE)
            break;

        if (key == clear_code) {
            curr_size = key_size + 1;
            slot = new_codes;
            top_slot = 1 << curr_size;
            first_value = last_key = -1;
            sp = p_stack;
            continue;
        }

        curr_code = key;
        /*
         * If the current code is a code that will be added to the decoding
         * dictionary, it is composed of the data list corresponding to the
         * previous key and its first data.
         * */
        if (curr_code == slot && first_value >= 0) {
            *sp++ = first_value;
            curr_code = last_key;
        }else if(curr_code >= slot)
            break;

        {
            int hops = 0;

            while (curr_code >= new_codes) {
                uint16_t prev = (uint16_t)curr_code;

                /* A two-code cycle never equals prev and never returns.
                 * That held the LVGL thread, so the NOR writer never
                 * reached flash writing and the glass stayed on no present. */
                if ((unsigned)curr_code >= LZW_TABLE_SIZE ||
                    sp >= p_stack + LZW_TABLE_SIZE ||
                    ++hops > LZW_TABLE_SIZE ||
                    custom_gif_decode_abort()) {
                    ret = -1;
                    frm_off = frm_size;
                    break;
                }
                *sp++ = p_suffix[curr_code];
                curr_code = p_prefix[curr_code];
                if (curr_code == prev) {
                    ret = -1;
                    frm_off = frm_size;
                    break;
                }
            }
        }
        if ((unsigned)curr_code >= LZW_TABLE_SIZE ||
            sp >= p_stack + LZW_TABLE_SIZE)
            break;
        *sp++ = curr_code;

        /* Add code to decoding dictionary */
        if (slot < top_slot && last_key >= 0) {
            p_suffix[slot] = curr_code;
            p_prefix[slot++] = last_key;
        }
        first_value = curr_code;
        last_key = key;
        if (slot >= top_slot) {
            if (curr_size < LZW_MAXBITS) {
                top_slot <<= 1;
                curr_size += 1;
            }
        }
    }
    }

    /* RAM reader walks br.p and leaves f_rw_p at the LZW start.
     * File reader already consumed through the current sub-block. */
    if(!gif->is_file && br.p)
        gif->f_rw_p = (size_t)(br.p - (const uint8_t *)gif->data);
    /* A write aborts the frame. Draining the rest here used to sit in
     * hundreds of NAND reads and the glass stayed on hold GRAM. */
    if(ret != 0)
        return ret;
    {
        uint8_t sz = 1U;
        int guard = 0;

        while(sz) {
            if(++guard > 64 || custom_gif_decode_abort()) {
                ret = -1;
                break;
            }
            f_gif_read(gif, &sz, 1);
            if(sz)
                f_gif_seek(gif, sz, LV_FS_SEEK_CUR);
        }
    }
    if(ret == 0 && gif->is_file)
        f_gif_rio_prefetch(gif);
    return ret;
}
#else
static Table *
new_table(int key_size)
{
    int key;
    int init_bulk = MAX(1 << (key_size + 1), 0x100);
    Table * table = lv_malloc(sizeof(*table) + sizeof(Entry) * init_bulk);
    if(table) {
        table->bulk = init_bulk;
        table->nentries = (1 << key_size) + 2;
        table->entries = (Entry *) &table[1];
        for(key = 0; key < (1 << key_size); key++)
            table->entries[key] = (Entry) {
            1, 0xFFF, key
        };
    }
    return table;
}

/* Add table entry. Return value:
 *  0 on success
 *  +1 if key size must be incremented after this addition
 *  -1 if could not realloc table */
static int
add_entry(Table ** tablep, uint16_t length, uint16_t prefix, uint8_t suffix)
{
    Table * table = *tablep;
    if(table->nentries == table->bulk) {
        table->bulk *= 2;
        table = lv_realloc(table, sizeof(*table) + sizeof(Entry) * table->bulk);
        if(!table) return -1;
        table->entries = (Entry *) &table[1];
        *tablep = table;
    }
    table->entries[table->nentries] = (Entry) {
        length, prefix, suffix
    };
    table->nentries++;
    if((table->nentries & (table->nentries - 1)) == 0)
        return 1;
    return 0;
}

/* Compute output index of y-th input line, in frame of height h. */
static int
interlaced_line_index(int h, int y)
{
    int p; /* number of lines in current pass */

    p = (h - 1) / 8 + 1;
    if(y < p)  /* pass 1 */
        return y * 8;
    y -= p;
    p = (h - 5) / 8 + 1;
    if(y < p)  /* pass 2 */
        return y * 8 + 4;
    y -= p;
    p = (h - 3) / 4 + 1;
    if(y < p)  /* pass 3 */
        return y * 4 + 2;
    y -= p;
    /* pass 4 */
    return y * 2 + 1;
}

/* Decompress image pixels.
 * Return 0 on success or -1 on out-of-memory (w.r.t. LZW code table). */
static int
read_image_data(gd_GIF * gif, int interlace)
{
    uint8_t sub_len, shift, byte;
    int init_key_size, key_size, table_is_full = 0;
    int frm_off, frm_size, str_len = 0, i, p, x, y;
    uint16_t key, clear, stop;
    int ret;
    Table * table;
    Entry entry = {0};
    size_t start, end;

    f_gif_read(gif, &byte, 1);
    key_size = (int) byte;
    start = f_gif_seek(gif, 0, LV_FS_SEEK_CUR);
    discard_sub_blocks(gif);
    end = f_gif_seek(gif, 0, LV_FS_SEEK_CUR);
    f_gif_seek(gif, start, LV_FS_SEEK_SET);
    clear = 1 << key_size;
    stop = clear + 1;
    table = new_table(key_size);
    key_size++;
    init_key_size = key_size;
    sub_len = shift = 0;
    key = get_key(gif, key_size, &sub_len, &shift, &byte); /* clear code */
    frm_off = 0;
    ret = 0;
    frm_size = gif->fw * gif->fh;
    while(frm_off < frm_size) {
        if(key == clear) {
            key_size = init_key_size;
            table->nentries = (1 << (key_size - 1)) + 2;
            table_is_full = 0;
        }
        else if(!table_is_full) {
            ret = add_entry(&table, str_len + 1, key, entry.suffix);
            if(ret == -1) {
                lv_free(table);
                return -1;
            }
            if(table->nentries == 0x1000) {
                ret = 0;
                table_is_full = 1;
            }
        }
        key = get_key(gif, key_size, &sub_len, &shift, &byte);
        if(key == clear) continue;
        if(key == stop || key == 0x1000) break;
        if(ret == 1) key_size++;
        entry = table->entries[key];
        str_len = entry.length;
        for(i = 0; i < str_len; i++) {
            p = frm_off + entry.length - 1;
            x = p % gif->fw;
            y = p / gif->fw;
            if(interlace)
                y = interlaced_line_index((int) gif->fh, y);
            gif->frame[(gif->fy + y) * gif->width + gif->fx + x] = entry.suffix;
            if(entry.prefix == 0xFFF || i > 4096)
                break;
            else
                entry = table->entries[entry.prefix];
        }
        frm_off += str_len;
        if(key < table->nentries - 1 && !table_is_full)
            table->entries[table->nentries - 1].suffix = entry.suffix;
    }
    lv_free(table);
    if(key == stop) f_gif_read(gif, &sub_len, 1);  /* Must be zero! */
    f_gif_seek(gif, end, LV_FS_SEEK_SET);
    return 0;
}

#endif

/* Read image.
 * Return 0 on success or -1 on out-of-memory (w.r.t. LZW code table). */
static int
read_image(gd_GIF * gif)
{
    uint8_t fisrz;
    int interlace;

    /* A corrupted header must not walk fw*fh. 65535x65535 never returns. */
    if(gif->width == 0 || gif->height == 0 || gif->width > 720 || gif->height > 480)
        return -1;
    /* Image Descriptor. */
    gif->fx = read_num(gif);
    gif->fy = read_num(gif);
    gif->fw = read_num(gif);
    gif->fh = read_num(gif);
    if(gif->fx >= gif->width) gif->fx = 0;
    if(gif->fy >= gif->height) gif->fy = 0;
    if((uint32_t)gif->fx + gif->fw > gif->width)
        gif->fw = gif->width - gif->fx;
    if((uint32_t)gif->fy + gif->fh > gif->height)
        gif->fh = gif->height - gif->fy;
    f_gif_read(gif, &fisrz, 1);
    interlace = fisrz & 0x40;
    /* Ignore Sort Flag. */
    /* Local Color Table? */
    if(fisrz & 0x80) {
        /* Read LCT */
        gif->lct.size = 1 << ((fisrz & 0x07) + 1);
        f_gif_read(gif, gif->lct.colors, 3 * gif->lct.size);
        gif->palette = &gif->lct;
    }
    else
        gif->palette = &gif->gct;
    /* Image Data. */
    return read_image_data(gif, interlace);
}

static void
render_frame_rect(gd_GIF * gif, uint8_t * buffer)
{
    int i = gif->fy * gif->width + gif->fx;
#ifdef GIFDEC_RENDER_FRAME
    GIFDEC_RENDER_FRAME(&buffer[i * 4], gif->fw, gif->fh, gif->width,
                        &gif->frame[i], gif->palette->colors,
                        gif->gce.transparency ? gif->gce.tindex : 0x100);
#else
    int j, k;
    uint8_t index, * color;

    for(j = 0; j < gif->fh; j++) {
        for(k = 0; k < gif->fw; k++) {
            index = gif->frame[(gif->fy + j) * gif->width + gif->fx + k];
            color = &gif->palette->colors[index * 3];
            if(!gif->gce.transparency || index != gif->gce.tindex) {
                buffer[(i + k) * 4 + 0] = *(color + 2);
                buffer[(i + k) * 4 + 1] = *(color + 1);
                buffer[(i + k) * 4 + 2] = *(color + 0);
                buffer[(i + k) * 4 + 3] = 0xFF;
            }
        }
        i += gif->width;
    }
#endif
}

static void
dispose(gd_GIF * gif)
{
    int i;
    uint8_t * bgcolor;
    switch(gif->gce.disposal) {
        case 2: /* Restore to background color. */
            bgcolor = &gif->palette->colors[gif->bgindex * 3];

            uint8_t opa = 0xff;
            if(gif->gce.transparency) opa = 0x00;

            i = gif->fy * gif->width + gif->fx;
#ifdef GIFDEC_FILL_BG
            GIFDEC_FILL_BG(&(gif->canvas[i * 4]), gif->fw, gif->fh, gif->width, bgcolor, opa);
#else
            int j, k;
            for(j = 0; j < gif->fh; j++) {
                for(k = 0; k < gif->fw; k++) {
                    gif->canvas[(i + k) * 4 + 0] = *(bgcolor + 2);
                    gif->canvas[(i + k) * 4 + 1] = *(bgcolor + 1);
                    gif->canvas[(i + k) * 4 + 2] = *(bgcolor + 0);
                    gif->canvas[(i + k) * 4 + 3] = opa;
                }
                i += gif->width;
            }
#endif
            break;
        case 3: /* Restore to previous, i.e., don't update canvas.*/
            break;
        default:
            /* Already painted after the previous gd_render_frame. */
            if(gif->canvas_painted)
                break;
            render_frame_rect(gif, gif->canvas);
    }
}

/* Return 1 if got a frame; 0 if got GIF trailer; -1 if error. */
int
gd_get_frame(gd_GIF * gif)
{
    char sep;
    int sep_guard = 0;

    if(!gif || gif->width == 0 || gif->height == 0 ||
       gif->width > 720 || gif->height > 480)
        return -1;
    custom_gif_decode_begin();
    f_gif_rio_arm();
    dispose(gif);
    f_gif_read(gif, &sep, 1);
    /* NUL padding is legal. An endless run of it (read past a RAM GIF)
     * never returns, so the boot GIF stops and the file tail is never
     * written. A real frame has a handful of extensions, not thousands. */
    while(sep != ',') {
        if (custom_gif_decode_abort())
            return -1;
        if(++sep_guard > 2048)
            return -1;
        if(sep == ';') {
            f_gif_seek(gif, gif->anim_start, LV_FS_SEEK_SET);
            if(gif->loop_count == 1 || gif->loop_count < 0) {
                return 0;
            }
            else if(gif->loop_count > 1) {
                gif->loop_count--;
            }
        }
        else if(sep == '!')
            read_ext(gif);
        else if(sep == 0x00)
            ; /* some encoders pad with NUL between blocks */
        else return -1;
        f_gif_read(gif, &sep, 1);
    }
    if(read_image(gif) == -1)
        return -1;
    return 1;
}

void
gd_render_frame(gd_GIF * gif, uint8_t * buffer)
{
    render_frame_rect(gif, buffer);
    if(buffer == gif->canvas)
        gif->canvas_painted = 1;
}

void
gd_rewind(gd_GIF * gif)
{
    gif->canvas_painted = 0;
    gif->loop_count = -1;
    f_gif_seek(gif, gif->anim_start, LV_FS_SEEK_SET);
}

void
gd_close_gif(gd_GIF * gif)
{
    f_gif_close(gif);
    aicos_free(MEM_CMA, gif);
}

static uint32_t f_gif_stat_size(const char *lvpath)
{
    struct stat st;
    const char *p = lvpath;

    if(!p || !p[0])
        return 0;
    if(p[1] == ':')
        p += 2;
    if(stat(p, &st) != 0 || st.st_size <= 0)
        return 0;
    return (uint32_t)st.st_size;
}

static bool f_gif_piece_name(const gd_GIF *gif, int idx, char *name, size_t nname)
{
    size_t n = strlen(gif->src);

    if(n == 0 || n + 3 >= nname)
        return false;
    memcpy(name, gif->src, n);
    if(idx <= 0) {
        name[n] = '\0';
        return true;
    }
    name[n] = '.';
    name[n + 1] = (char)('1' + idx);
    name[n + 2] = '\0';
    return true;
}

static uint32_t f_gif_total(const gd_GIF *gif)
{
    uint32_t n = 0;
    uint8_t i;

    for(i = 0; i < gif->piece_n; i++)
        n += gif->piece[i];
    return n;
}

static int f_gif_locate(const gd_GIF *gif, uint32_t pos, uint32_t *local, uint32_t *left)
{
    uint32_t base = 0;
    uint8_t i;

    for(i = 0; i < gif->piece_n; i++) {
        uint32_t sz = gif->piece[i];

        if(pos < base + sz) {
            *local = pos - base;
            *left = sz - *local;
            return (int)i;
        }
        base += sz;
    }
    return -1;
}

/* lv_fs_open writes drv before open_cb. A failed open must not be closed. */
static void f_gif_open_fail(lv_fs_file_t *fd)
{
    fd->drv = NULL;
    fd->file_d = NULL;
    fd->cache = NULL;
}

static bool f_gif_select(gd_GIF *gif, int idx, uint32_t local)
{
    char name[80];

    if(idx < 0 || idx >= (int)gif->piece_n)
        return false;
    if(gif->piece_ram[idx]) {
        gif->piece_i = (uint8_t)idx;
        gif->piece_at = local;
        return true;
    }
    if(gif->piece_i != (uint8_t)idx || gif->fd.drv == NULL) {
        if(gif->fd.drv)
            lv_fs_close(&gif->fd);
        if(!f_gif_piece_name(gif, idx, name, sizeof(name)))
            return false;
        if(lv_fs_open(&gif->fd, name, LV_FS_MODE_RD) != LV_FS_RES_OK) {
            f_gif_open_fail(&gif->fd);
            gif->piece_i = 0xff;
            return false;
        }
        gif->piece_i = (uint8_t)idx;
        gif->piece_at = 0;
    }
    if(gif->piece_at != local) {
        if(lv_fs_seek(&gif->fd, local, LV_FS_SEEK_SET) != LV_FS_RES_OK)
            return false;
        gif->piece_at = local;
    }
    return true;
}

static bool f_gif_open(gd_GIF * gif, const void * path, bool is_file)
{
    f_gif_rio_drop(gif);
    gif->f_rw_p = 0;
    gif->data = NULL;
    gif->is_file = is_file;
    gif->piece_n = 0;
    gif->piece_i = 0xff;
    gif->piece_at = 0;
    gif->seg_pos = 0;
    gif->src[0] = '\0';

    if(is_file) {
        const char *name = path;
        size_t nlen;
        uint8_t i;
        lv_fs_res_t res;

        if(!name)
            return false;
        nlen = strlen(name);
        if(nlen == 0 || nlen >= sizeof(gif->src))
            return false;
        memcpy(gif->src, name, nlen + 1);
        res = lv_fs_open(&gif->fd, name, LV_FS_MODE_RD);
        if(res != LV_FS_RES_OK) {
            f_gif_open_fail(&gif->fd);
            return false;
        }
        gif->piece_i = 0;
        gif->piece_at = 0;
        /* Sizes come from stat. SEEK_END while the next piece was also
         * open never returned, so play stopped before the sync log. */
        gif->piece[0] = f_gif_stat_size(name);
        if(gif->piece[0] == 0)
            return true;
        gif->piece_n = 1;
        for(i = 1; i < 4U; i++) {
            char next[80];
            uint32_t sz;

            if(!f_gif_piece_name(gif, (int)i, next, sizeof(next)))
                break;
            sz = f_gif_stat_size(next);
            if(sz == 0)
                break;
            gif->piece[gif->piece_n++] = sz;
        }
        return true;
    }
    else {
        gif->data = path;
        return true;
    }
}

/* One bulk NAND read per 16 KB. LZW then copies from this buffer while
 * the previous panel DMA is still running. */
#define F_GIF_RIO 16384U
static uint8_t s_rio[F_GIF_RIO];
static gd_GIF *s_rio_gif;
static uint32_t s_rio_pos;
static uint32_t s_rio_len;
static uint8_t s_rio_fail;

static void f_gif_rio_drop(gd_GIF *gif)
{
    (void)gif;
    s_rio_gif = NULL;
    s_rio_len = 0U;
    s_rio_pos = 0U;
    s_rio_fail = 0U;
}

static void f_gif_rio_arm(void)
{
    s_rio_fail = 0U;
}

static void f_gif_rio_fill(gd_GIF *gif)
{
    uint32_t filled = 0U;
    uint32_t start;

    if(!gif || !gif->is_file) {
        f_gif_rio_drop(gif);
        return;
    }
    start = gif->seg_pos;
    /* Pieces already copied into RAM. No NAND, no shared DMA. */
    if(gif->piece_ram[0]) {
        while(filled < F_GIF_RIO) {
            uint32_t local = 0U;
            uint32_t left = 0U;
            uint32_t room;
            int idx = f_gif_locate(gif, gif->seg_pos, &local, &left);

            if(idx < 0 || left == 0U || !gif->piece_ram[idx])
                break;
            room = F_GIF_RIO - filled;
            if(room > left)
                room = left;
            memcpy(s_rio + filled, gif->piece_ram[idx] + local, room);
            filled += room;
            gif->seg_pos += room;
        }
        gif->seg_pos = start;
        s_rio_gif = gif;
        s_rio_pos = start;
        s_rio_len = filled;
        return;
    }
    /* Abort is not checked here. gd_open reads the header with the
     * previous frame's timer still running, and that used to return
     * no bytes so the saved GIF never opened. */
    if(s_rio_fail || custom_gif_io_prepare() != 0) {
        s_rio_fail = 1U;
        s_rio_gif = gif;
        s_rio_pos = start;
        s_rio_len = 0U;
        return;
    }
    while(filled < F_GIF_RIO) {
        uint32_t n = 0U;
        lv_fs_res_t res;

        if(gif->piece_n == 0) {
            res = lv_fs_read(&gif->fd, s_rio + filled, F_GIF_RIO - filled, &n);
            if(res != LV_FS_RES_OK || n == 0U)
                break;
            filled += n;
            gif->seg_pos += n;
            break;
        }
        {
            uint32_t local = 0U;
            uint32_t left = 0U;
            uint32_t room;
            int idx = f_gif_locate(gif, gif->seg_pos, &local, &left);

            if(idx < 0 || left == 0U)
                break;
            if(!f_gif_select(gif, idx, local))
                break;
            room = F_GIF_RIO - filled;
            if(room > left)
                room = left;
            res = lv_fs_read(&gif->fd, s_rio + filled, room, &n);
            if(res != LV_FS_RES_OK || n == 0U)
                break;
            filled += n;
            gif->seg_pos += n;
            gif->piece_at += n;
        }
    }
    custom_gif_io_done();
    gif->seg_pos = start;
    s_rio_gif = gif;
    s_rio_pos = start;
    s_rio_len = filled;
}

static void f_gif_rio_prefetch(gd_GIF *gif)
{
    uint32_t ahead;

    if(!gif || !gif->is_file || s_rio_fail || custom_gif_decode_abort())
        return;
    if(s_rio_gif == gif && gif->seg_pos >= s_rio_pos &&
       gif->seg_pos < s_rio_pos + s_rio_len) {
        ahead = s_rio_pos + s_rio_len - gif->seg_pos;
        if(ahead >= 4096U)
            return;
    }
    f_gif_rio_fill(gif);
}

static void f_gif_read(gd_GIF * gif, void * buf, size_t len)
{
    uint8_t *p = (uint8_t *)buf;

    if(!gif->is_file) {
        memcpy(buf, &gif->data[gif->f_rw_p], len);
        gif->f_rw_p += len;
        return;
    }
    while(len) {
        uint32_t off;
        uint32_t have;
        uint32_t n;

        if(s_rio_gif != gif || gif->seg_pos < s_rio_pos ||
           gif->seg_pos >= s_rio_pos + s_rio_len)
            f_gif_rio_fill(gif);
        if(s_rio_gif != gif || s_rio_len == 0U || gif->seg_pos < s_rio_pos ||
           gif->seg_pos >= s_rio_pos + s_rio_len)
            break;
        off = gif->seg_pos - s_rio_pos;
        have = s_rio_len - off;
        n = (uint32_t)len;
        if(n > have)
            n = have;
        memcpy(p, s_rio + off, n);
        p += n;
        len -= n;
        gif->seg_pos += n;
    }
}

static int f_gif_seek(gd_GIF * gif, size_t pos, int k)
{
    if(gif->is_file) {
        uint32_t next;
        uint32_t total;

        if(gif->piece_n == 0) {
            f_gif_rio_drop(gif);
            lv_fs_seek(&gif->fd, pos, k);
            {
                uint32_t x;
                lv_fs_tell(&gif->fd, &x);
                gif->seg_pos = x;
                return (int)x;
            }
        }
        total = f_gif_total(gif);
        if(k == LV_FS_SEEK_CUR)
            next = gif->seg_pos + (uint32_t)pos;
        else if(k == LV_FS_SEEK_END)
            next = total;
        else
            next = (uint32_t)pos;
        if(next > total)
            next = total;
        gif->seg_pos = next;
        return (int)next;
    }
    else {
        if(k == LV_FS_SEEK_CUR) gif->f_rw_p += pos;
        else if(k == LV_FS_SEEK_SET) gif->f_rw_p = pos;
        return gif->f_rw_p;
    }
}

static void f_gif_close(gd_GIF * gif)
{
    f_gif_rio_drop(gif);
    if(gif->is_file && gif->fd.drv)
        lv_fs_close(&gif->fd);
}

#endif /*LV_USE_GIF*/
