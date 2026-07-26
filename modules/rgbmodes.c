/* quadcastrgb - set RGB lights of HyperX Quadcast S and DuoCast
 * File rgbmodes.c
 *
 * <----- License notice ----->
 * Copyright (C) 2022, 2023, 2024 Ors1mer
 *
 * You may contact the author by email:
 * ors1mer [[at]] ors1mer dot xyz
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 2 of the License ONLY.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see
 * <https://www.gnu.org/licenses/gpl-2.0.en.html>. For any questions
 * concerning the license, you can write to <licensing@fsf.org>.
 * Also, you may visit the Free Software Foundation at
 * 51 Franklin Street, Fifth Floor Boston, MA 02110 USA. 
 */
#include <stdio.h> /* for fprintf & fputs */
#include <stdlib.h> /* for srand & rand */
#include <time.h> /* for time */
#include <math.h> /* for cos & sin, gradient angle on Quadcast 2S */

#define QS2S_PI 3.14159265358979323846

#include "devio.h" /* for QUADCAST_2S_PID */

#include "rgbmodes.h"



static void get_mode_sizes(struct colschemes *cs, int *seq_upper,
                                                               int *seq_lower);
static int count_data(struct colscheme *colsch, int pid);
static int count_2s_data(struct colscheme *colsch);
static void fill_data(struct colscheme *colsch, byte_t *da, int pckcnt,
                                                                    int group);
static void fill_qs2s_data(struct colscheme *colsch, byte_t *da,
                                                        int pckcnt, int group);
static void equalize(int upper_size, int lower_size, datpack *da);
static void fillup_to(size_t copy_size, byte_t *curr, byte_t *finish);
static void set_brightness(int *color, int br);

/* Solid */
static void sequence_solid(const int *colors, byte_t *da);
static void sequence_solid_qs2s(const int *colors, byte_t *da, int group);
static void qs2s_write_group_color(byte_t *frame, int color, int group);
static void fill_qs2s_packets_with_color(byte_t *start, int clr, int offset,
                                                                      int cnt);
static void qs2s_animate(struct colscheme *colsch, byte_t *da, int frame_total,
                                                                     int group);
static int qs2s_led_column(int led);
static byte_t *qs2s_led_ptr(byte_t *frame, int led);
static void qs2s_write_wave_group(byte_t *frame, const byte_t *cmdbuf,
                          unsigned int rawsize, unsigned int frame_step,
                          int group, int angle_deg, int width_pct);
static void qs2s_debug_row(byte_t *frame, int row, int group);
static int qs2s_led_row(int led);
/* Blink */
static unsigned int count_blink_data(struct colscheme *colsch,
                                                       unsigned int *rawsize);
static void sequence_blink_random(int speed, int dly_seg, byte_t *da);
static void sequence_blink(const struct colscheme *colsch, byte_t *da,
                                                                   int pckcnt);
static void blink_segment_fill(int col, int col_seg, int dly_seg, byte_t **da);
static void color_fill(int color, int size, byte_t **da);
static int random_color();
/* Cycle */
static unsigned int count_cycle_data(const struct colscheme *colsch,
                                                       unsigned int *rawsize);
static int get_gradient_length(const int *color, int spd);
static void sequence_cycle(const int *color, const int *stops, int spd,
                                                                  byte_t *da);
static void write_gradient(byte_t **da, int start_col, int end_col,
                                                                   int length);
/* Wave */
static void sequence_wave(int *color, int spd, int group, byte_t *da);
static void wave_array_shift(int *color);
/* Lightning & Pulse */
static unsigned int count_lightning_data(struct colscheme *colsch,
                                                       unsigned int *rawsize);
static void sequence_lightning(const int *color, int spd, int group,
                                                  int synchronous, byte_t *da);
static int next_gradient_color(int color, int endcolor, unsigned int size);

/* Shared */
static void write_hexcolor(int color, byte_t *mem);
static unsigned int colarr_len(const int *arr);
static unsigned int sizeof_frames(int *color, unsigned int framesize);

#ifdef DEBUG
static void print_datpack(datpack *da, int pck_cnt);
#endif

