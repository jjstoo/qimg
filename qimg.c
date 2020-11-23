/**
 ** This file is part of the qimg project.
 ** Copyright 2020 Joni Lepistö <joni.m.lepisto@gmail.com>.
 **
 ** This program is free software: you can redistribute it and/or modify
 ** it under the terms of the GNU General Public License as published by
 ** the Free Software Foundation, either version 3 of the License, or
 ** (at your option) any later version.
 **
 ** This program is distributed in the hope that it will be useful,
 ** but WITHOUT ANY WARRANTY; without even the implied warranty of
 ** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 ** GNU General Public License for more details.
 **
 ** You should have received a copy of the GNU General Public License
 ** along with this program.  If not, see <http://www.gnu.org/licenses/>.
 **
 ** ----------------------------------------------------------------------------
 **
 ** Qimg - Quick Image Display
 **
 ** Qimg provides a totally stripped and straightforward way of displaying
 ** images on a Linux system. No desktop environment or windowing system needed!
 **
 ** Qimg uses the Linux framebuffer interface to draw bitmap data on the screen.
 ** Images are drawn as raw pixels with no windowing context whatsoever.
 **
 ** Why? Mostly for fun but I've had a few occasions on some terminal-only
 ** systems where it would've been nice to inspect images.
 **/


#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb_image.h>
#include <stb_image_resize.h>

#include <fcntl.h>
#include <glob.h>
#include <stdint.h>
#include <signal.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <linux/fb.h>

#define FB_IDX_MAX_SIZE 4
#define FB_DEV_BASE "/dev/fb"
#define FB_CLASS_BASE "/sys/class/graphics/fb"
#define FB_CLASS_RESOLUTION "/virtual_size"
#define FB_GLOB "/sys/class/graphics/fb[0-9]"

/* Standard terminal control sequences */
#define CUR_SHOW "\e[?25h"
#define CUR_HIDE "\e[?25l"

/* Adjust according to platform limitations */
#define MAX_IMAGES 24