datpack *parse_colorscheme(struct colschemes *cs, int *pck_cnt)
{
    datpack *data_arr = NULL;
    int seq_upper, seq_lower;

    set_brightness(cs->upper.colors, cs->upper.br);
    set_brightness(cs->lower.colors, cs->lower.br);

    get_mode_sizes(cs, &seq_upper, &seq_lower);
    *pck_cnt = seq_upper >= seq_lower ? seq_upper : seq_lower;
    data_arr = calloc(sizeof(datpack), *pck_cnt);

    if(cs->pid == QUADCAST_2S_PID) {
        fill_qs2s_data(&cs->upper, *data_arr, *pck_cnt, upper);
        fill_qs2s_data(&cs->lower, *data_arr, *pck_cnt, lower);
    } else {
        fill_data(&cs->upper, *data_arr, *pck_cnt, upper);
        fill_data(&cs->lower, *data_arr+BYTE_STEP, *pck_cnt, lower);
        equalize(seq_upper, seq_lower, data_arr);
    }

    #ifdef DEBUG
    print_datpack(data_arr, *pck_cnt);
    #endif

    return data_arr;
}

static void get_mode_sizes(struct colschemes *cs, int *seq_upper,
                                                                int *seq_lower)
{
    *seq_upper = count_data(&cs->upper, cs->pid);
    *seq_lower = count_data(&cs->lower, cs->pid);
    if(*seq_upper < 1 || *seq_lower < 1) {
        if (cs->pid == QUADCAST_2S_PID)
            printf(QS_2S_NOSUPPORT_MSG, cs->upper.mode);
        else
            puts(NOSUPPORT_MSG);
        exit(254);
    }
}

short count_color_commands(const datpack *data_arr, int pck_cnt, int colgroup)
{
    short cnt, step = 0;
    const byte_t *b;
    if(colgroup) /* case of lower color commands */
        step = BYTE_STEP;
    cnt = (pck_cnt-1)*8;
    for(b = data_arr[pck_cnt-1]; b < data_arr[pck_cnt-1]+DATA_PACKET_SIZE;
                                                         b += 2*BYTE_STEP) {
        if(*(b+step) != RGB_CODE)
            break;
        cnt++;
    }
    return cnt;
}

static int count_data(struct colscheme *colsch, int pid)
{
    if(pid == QUADCAST_2S_PID) /* the protocol is different for this one */
        return count_2s_data(colsch);

    if(strequ(colsch->mode, "solid")) {
        return 1;
    } else if(strequ(colsch->mode, "blink")) {
        return count_blink_data(colsch, NULL);
    } else if(strequ(colsch->mode, "cycle") || strequ(colsch->mode, "wave")) {
        return count_cycle_data(colsch, NULL);
    } else if(strequ(colsch->mode, "lightning") ||
              strequ(colsch->mode, "pulse")) {
        return count_lightning_data(colsch, NULL);
    }
    return -1;
}

static int count_2s_data(struct colscheme *colsch)
{
    unsigned int rawsize = 0;

    if(strequ(colsch->mode, "solid")) {
        /* 6 packets for theoretical 140 LEDs where 108 are actually used */
        return QS2S_SOLID_PKT_CNT;
    } else if(strequ(colsch->mode, "blink")) {
        count_blink_data(colsch, &rawsize);
    } else if(strequ(colsch->mode, "cycle") || strequ(colsch->mode, "wave")) {
        count_cycle_data(colsch, &rawsize);
    } else if(strequ(colsch->mode, "lightning") ||
              strequ(colsch->mode, "pulse")) {
        count_lightning_data(colsch, &rawsize);
    } else {
        return -1;
    }
    /* one host-driven animation frame = 6 physical packets (108 LEDs) */
    return rawsize < 1 ? -1 : (int)rawsize * QS2S_SOLID_PKT_CNT;
}

static unsigned int count_blink_data(struct colscheme *colsch,
                                                        unsigned int *rawsize)
{
    unsigned int frame, size = 0;

    if(colsch->colors[0] == nocolor) { /* case of random colors */
        srand(time(NULL)); /* random seed (must be done only once) */
        if(rawsize)
            *rawsize = MAX_COLPAIR_COUNT;
        return MAX_PCT_COUNT;
    }

    frame = 101-colsch->spd + colsch->dly;
    size = sizeof_frames(colsch->colors, frame);
    if(rawsize)
        *rawsize = size;
    return DIV_CEIL(size, COLPAIR_PER_PCT);
}

static unsigned int count_cycle_data(const struct colscheme *colsch,
                                                        unsigned int *rawsize)
{
    unsigned int size;
    /* The size of one gradient: */
    size = SPEED_RANGE(MIN_CYCL_TR, MAX_CYCL_TR, colsch->spd);
    /* The size of all colpairs: */
    size *= colarr_len(colsch->colors);
    if(size > MAX_COLPAIR_COUNT) { /* case of overflow */
        if(rawsize)
            *rawsize = MAX_COLPAIR_COUNT;
        return MAX_PCT_COUNT;
    }
    if(rawsize)
        *rawsize = size;
    return DIV_CEIL(size, COLPAIR_PER_PCT);
}

static unsigned int count_lightning_data(struct colscheme *colsch,
                                                        unsigned int *rawsize)
{
    unsigned int frame, size = 0;
    frame = SPEED_RANGE(MIN_LGHT_BL, MAX_LGHT_BL, colsch->spd) +
            SPEED_RANGE(MIN_LGHT_UP, MAX_LGHT_UP, colsch->spd) +
            SPEED_RANGE(MIN_LGHT_DOWN, MAX_LGHT_DOWN, colsch->spd);
    size = sizeof_frames(colsch->colors, frame);
    if(rawsize)
        *rawsize = size;
    return DIV_CEIL(size, COLPAIR_PER_PCT);
}

static unsigned int sizeof_frames(int *color, unsigned int framesize)
{
    unsigned int size = 0;
    for(; *color != nocolor && size+framesize <= MAX_COLPAIR_COUNT; color++)
        size += framesize;
    *color = nocolor; /* strip the array */
    return size;
}

static unsigned int colarr_len(const int *arr)
{
    unsigned int cnt;
    for(cnt = 0; *(arr+cnt) != nocolor; cnt++)
        {}
    return cnt;
}

static void fill_data(struct colscheme *colsch, byte_t *da, int pckcnt,
                                                                     int group)
{
    if(strequ(colsch->mode, "solid")) {
        sequence_solid(colsch->colors, da);
    } else if(strequ(colsch->mode, "blink")) {
        if(colsch->colors[0] == nocolor)
            sequence_blink_random(colsch->spd, colsch->dly, da);
        else
            sequence_blink(colsch, da, pckcnt);
    } else if(strequ(colsch->mode, "cycle")) {
        sequence_cycle(colsch->colors, colsch->stops, colsch->spd, da);
    } else if(strequ(colsch->mode, "wave")) {
        sequence_wave(colsch->colors, colsch->spd, group, da);
    } else if(strequ(colsch->mode, "lightning")) {
        sequence_lightning(colsch->colors, colsch->spd, group, 0, da);
    } else if(strequ(colsch->mode, "pulse")) {
        sequence_lightning(colsch->colors, colsch->spd, group, 1, da);
    }
}

static void fill_qs2s_data(struct colscheme *colsch, byte_t *da,
                                                         int pckcnt, int group)
{
    int pcknum = 0;

    for(; pcknum < pckcnt; pcknum++) {
        da[pcknum*DATA_PACKET_SIZE] = QS2S_DISPLAY_CODE;
        da[pcknum*DATA_PACKET_SIZE+1] = QS2S_RGB_PACKET_CODE;
        /* packet index is local to its own 6-packet display frame */
        da[pcknum*DATA_PACKET_SIZE+2] = pcknum % QS2S_SOLID_PKT_CNT;
    }

    /* Temporary hardware-mapping probe: QUADCASTRGB_DEBUG_ROW=N (0-8) lights
     * up only row N of the LED matrix so the physical layout can be read
     * off the mic. Remove once the mute-ring row is identified. */
    if(getenv("QUADCASTRGB_DEBUG_ROW")) {
        qs2s_debug_row(da, atoi(getenv("QUADCASTRGB_DEBUG_ROW")), group);
        return;
    }

    if(strequ(colsch->mode, "solid"))
        sequence_solid_qs2s(colsch->colors, da, group);
    else
        qs2s_animate(colsch, da, pckcnt/QS2S_SOLID_PKT_CNT, group);
}