#define log_msg(fmt_, ...)\
    fprintf(stderr, (fmt_ "\n"), ##__VA_ARGS__)

#define assertf(A, fmt_, ...)\
    if (!(A)) {log_msg("[ERROR]: " fmt_, ##__VA_ARGS__); exit(EXIT_FAILURE);}\
    void f(void) /* To enforce semicolon and prevent warnings */

/* Generates lookup functions for converting string arguments to enums */
#define STRING_TO_ENUM_(e) e str2##e(const char* str) {                         \
    int j;                                                                      \
    for (j = 0;  j < sizeof (e##_conversion) / sizeof (e##_conversion[0]);  ++j)\
        if (!strcmp (str, e##_conversion[j].str))                               \
            return e##_conversion[j].en;                                        \
    assertf(false, "Unknown option %s for " #e, str);                           \
}

typedef enum { false, true } bool;

typedef struct res_ {
    int x;
    int y;
} qimg_resolution;

typedef struct col_ {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t a;
} qimg_color;

typedef struct fb_ {
    qimg_resolution res;            /* framebuffer resolution */
    unsigned int size;              /* framebuffer size */
    int fbfd;                       /* framebuffer file descriptor */
    char* fbdata;                   /* framebuffer data pointer */
} qimg_fb;

typedef struct im_ {
    qimg_resolution res;            /* resolution */
    int c;                          /* channels */
    char _padding[4];               /* guess what */
    uint8_t* pixels;                /* image data pointer */
} qimg_image;

typedef struct collection_ {
    int size;                       /* number of images */
    char _padding[4];               /* yeah */
    qimg_image images[MAX_IMAGES];  /* image array */
} qimg_collection;

typedef enum pos_ {
    POS_CENTERED,
    POS_TOP_LEFT,
    POS_TOP_RIGHT,
    POS_BOTTOM_RIGHT,
    POS_BOTTOM_LEFT
} qimg_position;        /* image position */

typedef enum bg_ {
    BG_BLACK,
    BG_WHITE,
    BG_RED,
    BG_GREEN,
    BG_BLUE,
    BG_DISABLED
} qimg_bg;              /* background color */

typedef enum scale_ {
    SCALE_DISABLED,
    SCALE_FIT,
    SCALE_STRETCH,
    SCALE_FILL
} qimg_scale;           /* image scale style */


/* Lookup tables to find enums with string arguments */
const static struct {
    qimg_position en;
    const char *str;
} qimg_position_conversion [] = {
    {POS_CENTERED, "c"},
    {POS_TOP_LEFT, "tl"},
    {POS_TOP_RIGHT, "tr"},
    {POS_BOTTOM_LEFT, "bl"},
    {POS_BOTTOM_RIGHT, "br"}
};

const static struct {
    qimg_bg en;
    const char *str;
} qimg_bg_conversion [] = {
    {BG_BLACK, "black"},
    {BG_WHITE, "white"},
    {BG_RED, "red"},
    {BG_GREEN, "green"},
    {BG_BLUE, "blue"},
    {BG_DISABLED, "disabled"}
};

const static struct {
    qimg_scale en;
    const char *str;
} qimg_scale_conversion [] = {
    {SCALE_DISABLED, "disabled"},
    {SCALE_FIT, "fit"},
    {SCALE_STRETCH, "stretch"},
    {SCALE_FILL, "fill"}
};

STRING_TO_ENUM_(qimg_position)
STRING_TO_ENUM_(qimg_bg)
STRING_TO_ENUM_(qimg_scale)

static volatile bool run = true; /* used to go through cleanup on exit */
static clock_t begin_clk;

/**
 * @brief Gets color of an image at given position
 * @param im    input image
 * @param x     pos x
 * @param y     pos y
 * @return color of the given point
 */
qimg_color qimg_get_pixel_color(qimg_image* im, int x, int y);

/**
 * @brief Gets color values for background color enumeration
 * @param bg    background color value
 * @return color value
 */
qimg_color qimg_get_bg_color(qimg_bg bg);

/**
 * @brief Loads image at given path
 * @param input_path    input path
 * @return loaded image, exits if loading errors
 */
qimg_image qimg_load_image(char* input_path);

/**
 * @brief Loads multiple images to a collection
 * @param input_paths   input path vector
 * @param n_inputs      number of inputs
 * @return collection of images
 */
qimg_collection qimg_load_images(char** input_paths, int n_inputs);

/**
 * @brief Resizes image
 * @param im        image to resize
 * @param dest_res  target resolution
 * @return          true if resizing succeeded, false if not
 */
bool qimg_resize_image(qimg_image* im, qimg_resolution dest_res);

qimg_resolution qimg_get_scaled_dims(qimg_resolution src, qimg_resolution vp,
                                     qimg_scale scale);

/**
 * @brief Opens framebuffer with given index
 * @param idx   framebuffer index (/dev/fb<idx>)
 * @return framebuffer instance
 */
qimg_fb qimg_open_framebuffer(int idx);

/**
 * @brief Gets milliseconds since program start
 * @return milliseconds from start
 */
uint32_t qimg_get_millis(void);

/**
 * @brief Checks if given milliseoncds have elapsed since timestamps
 * @param start     beginning timestamp
 * @param millis    interval to check
 * @return true if the given interval has elapsed since beginnind
 */
bool qimg_have_millis_elapsed(uint32_t start, uint32_t millis);

/**
 * @brief Sleeps for given amount of time
 * @param ms    sleep time in milliseconds
 */
void qimg_sleep_ms(uint32_t ms);

/**
 * @brief Fills the framebuffer with black
 * @param fb    target framebuffer
 */
void qimg_clear_framebuffer(qimg_fb* fb);

/**
 * @brief Frees and unmaps the framebuffer instance
 * @param fb    target framebuffer
 */
void qimg_free_framebuffer(qimg_fb* fb);

/**
 * @brief Frees all images in a collection
 * @param col   target collection
 */
void qimg_free_collection(qimg_collection* col);

/**
 * @brief Frees an image from memory
 * @param im    target image
 */
void qimg_free_image(qimg_image* im);

/**
 * @brief Draws a collection of images on the framebuffer
 * @param col       image collection
 * @param fb        target framebuffer
 * @param pos       image positioning
 * @param bg        background style
 * @param repaint   keep repainting the image
 * @param delay_s   delay between images
 */
void qimg_draw_images(qimg_collection* col, qimg_fb* fb, qimg_position pos,
                      qimg_bg bg, bool repaint, int delay_s);

/**
 * @brief Draws an image on the framebuffer
 *
 * If delay_s <= 0, it is not applied. In this case the image is drawn
 * indefinitely if repaint is set to true.
 *
 * @param im        image
 * @param fb        target framebuffer
 * @param pos       image positioning
 * @param bg        background style
 * @param repaint   keep repainting the image
 * @param delay_s   time to keep the image on the framebuffer.
 */
void qimg_draw_image(qimg_image* im, qimg_fb* fb, qimg_position pos, qimg_bg bg,
                     bool repaint, int delay_s);

int get_default_framebuffer_idx(void);
void set_cursor_visibility(bool blink);
void interrupt_handler(int);
void print_help(void);

int get_default_framebuffer_idx() {
    /* Glob search for framebuffer devices */
    glob_t globbuf;
    glob(FB_GLOB, 0, NULL, &globbuf);

    /* At least one result matched */
    assertf(globbuf.gl_pathc > 0, "No framebuffers found");

    /* Get framebuffer index */
    strsep(&globbuf.gl_pathv[0], "fb"); /* Discard the first split */
    return atoi(strsep(&globbuf.gl_pathv[0], "fb"));
}

void qimg_free_framebuffer(qimg_fb* fb) {
    munmap(fb->fbdata, fb->size);
    close(fb->fbfd);
}

uint32_t qimg_get_millis(void) {
    return (uint32_t)((double)(clock() - begin_clk) / CLOCKS_PER_SEC) * 1000;
}

bool qimg_have_millis_elapsed(uint32_t start, uint32_t millis) {
    return (qimg_get_millis() - start) > millis;
}

void qimg_sleep_ms(uint32_t ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000;
    nanosleep(&ts, NULL);
}

qimg_fb qimg_open_framebuffer(int idx) {
    /* Append device index to the framebuffer device path */
    char idx_buf[FB_IDX_MAX_SIZE];
    assertf(snprintf(idx_buf, FB_IDX_MAX_SIZE, "%d", idx) <
            FB_IDX_MAX_SIZE, "Framebuffer index overflow");
    char dev[FB_IDX_MAX_SIZE + sizeof(FB_DEV_BASE)] = FB_DEV_BASE;
    strncat(dev, idx_buf, FB_IDX_MAX_SIZE);

    /* Open framebuffer */
    qimg_fb fb;
    fb.fbfd = open(dev, O_RDWR);
    assertf(fb.fbfd >= 0, "Framebuffer device fopen() failed");

    /* Get framebuffer information */
    struct fb_var_screeninfo vinfo;
    ioctl(fb.fbfd, FBIOGET_VSCREENINFO, &vinfo);

    fb.res.x = (int) vinfo.xres;
    fb.res.y = (int) vinfo.yres;
    unsigned int fb_bpp = vinfo.bits_per_pixel;
    unsigned int fb_bytes = fb_bpp / 8;

    /* Calculate data size and map framebuffer to memory */
    fb.size = fb.res.x * fb.res.y * fb_bytes;
    fb.fbdata = mmap(0, fb.size,
                        PROT_READ | PROT_WRITE, MAP_SHARED, fb.fbfd, (off_t) 0);

    return fb;
}

void qimg_clear_framebuffer(qimg_fb* fb) {
    memset(fb->fbdata, 0, fb->size);
}

void qimg_draw_images(qimg_collection* col, qimg_fb* fb, qimg_position pos,
                      qimg_bg bg, bool repaint, int delay_s) {
    for (int i = 0; i < col->size; ++i) {
        qimg_draw_image(&col->images[i], fb, pos, bg, repaint, delay_s);
        if (!run) /* Draw routine exited via interrupt signal */
            break;
    }

}

void qimg_draw_image(qimg_image* im, qimg_fb* fb, qimg_position pos, qimg_bg bg,
                     bool repaint, int delay_s) {
    char* buf = malloc(fb->size);
    memcpy(buf, fb->fbdata, fb->size);
    qimg_color c;
    int offs;
    int x, y;
    uint32_t delay_ms = delay_s * 1000;
    bool delay_set = (delay_s > 0);
    for (int x_ = 0; x_ < fb->res.x; ++x_) { /* This is embarassingly parallel */
        for (int y_ = 0; y_ < fb->res.y; ++y_) {
            switch (pos) {
            case POS_TOP_LEFT:
                x = x_;
                y = y_;
                break;
            case POS_TOP_RIGHT:
                x = x_ - (fb->res.x - im->res.x);
                y = y_;
                break;
            case POS_BOTTOM_RIGHT:
                x = x_ - (fb->res.x - im->res.x);
                y = y_ - (fb->res.y - im->res.y);
                break;
            case POS_BOTTOM_LEFT:
                x = x_;
                y = y_ - (fb->res.y - im->res.y);
                break;
            case POS_CENTERED:
                x = x_ - ((fb->res.x / 2) - (im->res.x / 2));
                y = y_ - ((fb->res.y / 2) - (im->res.y / 2));
                break;
            }

            if (x >= im->res.x || y >= im->res.y || x < 0 || y < 0) {
                if (bg == BG_DISABLED) {
                    continue; /* Keep the framebuffer as-is */
                } else {
                    c = qimg_get_bg_color(bg);
                }
            } else {
                c = qimg_get_pixel_color(im, x, y);
            }

            offs = (y_ * fb->res.x + x_) * 4;
            buf[offs + 0] = (char) c.b;
            buf[offs + 1] = (char) c.g;
            buf[offs + 2] = (char) c.r;
            buf[offs + 3] = (char) c.a;
        }
    }

    uint32_t start_ticks = qimg_get_millis();
    do {
        memcpy(fb->fbdata, buf, fb->size);

        /* Delay and repaint, check timer and draw again if needed */
        if (delay_set && repaint) {
            if (qimg_have_millis_elapsed(start_ticks, delay_ms))
                break;
        }
        /* Delay and no repaint, draw once and wait before break */
        else if (delay_set && !repaint) {
            uint32_t to_sleep = delay_ms - (qimg_get_millis() - start_ticks);
            if (to_sleep > 0)
                qimg_sleep_ms(to_sleep);
            break;
        }
        /* No delay or repaint, break immediately */
        else if (!delay_set && !repaint) {
            break;
        }
    } while (run);
}

qimg_color qimg_get_pixel_color(qimg_image* im, int x, int y) {
    assertf(x < im->res.x && y < im->res.y, "Image coordinates out of bounds");
    uint8_t* offset = im->pixels + (y * im->res.x + x) * im->c;
    qimg_color color;

    if (im->c < 3) {
        color.r = offset[0];
        color.g = offset[0];
        color.b = offset[0];
        color.a = im->c >= 2 ? offset[1] : 0xff;
    } else {
        color.r = offset[0];
        color.g = offset[1];
        color.b = offset[2];
        color.a = im->c >= 4 ? offset[3] : 0xff;
    }
    return color;
}

qimg_color qimg_get_bg_color(qimg_bg bg) {
    qimg_color bg_color;
    switch (bg) {
    case BG_BLACK:
        bg_color = (qimg_color){0, 0, 0, 0xff};
        break;
    case BG_WHITE:
        bg_color = (qimg_color){0xff, 0xff, 0xff, 0xff};
        break;
    case BG_RED:
        bg_color = (qimg_color){0xff, 0, 0, 0xff};
        break;
    case BG_GREEN:
        bg_color = (qimg_color){0, 0xff, 0, 0xff};
        break;
    case BG_BLUE:
        bg_color = (qimg_color){0, 0, 0xff, 0xff};
        break;
    default:
        bg_color = (qimg_color){0, 0, 0, 0xff};
        break;
    }
    return bg_color;
}

qimg_image qimg_load_image(char* input_path) {
    qimg_image im;
    im.pixels = stbi_load(input_path, &im.res.x, &im.res.y, &im.c, 0);
    assertf(im.pixels, "Loading image %s failed", input_path);
    return im;
}

qimg_collection qimg_load_images(char** input_paths, int n_inputs) {
    qimg_collection col;
    int i;
    for (i = 0; i < n_inputs; ++i) {
        col.images[i] = qimg_load_image(input_paths[i]);
    }
    col.size = i;
    return col;
}

void qimg_free_image(qimg_image* im) {
    if (im->pixels)
    free(im->pixels);
}

void qimg_free_collection(qimg_collection* col) {
    for (int i = 0; i < col->size; ++i)
        qimg_free_image(&col->images[i]);
}

bool qimg_resize_image(qimg_image* im, qimg_resolution dest_res) {
    uint8_t* out_buf = malloc(dest_res.x * dest_res.y * im->c);
    if (stbir_resize_uint8(im->pixels, im->res.x, im->res.y, 0, out_buf,
                           dest_res.x, dest_res.y, 0, im->c)) {
        /* Update data pointer */
        free(im->pixels);
        im->pixels = out_buf;

        /* Update resolution */
        im->res.x = dest_res.x;
        im->res.y = dest_res.y;
        return true;
    }
    return false;
}

qimg_resolution qimg_get_scaled_dims(qimg_resolution src, qimg_resolution vp,
                                     qimg_scale scale) {
    qimg_resolution r;
    float p;
    float px = (float) vp.x / (float) src.x;
    float py = (float) vp.y / (float) src.y;

    switch (scale) {
    case SCALE_DISABLED:
        r = src;
        break;
    case SCALE_STRETCH:
        r = vp;
        break;
    case SCALE_FILL:
        p = (px > py) ? px : py;
        r.x = src.x * p;
        r.y = src.y * p;
        break;
    case SCALE_FIT:
        p = (px > py) ? py : px;
        r.x = src.x * p;
        r.y = src.y * p;
        break;
    }

    return r;
}

void set_cursor_visibility(bool visible) {
    if (visible) printf(CUR_SHOW);
    else printf(CUR_HIDE);
    fflush(stdout);
}

void interrupt_handler(int dummy) {
    run = false;
}

void print_help() {
    printf("QIMG - Quick Image Display\n"
           "\n"
           "Usage: qimg [OPTION]... INPUT...\n"
           "\n"
           "General options:\n"
           "-h,             Print this help.\n"
           "-b <index>,     Use framebuffer device with given index.\n"
           "                Default is to use one found with the lowest index.\n"
           "-c,             Hide terminal cursor.\n"
           "-r,             Keep repainting the image. If hiding the cursor\n"
           "                fails, this will certainly work for keeping the\n"
           "                image on top with the cost of CPU usage.\n"
           "\n"
           "Image layout:\n"
           "-pos <position> Draw the image in given position. Possible values:\n"
           "                c   -   centered\n"
           "                tl  -   top left (default)\n"
           "                tr  -   top right\n"
           "                br  -   bottom right\n"
           "                bl  -   bottom left\n"
           "-bg <color>     Fill background with color. Possible values:\n"
           "                black\n"
           "                white\n"
           "                red\n"
           "                green\n"
           "                blue\n"
           "                disabled (transparent, default)\n"
           "-scale <style>  Scale the image with given style. Possible values:\n"
           "                disabled    -   no scaling (default).\n"
           "                fit         -   fit the image to screen, preserving\n"
           "                                aspect ratio.\n"
           "                stretch     -   stretch the image to fill whole screen.\n"
           "                fill        -   fill the screen with the image,\n"
           "                                preserving aspect ratio.\n"
           "\n"
           "Slideshow and timing options:\n"
           "-d <delay>      Slideshow interval in seconds (default 5s).\n"
           "                If used with a single image, the image is displayed\n"
           "                for <delay> seconds.\n"
           "\n"
           "Generic framebuffer operations:\n"
           "(Use one at a time, cannot be joined with other operations)\n"
           "-clear          Clear the framebuffer\n"
           "\n");
}

void parse_arguments(int argc, char *argv[], int* fb_idx, char** input,
                     int* n_inputs, bool* refresh, bool* hide_cursor,
                     qimg_position* pos, qimg_bg* bg, int* slide_delay_s,
                     qimg_scale* scale) {
    assertf(argc > 1, "Arguments missing");
    int opts = 0;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-b") == 0) {
            ++opts;
            if (argc > (++i)) {
                ++opts;
                *fb_idx = atoi(argv[i]);
            }
        } else if (strcmp(argv[i], "-r") == 0) {
            ++opts;
            *refresh = true;
        } else if (strcmp(argv[i], "-c") == 0) {
            ++opts;
            *hide_cursor = true;
        } else if (strcmp(argv[i], "-pos") == 0) {
            ++opts;
            if (argc > (++i)) {
                ++opts;
                *pos = str2qimg_position(argv[i]);
            }
        } else if (strcmp(argv[i], "-bg") == 0) {
            ++opts;
            if (argc > (++i)) {
                ++opts;
                *bg = str2qimg_bg(argv[i]);
            }
        } else if (strcmp(argv[i], "-d") == 0) {
            ++opts;
            if (argc > (++i)) {
                ++opts;
                int dly = atoi(argv[i]);
                assertf(dly >= 0, "Delay must be positive");
                *slide_delay_s = dly;
            }
        } else if (strcmp(argv[i], "-scale") == 0) {
            ++opts;
            if (argc > (++i)) {
                ++opts;
                *scale = str2qimg_scale(argv[i]);
            }
        }
        /* These options only work one at a time, exiting after completion */
        else if (strcmp(argv[i], "-h") == 0) {
            ++opts;
            print_help();
            exit(EXIT_SUCCESS);
        } else if (strcmp(argv[i], "-clear") == 0) {
            ++opts;
            if (*fb_idx == -1)
                *fb_idx = get_default_framebuffer_idx();
            qimg_fb fb = qimg_open_framebuffer(*fb_idx);
            qimg_clear_framebuffer(&fb);
            exit(EXIT_SUCCESS);
        }
    }
    /* We should still have some leftover arguments, these are our inputs */
    while (++opts < argc) {
        input[*n_inputs] = argv[opts];
        ++*n_inputs;
        assertf(*n_inputs <= MAX_IMAGES, "Too many input images (max %d)",
                MAX_IMAGES);
    }
}

int main(int argc, char *argv[]) {

    /* Record start ticks for timekeeping */
    begin_clk = clock();

    /* Setup start values for params */
    int fb_idx = -1;
    int n_inputs = 0;
    int slide_delay_s = 0;
    char* input_paths[MAX_IMAGES];
    bool repaint = false;
    bool hide_cursor = false;
    qimg_position pos = POS_TOP_LEFT;
    qimg_bg bg = BG_DISABLED;
    qimg_scale scale = SCALE_DISABLED;

    parse_arguments(argc, argv, &fb_idx, input_paths, &n_inputs, &repaint,
                    &hide_cursor, &pos, &bg, &slide_delay_s, &scale);

    assertf(n_inputs, "No input file");
    if (fb_idx == -1)
        fb_idx = get_default_framebuffer_idx();
    if (slide_delay_s == 0 && n_inputs > 1) /* Default interval for slideshows */
        slide_delay_s = 5;

    qimg_fb fb = qimg_open_framebuffer(fb_idx);
    qimg_collection col = qimg_load_images(input_paths, n_inputs);

    /* Resize images if needed */
    if (scale != SCALE_DISABLED) {
        qimg_resolution dest;
        for (int i = 0; i < col.size; ++i) {
            dest = qimg_get_scaled_dims(col.images[i].res, fb.res, scale);
            qimg_resize_image(&col.images[i], dest);
        }
    }

    /* Setup exit hooks on signals */
    signal(SIGINT, interrupt_handler);
    signal(SIGTERM, interrupt_handler);

    /* Fasten your seatbelts */
    if (hide_cursor) set_cursor_visibility(false);
    qimg_draw_images(&col, &fb, pos, bg, repaint, slide_delay_s);

    /* if cursor is set to hidden and no repaint nor delay is set, the program
     * shall wait indefinitely for user interrupt */
    if (!repaint && hide_cursor && !slide_delay_s) pause();

    /* Cleanup */
    if (repaint || hide_cursor)
        qimg_clear_framebuffer(&fb);
    if (hide_cursor) set_cursor_visibility(true);
    qimg_free_collection(&col);
    qimg_free_framebuffer(&fb);

    return EXIT_SUCCESS;
}