static void qs2s_debug_row(byte_t *frame, int row, int group)
{
    int start = (group == upper) ? 0 : QS2S_LED_CNT/2;
    int end = start + QS2S_LED_CNT/2;
    int led;

    for(led = start; led < end; led++) {
        int color = (qs2s_led_row(led) == row) ? 0xffffff : 0;
        write_hexcolor(color, qs2s_led_ptr(frame, led));
    }
}

/* True vertical row (0=top) of a wire-order LED. The strip is wired in a
 * serpentine: even columns run top->bottom in wire order, odd columns
 * bottom->top, so the raw led%9 offset has to be flipped for even columns */
static int qs2s_led_row(int led)
{
    int local = led % QS2S_LEDS_PER_COLUMN;
    return (qs2s_led_column(led) % 2 == 0)
                              ? QS2S_LEDS_PER_COLUMN-1 - local : local;
}

/* Host-driven animation for the Quadcast 2S: there is no on-device effect
 * engine (confirmed against OpenRGB's driver), so every mode is realised by
 * repeatedly pushing full 108-LED frames, exactly like the control-transfer
 * protocol does for the older mics. The per-step colors are produced by
 * reusing the existing sequence_* generators against a private buffer. */
static void qs2s_animate(struct colscheme *colsch, byte_t *da, int frame_total,
                                                                     int group)
{
    unsigned int rawsize = 0, k;
    byte_t *cmdbuf, *cmd;

    if(strequ(colsch->mode, "blink")) {
        count_blink_data(colsch, &rawsize);
    } else if(strequ(colsch->mode, "cycle") || strequ(colsch->mode, "wave")) {
        count_cycle_data(colsch, &rawsize);
    } else if(strequ(colsch->mode, "lightning") ||
                                            strequ(colsch->mode, "pulse")) {
        count_lightning_data(colsch, &rawsize);
    }
    if(rawsize < 1)
        return;

    cmdbuf = calloc(2*BYTE_STEP, rawsize);
    if(!cmdbuf)
        return;

    if(strequ(colsch->mode, "blink")) {
        if(colsch->colors[0] == nocolor)
            sequence_blink_random(colsch->spd, colsch->dly, cmdbuf);
        else
            sequence_blink(colsch, cmdbuf, rawsize);
    } else if(strequ(colsch->mode, "cycle") || strequ(colsch->mode, "wave")) {
        /* "wave" gets its travelling look from qs2s_write_wave_group below,
         * spreading this same plain cycle across the ring's 12 columns; the
         * old two-diode phase shift (wave_array_shift) doesn't apply here */
        sequence_cycle(colsch->colors, colsch->stops, colsch->spd, cmdbuf);
    } else if(strequ(colsch->mode, "lightning")) {
        sequence_lightning(colsch->colors, colsch->spd, group, 0, cmdbuf);
    } else if(strequ(colsch->mode, "pulse")) {
        sequence_lightning(colsch->colors, colsch->spd, group, 1, cmdbuf);
    }

    for(k = 0; k < (unsigned int)frame_total; k++) {
        byte_t *frame = da + (size_t)k*QS2S_SOLID_PKT_CNT*DATA_PACKET_SIZE;
        if(strequ(colsch->mode, "wave")) {
            qs2s_write_wave_group(frame, cmdbuf, rawsize, k, group,
                                  colsch->angle, colsch->width);
        } else {
            cmd = cmdbuf + (k % rawsize)*2*BYTE_STEP;
            qs2s_write_group_color(frame, (cmd[1]<<16) + (cmd[2]<<8) + cmd[3],
                                   group);
        }
    }
    free(cmdbuf);
}

/* Physical LED index -> ring column (0-11), from the 12x9 matrix HyperX
 * uses on this mic (reverse-engineered by the OpenRGB project); 9 LEDs per
 * column, columns run consecutively in wire order starting at column 10 */
static int qs2s_led_column(int led)
{
    return ((led/QS2S_LEDS_PER_COLUMN) + 10) % QS2S_NUM_COLUMNS;
}

static byte_t *qs2s_led_ptr(byte_t *frame, int led)
{
    return frame + (led/QS2S_LEDS_PER_PACKET)*DATA_PACKET_SIZE
                  + 4 + 3*(led % QS2S_LEDS_PER_PACKET);
}

/* Spreads the cmdbuf gradient across the ring by column instead of just
 * splitting the mic into two halves, so the colors travel around the mic
 * instead of jumping between two blocks */
static void qs2s_write_wave_group(byte_t *frame, const byte_t *cmdbuf,
                          unsigned int rawsize, unsigned int frame_step,
                          int group, int angle_deg, int width_pct)
{
    int start = (group == upper) ? 0 : QS2S_LED_CNT/2;
    int end = start + QS2S_LED_CNT/2;
    int led;
    double theta = angle_deg*QS2S_PI/180.0;
    double cos_t = cos(theta), sin_t = sin(theta);
    /* width < 100% shows only a slice of the gradient across the ring at
     * once (it still scrolls through the rest as frame_step advances);
     * width > 100% repeats the gradient more than once around the ring */
    double span = rawsize*width_pct/100.0;

    for(led = start; led < end; led++) {
        double col_phase = qs2s_led_column(led)*span/QS2S_NUM_COLUMNS;
        double row_phase = qs2s_led_row(led)*span/QS2S_LEDS_PER_COLUMN;
        long shift = (long)(col_phase*cos_t + row_phase*sin_t);
        long phase = ((long)frame_step + shift) % (long)rawsize;
        const byte_t *cmd;
        if(phase < 0)
            phase += rawsize;
        cmd = cmdbuf + phase*2*BYTE_STEP;
        memcpy(qs2s_led_ptr(frame, led), cmd+1, 3);
    }
}

static void set_brightness(int *color, int br) 
{
    for(; color && *color != nocolor; color++) {
        int i, shift;
        byte_t rgb[3];
        for(shift = 16, i = 0; i < 3; shift -= 8, i++) {
            rgb[i] = (byte_t)((*color >> shift) & 0xff);
            rgb[i] = rgb[i]*br/100;
        }
        *color = (rgb[0] << 16) + (rgb[1] << 8) + rgb[2];
    }
}

static void equalize(int upper_size, int lower_size, datpack *da)
{
    enum { upper = 0, lower };
    byte_t *upper_end, *lower_end;
    upper_size = count_color_commands(da, upper_size, upper);
    lower_size = count_color_commands(da, lower_size, lower);
    upper_end = *da + 2*BYTE_STEP*(upper_size-1);
    lower_end = *da + 2*BYTE_STEP*(lower_size-1) + BYTE_STEP;

    if(upper_size < lower_size) {
        fillup_to(upper_size, upper_end+2*BYTE_STEP, lower_end-BYTE_STEP);
    } else if(lower_size < upper_size) {
        fillup_to(lower_size, lower_end+2*BYTE_STEP, upper_end+BYTE_STEP);
    } /* else equalizing isn't needed */
}

static void fillup_to(size_t copy_size, byte_t *curr, byte_t *finish)
{
    while(curr <= finish) {
        memcpy(curr, curr-(2*BYTE_STEP*copy_size), BYTE_STEP);
        curr += 2*BYTE_STEP;
    }
}

/* Mode-related functions */
static void sequence_solid(const int *colors, byte_t *da)
{
    *da = RGB_CODE; /* write code to the first byte */
    write_hexcolor(*colors, da+1); /* write RGB */
}

static void sequence_solid_qs2s(const int *colors, byte_t *da, int group)
{
    qs2s_write_group_color(da, colors[0], group);
}

static void qs2s_write_group_color(byte_t *frame, int color, int group)
{
    if(group == upper)
        fill_qs2s_packets_with_color(frame, color, 0, QS2S_LED_CNT/2);
    else if(group == lower)
        fill_qs2s_packets_with_color(frame+2*DATA_PACKET_SIZE, color, 14,
                                                               QS2S_LED_CNT/2);
}

static void fill_qs2s_packets_with_color(byte_t *start, int clr, int offset,
                                                                       int cnt)
{
    int i = 0;
    byte_t *p = start + 4 + 3*offset; /* skip the codes part by adding 4 */

    write_hexcolor(clr, p);

    for(; i <= cnt; i++) {
        p = ((p+3 - start) % DATA_PACKET_SIZE == 0) ? p + 7 : p + 3 ;
        memcpy(p, start + 4 + 3*offset, 3);
    }
}

static void sequence_blink_random(int speed, int delay, byte_t *da)
{
    int colpair = 0;
    int col_seg, dly_seg;

    col_seg = RAND_COL_SEG_MIN +
              (int)(speed * (RAND_COL_SEG_MAX-RAND_COL_SEG_MIN)) / MAX_SPD;
    dly_seg = RAND_DLY_SEG_MIN +
              (int)(delay * (RAND_DLY_SEG_MAX-RAND_DLY_SEG_MIN)) / MAX_DLY;
     
    while(colpair < MAX_COLPAIR_COUNT) {
        colpair += col_seg + dly_seg;
        if(colpair > MAX_COLPAIR_COUNT) /* strip color segment if overflow */
            col_seg -= colpair - MAX_COLPAIR_COUNT;
        blink_segment_fill(random_color(), col_seg, dly_seg, &da);
    }
}

static void sequence_blink(const struct colscheme *colsch, byte_t *da,
                           int pckcnt)
{
    const int *col;
    int col_seg = 101 - colsch->spd;
    for(col = colsch->colors; *col != nocolor; col++)
        blink_segment_fill(*col, col_seg, colsch->dly, &da);
}

static void blink_segment_fill(int col, int col_seg, int dly_seg, byte_t **da)
{
    color_fill(col, col_seg, da);
    color_fill(black, dly_seg, da);
}

static void sequence_cycle(const int *color, const int *stops, int spd,
                                                                  byte_t *da)
{
    const int *first_col = color;
    int color_cnt = colarr_len(color);
    int total_length = get_gradient_length(color, spd) * color_cnt;
    int has_stops = stops && stops[0] != nocolor;
    /* The wrap segment (last color back to the first) is sized from how
     * much room the user actually left after the last stop: close to
     * position 100 gives a short blend, further away gives a longer one.
     * A small floor keeps it from ever fully collapsing (hard, flickering
     * seam) when a stop sits right at 100. Non-wrap segments split
     * whatever's left, proportional to their own positions, so the total
     * never exceeds the buffer allocated for this scheme. */
    int span = has_stops ? stops[color_cnt-1] - stops[0] : 0;
    int wrap_length = has_stops ?
                       total_length * (100 + stops[0] - stops[color_cnt-1])
                                                                  / 100 : 0;
    int idx = 0, written = 0;

    if(has_stops && total_length > MIN_WRAP_TR && wrap_length < MIN_WRAP_TR)
        wrap_length = MIN_WRAP_TR;

    for(; *color != nocolor; color++, idx++) {
        int tr_start, tr_end, tr_length;
        int is_wrap = (*(color+1) == nocolor);

        tr_start = *color;
        tr_end = is_wrap ? *first_col : *(color+1);

        if(!has_stops) {
            tr_length = total_length / color_cnt;
        } else if(is_wrap) {
            tr_length = total_length - written; /* whatever remains */
        } else if(span > 0) {
            tr_length = (total_length-wrap_length) * (stops[idx+1]-stops[idx])
                                                                     / span;
        } else {
            tr_length = (total_length-wrap_length) / (color_cnt-1);
        }
        write_gradient(&da, tr_start, tr_end, tr_length);
        written += tr_length;
    }
}

static int get_gradient_length(const int *color, int spd)
{
    int color_cnt, tr_size;

    color_cnt = colarr_len(color);

    tr_size = MIN_CYCL_TR + (MAX_CYCL_TR - MIN_CYCL_TR)*(100 - spd)/100;
    if(tr_size*color_cnt > MAX_COLPAIR_COUNT)
        return MIN_CYCL_TR +
               (MAX_COLPAIR_COUNT/color_cnt - MIN_CYCL_TR)*(100 - spd)/100;
    return tr_size;
}

static void write_gradient(byte_t **da, int start_col, int end_col, int length)
{
    byte_t rgb_st[3], rgb_end[3], rgb_curr[3];
    int shift, i;
    /* Fill the arrays */
    for(shift = 16, i = 0; shift >= 0; shift -= 8, i++) {
        rgb_st[i] = (byte_t)((start_col >> shift) & 0xff);
        rgb_end[i] = (byte_t)((end_col >> shift) & 0xff);
        rgb_curr[i] = rgb_st[i]; /* the start is going to be the 1st rgb */
    }
    /* Write the transition to *da */
    for(i = 1; i <= length; i++, *da += BYTE_STEP) {
        int j;
        **da = RGB_CODE;
        (*da)++;
        for(j = 0; j < 3; j++, (*da)++) {
            **da = rgb_curr[j]; /* write R, G, or B */
            /* Alter the first RGB depending on the second and the length */
            rgb_curr[j] = (int)(rgb_st[j] +
                          ((float)(i)/(length - 1))*(rgb_end[j] - rgb_st[j]));
        }
    }
}

static void sequence_wave(int *color, int spd, int group, byte_t *da)
{
    if(group == lower)
        wave_array_shift(color);
    /* Just do the same as in the Cycle mode; gradient stops aren't
     * supported on this protocol's simple two-diode wave */
    sequence_cycle(color, NULL, spd, da);
}

static void wave_array_shift(int *color)
{
    int *tmp, first;
    first = *color;
    for(tmp = color; *(tmp+1) != nocolor; tmp++)
        *(tmp) = *(tmp+1);
    *(tmp) = first;
}

static void sequence_lightning(const int *color, int spd, int group,
                               int synchronous, byte_t *da)
{
    unsigned int bl_size, up, down; /* the sizes of sections */
    bl_size = SPEED_RANGE(MIN_LGHT_BL, MAX_LGHT_BL, spd);
    up = SPEED_RANGE(MIN_LGHT_UP, MAX_LGHT_UP, spd);
    down = SPEED_RANGE(MIN_LGHT_DOWN, MAX_LGHT_DOWN, spd);
    for(; *color != nocolor; color++) {
        if(group == lower && !synchronous)
            color_fill(black, bl_size, &da);
        write_gradient(&da, black, *color, up);
        write_gradient(&da, next_gradient_color(*color, black, down), black,
                       down);
        if(group == upper || synchronous)
            color_fill(black, bl_size, &da);
    }
}

static int next_gradient_color(int color, int endcolor, unsigned int size)
{
    byte_t rgb[3], rgb_end[3];
    int shift, i, nextcolor = 0;
    for(shift = 16, i = 0; shift >= 0; shift -= 8, i++) {
        /* Get R, G, or B values */
        rgb[i] = (byte_t)((color >> shift) & 0xff);
        rgb_end[i] = (byte_t)((endcolor >> shift) & 0xff);
        /* Perform one step */
        rgb[i] = (int)(rgb[i] +
                 ((float)(1)/(size - 1))*(rgb_end[i] - rgb[i]));
        nextcolor += (int)(rgb[i] << shift);
    }
    return nextcolor;
}

static int random_color()
{
    /* Generates a pseudorandom number from 0x1 to 0xffffff */
    return 1 + (int)(16777215.0*rand()/(RAND_MAX+1.0));
}

static void write_hexcolor(int color, byte_t *mem)
{
    int n;
    for(n = 16; n >= 0; n -= 8) {
        *mem = (byte_t)((color >> n) & 0xff);
        mem++;
    }
}

static void color_fill(int color, int size, byte_t **da)
{
    for(; size > 0; size--, *da += 2*BYTE_STEP) {
        **da = RGB_CODE;
        write_hexcolor(color, (*da)+1);
    }
}

#ifdef DEBUG
static void print_datpack(datpack *da, int pck_cnt)
{
    int i, j;
    printf(N_("Packets to be sent: %d\n"), pck_cnt);
    for(j = 0; j < pck_cnt; j++) {
        printf(N_("Packet %d:\n"), j+1);
        for(i = 0; i < DATA_PACKET_SIZE; i++) {
            printf("%02X ", (unsigned int)da[j][i]);
            if((i+1) % 4 == 0)
                printf("\t");
            if((i+1) % 8 == 0)
                puts("");
        }
        puts("");
    }
}
#endif
