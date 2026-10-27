/*
 * MSKman - a maze chase arcade game for the Linux desktop.
 *
 * Everything (mazes, sprites, sounds) is original and drawn or synthesized
 * at runtime into a 224x288 framebuffer that SDL scales to the window.
 */
#include <SDL.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "audio.h"
#include "font.h"
#include "mazegen.h"
#include "mazes.h"

#ifndef VERSION
#define VERSION "dev"
#endif

enum { SCR_W = 224, SCR_H = 288, TILE = 8, MW = 28, MH = 31, MAZE_Y = 24 };
/* Positions are in sub-pixels so speeds can be fractional. */
enum { SUB = 16, TU = TILE * SUB, WORLD_W = MW * TU };
enum { UP, LEFT, DOWN, RIGHT, NODIR = -1 };
static const int DX[4] = {0, -1, 0, 1};
static const int DY[4] = {-1, 0, 1, 0};
#define OPP(d) (((d) + 2) & 3)

#define FPS 60
#define RGB(r, g, b) (0xFF000000u | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))

static const uint32_t C_BLACK = RGB(0, 0, 0);
static const uint32_t C_WHITE = RGB(255, 255, 255);
static const uint32_t C_YELLOW = RGB(255, 230, 0);
static const uint32_t C_RED = RGB(255, 40, 40);
static const uint32_t C_PINK = RGB(255, 170, 220);
static const uint32_t C_CYAN = RGB(0, 230, 255);
static const uint32_t C_ORANGE = RGB(255, 170, 60);
static const uint32_t C_BLUE = RGB(40, 50, 255);
static const uint32_t C_DOT = RGB(255, 205, 175);
static const uint32_t C_DOOR = RGB(255, 180, 230);

/* Maze colour schemes (fill, edge), cycled as levels advance. */
static const uint32_t SCHEMES[][2] = {
    {RGB(20, 52, 140), RGB(110, 200, 255)},
    {RGB(120, 28, 92), RGB(255, 160, 226)},
    {RGB(24, 96, 44), RGB(160, 245, 160)},
    {RGB(110, 60, 16), RGB(255, 196, 108)},
};
#define SCHEME_COUNT (int)(sizeof SCHEMES / sizeof SCHEMES[0])

/* Ghost house geometry (sub-pixels). */
#define EXIT_X (14 * TU)
#define EXIT_Y (11 * TU + TU / 2)
#define HOUSE_Y (14 * TU + TU / 2)
#define START_Y (23 * TU + TU / 2)
#define FRUIT_Y (17 * TU + TU / 2)

typedef enum { G_HOUSE, G_LEAVING, G_ACTIVE, G_EYES, G_ENTERING } GhostState;

typedef struct {
    const char *name;
    uint32_t color;
    int scatter_tx, scatter_ty;
    int x, y, dir;
    GhostState st;
    int fright;
    int reverse;
    int home_x;
    int dot_limit;
} Ghost;

typedef struct {
    int x, y, dir, want;
    int moving;
    int stall;
    int anim;
} Player;

typedef enum {
    ST_TITLE, ST_OPTIONS, ST_READY, ST_PLAY, ST_GHOST_EATEN, ST_DYING, ST_CLEAR, ST_PLAYER_OUT, ST_GAME_OVER
} GameState;

/* Players take turns like the arcade: a turn ends when the player loses a
 * life, and each player keeps their own score, lives, level and maze. */
#define MAX_PLAYERS 4
typedef struct {
    int score, lives, level, extra_given, lost_life_this_level;
    int dots_left, dots_total, dots_eaten, fruits_spawned;
    char grid[MH][MW];
} Turn;

/* Control tuning, saved to the user's config directory. */
typedef struct {
    int corner_assist; /* pixels a turn may be taken before or after a junction */
    int player_speed;  /* percent of normal */
    int ghost_speed;   /* percent of normal */
} Settings;

static const Settings DEFAULT_SETTINGS = {3, 100, 100};
#define ASSIST_MAX 4
#define SPEED_MIN 50
#define SPEED_MAX 150
#define SPEED_STEP 10

/* ---- global game state ---- */
static uint32_t fb[SCR_W * SCR_H];
static uint32_t bg[2][SCR_W * MH * TILE]; /* normal and flashing maze */
static char grid[MH][MW];

static Player pl;
static Ghost gh[4];
static GameState state;
static int state_timer;
static int frame;
static int paused;

static int level, lives, score, hiscore, extra_given;
static int dots_left, dots_total, dots_eaten;
static int life_dots, idle_timer, lost_life_this_level;
static int mode_idx, mode_timer, fright_timer, fright_len, ghosts_eaten;
static int chomp_alt;
static int fruit_timer, fruit_kind, fruits_spawned;
static int popup_timer, popup_value, popup_x, popup_y;
static int first_start;
static int autoplay;

static Turn turns[MAX_PLAYERS];
static int num_players = 1, cur_player;
static uint32_t game_seed;
static Settings settings;
static int option_sel;
static int start_level_no = 1;

static const int MODE_SECONDS[] = {7, 20, 7, 20, 5, 20, 5, -1};
static const int FRIGHT_SECONDS[] = {6, 5, 4, 3, 2, 5, 2, 2, 1, 5, 2, 1, 1, 3, 1, 1, 0, 1};
static const int FRUIT_POINTS[] = {100, 200, 500, 700, 1000, 2000, 5000};
#define FRUIT_KINDS 7

/* ---- small helpers ---- */
static int imod(int a, int m) { return ((a % m) + m) % m; }
static int wrapx(int x) { return imod(x, WORLD_W); }

static char tile(int tx, int ty)
{
    if (ty < 0 || ty >= MH)
        return '#';
    return grid[ty][imod(tx, MW)];
}

static int is_wall(char c) { return c == '#'; }

/* The built-in mazes come first, then every level gets a freshly generated one. */
static int scheme_index(void) { return (level - 1) % SCHEME_COUNT; }

/* Distance (sub-pixels) from coordinate c to the next tile centre in direction sign. */
static int dist_to_center(int c, int sign)
{
    int off = imod(c - TU / 2, TU);
    if (off == 0)
        return 0;
    return sign > 0 ? TU - off : off;
}

/* ---- drawing primitives ---- */
static void pset(int x, int y, uint32_t c)
{
    if ((unsigned)x < SCR_W && (unsigned)y < SCR_H)
        fb[y * SCR_W + x] = c;
}

static void fill_rect(int x, int y, int w, int h, uint32_t c)
{
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++)
            pset(i, j, c);
}

static void disc(float cx, float cy, float r, uint32_t c)
{
    for (int y = (int)floorf(cy - r); y <= (int)ceilf(cy + r); y++)
        for (int x = (int)floorf(cx - r); x <= (int)ceilf(cx + r); x++) {
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            if (dx * dx + dy * dy <= r * r)
                pset(x, y, c);
        }
}

static void draw_char(int x, int y, char ch, uint32_t c, int scale)
{
    const uint8_t *g = font_glyph(ch);
    if (!g)
        return;
    for (int r = 0; r < 7; r++)
        for (int b = 0; b < 5; b++)
            if (g[r] & (0x10 >> b))
                fill_rect(x + b * scale, y + r * scale, scale, scale, c);
}

static void draw_text(int x, int y, const char *s, uint32_t c, int scale)
{
    for (; *s; s++, x += 6 * scale)
        draw_char(x, y, *s, c, scale);
}

/* Text placed on the 8x8 tile grid of the whole screen. */
static void text_at(int col, int row, const char *s, uint32_t c)
{
    draw_text(col * TILE + 1, row * TILE, s, c, 1);
}

static void text_center(int y, const char *s, uint32_t c, int scale)
{
    int w = (int)strlen(s) * 6 * scale - scale;
    draw_text((SCR_W - w) / 2, y, s, c, scale);
}

/* ---- sprites ---- */
static void draw_player_at(float cx, float cy, int dir, float mouth_deg, int bow)
{
    const float R = 6.5f;
    float fdx = (float)DX[dir], fdy = (float)DY[dir];
    float half = mouth_deg * (float)M_PI / 360.0f;
    for (int y = (int)floorf(cy - R); y <= (int)ceilf(cy + R); y++)
        for (int x = (int)floorf(cx - R); x <= (int)ceilf(cx + R); x++) {
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            if (dx * dx + dy * dy > R * R)
                continue;
            float dot = dx * fdx + dy * fdy;
            float crs = dx * fdy - dy * fdx;
            float a = fabsf(atan2f(crs, dot));
            if (half > 0.0f && a < half)
                continue;
            pset(x, y, C_YELLOW);
        }
    if (!bow || mouth_deg >= 300.0f)
        return;
    /* A red bow on top and an eye, mirrored when facing left. */
    float side = dir == LEFT ? 1.0f : -1.0f;
    float bx = cx + side * 3.0f, by = cy - 6.0f;
    disc(bx - 2.2f, by, 2.0f, C_RED);
    disc(bx + 2.2f, by, 2.0f, C_RED);
    disc(bx, by, 1.0f, C_BLUE);
    if (dir == LEFT || dir == RIGHT)
        fill_rect((int)(cx - side * 1.0f) - 1, (int)cy - 4, 2, 2, C_BLACK);
}

static void draw_ghost_at(float cx, float cy, uint32_t color, int dir, int mode, int anim)
{
    /* mode: 0 normal, 1 frightened blue, 2 frightened flashing white, 3 eyes only */
    int x0 = (int)floorf(cx) - 7, y0 = (int)floorf(cy) - 7;
    uint32_t body = mode == 1 ? C_BLUE : mode == 2 ? C_WHITE : color;
    if (mode != 3) {
        disc(cx, cy - 0.5f, 6.8f, body);
        fill_rect(x0, (int)floorf(cy), 14, 5, body);
        for (int i = 0; i < 14; i++)
            if (((i + anim * 2) & 3) < 2)
                fill_rect(x0 + i, (int)floorf(cy) + 5, 1, 2, body);
            else
                pset(x0 + i, (int)floorf(cy) + 5, body);
    }
    if (mode == 1 || mode == 2) {
        uint32_t face = mode == 1 ? C_DOT : C_RED;
        fill_rect(x0 + 4, y0 + 5, 2, 2, face);
        fill_rect(x0 + 8, y0 + 5, 2, 2, face);
        for (int i = 0; i < 10; i++)
            pset(x0 + 2 + i, y0 + 10 + ((i / 2) & 1), face);
        return;
    }
    int ex = dir == NODIR ? 0 : DX[dir], ey = dir == NODIR ? 0 : DY[dir];
    for (int s = 0; s < 2; s++) {
        int wx = x0 + 2 + s * 6 + ex, wy = y0 + 3 + ey;
        fill_rect(wx, wy + 1, 4, 3, C_WHITE);
        fill_rect(wx + 1, wy, 2, 5, C_WHITE);
        fill_rect(wx + 1 + ex, wy + 2 + ey, 2, 2, C_BLUE);
    }
}

static void draw_fruit_at(float cx, float cy, int kind)
{
    static const uint32_t col[FRUIT_KINDS] = {
        RGB(230, 20, 40),  RGB(255, 60, 90),  RGB(255, 150, 20), RGB(200, 140, 60),
        RGB(220, 30, 30),  RGB(170, 220, 60), RGB(255, 225, 60),
    };
    uint32_t leaf = RGB(40, 200, 60);
    switch (kind) {
    case 0: /* twin berries */
        fill_rect((int)cx, (int)cy - 6, 1, 4, leaf);
        pset((int)cx - 1, (int)cy - 3, leaf);
        pset((int)cx + 1, (int)cy - 3, leaf);
        disc(cx - 2.5f, cy + 1.5f, 2.8f, col[0]);
        disc(cx + 2.5f, cy + 2.5f, 2.8f, col[0]);
        pset((int)cx - 3, (int)cy, C_WHITE);
        break;
    case 3: /* ring cookie */
        disc(cx, cy, 5.5f, col[3]);
        disc(cx, cy, 2.2f, C_BLACK);
        break;
    case 6: /* star fruit */
        for (int i = 0; i < 5; i++) {
            float a = -1.5708f + i * 1.2566f;
            disc(cx + cosf(a) * 3.2f, cy + sinf(a) * 3.2f, 2.0f, col[6]);
        }
        disc(cx, cy, 2.6f, col[6]);
        break;
    default:
        disc(cx, cy + 1.0f, 5.0f, col[kind]);
        fill_rect((int)cx - 1, (int)cy - 6, 3, 2, leaf);
        if (kind == 1)
            for (int i = 0; i < 4; i++)
                pset((int)cx - 2 + (i % 2) * 3, (int)cy - 1 + (i / 2) * 3, C_DOT);
        pset((int)cx - 2, (int)cy - 1, C_WHITE);
        break;
    }
}

/* ---- maze ---- */
static void render_maze_bg(void)
{
    uint32_t fill = SCHEMES[scheme_index()][0], edge = SCHEMES[scheme_index()][1];
    for (int py = 0; py < MH * TILE; py++)
        for (int px = 0; px < SCR_W; px++) {
            uint32_t c0 = C_BLACK, c1 = C_BLACK;
            if (is_wall(grid[py / TILE][px / TILE])) {
                int best = 99;
                for (int dy = -4; dy <= 4; dy++)
                    for (int dx = -4; dx <= 4; dx++) {
                        int qx = px + dx, qy = py + dy;
                        int qtx = qx < 0 ? -1 : qx / TILE;
                        int qty = qy < 0 ? -1 : qy / TILE;
                        if (!is_wall(tile(qtx, qty)) && dx * dx + dy * dy < best)
                            best = dx * dx + dy * dy;
                    }
                if (best > 9) {
                    c0 = fill;
                } else if (best > 4) {
                    c0 = edge;
                    c1 = C_WHITE;
                }
            }
            bg[0][py * SCR_W + px] = c0;
            bg[1][py * SCR_W + px] = c1;
        }
}

static void load_maze(void)
{
    char gen[MH][MW / 2 + 1];
    const char *half[MH];
    if (level <= MAZE_COUNT) {
        for (int y = 0; y < MH; y++)
            half[y] = MAZES[level - 1][y];
    } else {
        /* Same seed for every player, so all of them face the same boards. */
        mazegen_build(game_seed + (uint32_t)level * 7919u, gen);
        for (int y = 0; y < MH; y++)
            half[y] = gen[y];
    }
    dots_total = 0;
    for (int y = 0; y < MH; y++)
        for (int x = 0; x < MW / 2; x++) {
            grid[y][x] = half[y][x];
            grid[y][MW - 1 - x] = half[y][x];
        }
    /* The start position is kept clear of dots. */
    grid[23][13] = grid[23][14] = ' ';
    for (int y = 0; y < MH; y++)
        for (int x = 0; x < MW; x++)
            if (grid[y][x] == '.' || grid[y][x] == 'o')
                dots_total++;
    dots_left = dots_total;
    dots_eaten = 0;
    render_maze_bg();
}

/* ---- high score and settings persistence ---- */
/* $XDG_<var>/mskman, falling back to ~/<fallback>/mskman. */
static void user_dir(char *buf, size_t n, const char *var, const char *fallback)
{
    const char *xdg = getenv(var);
    const char *home = getenv("HOME");
    if (xdg && *xdg)
        snprintf(buf, n, "%s/mskman", xdg);
    else if (home && *home)
        snprintf(buf, n, "%s/%s/mskman", home, fallback);
    else
        buf[0] = 0;
}

static void hiscore_path(char *buf, size_t n) { user_dir(buf, n, "XDG_DATA_HOME", ".local/share"); }
static void config_path(char *buf, size_t n) { user_dir(buf, n, "XDG_CONFIG_HOME", ".config"); }

/* Creates the directory chain (e.g. ~/.local/share/mskman). */
static int make_dirs(char *dir)
{
    for (char *p = dir + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            mkdir(dir, 0755);
            *p = '/';
        }
    return mkdir(dir, 0755) == 0 || errno == EEXIST;
}

static void load_hiscore(void)
{
    char dir[1024], path[1100];
    hiscore_path(dir, sizeof dir);
    if (!dir[0])
        return;
    snprintf(path, sizeof path, "%s/highscore", dir);
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    if (fscanf(f, "%d", &hiscore) != 1)
        hiscore = 0;
    fclose(f);
}

static void save_hiscore(void)
{
    char dir[1024], path[1100];
    hiscore_path(dir, sizeof dir);
    if (!dir[0] || autoplay || !make_dirs(dir))
        return;
    snprintf(path, sizeof path, "%s/highscore", dir);
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    fprintf(f, "%d\n", hiscore);
    fclose(f);
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void load_settings(void)
{
    char dir[1024], path[1100], line[128];
    settings = DEFAULT_SETTINGS;
    config_path(dir, sizeof dir);
    if (!dir[0])
        return;
    snprintf(path, sizeof path, "%s/settings.conf", dir);
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    while (fgets(line, sizeof line, f)) {
        char key[64];
        int v;
        if (sscanf(line, " %63[a-z_] = %d", key, &v) != 2)
            continue;
        if (!strcmp(key, "corner_assist"))
            settings.corner_assist = clampi(v, 0, ASSIST_MAX);
        else if (!strcmp(key, "player_speed"))
            settings.player_speed = clampi(v, SPEED_MIN, SPEED_MAX);
        else if (!strcmp(key, "ghost_speed"))
            settings.ghost_speed = clampi(v, SPEED_MIN, SPEED_MAX);
    }
    fclose(f);
}

static void save_settings(void)
{
    char dir[1024], path[1100];
    config_path(dir, sizeof dir);
    if (!dir[0] || autoplay || !make_dirs(dir))
        return;
    snprintf(path, sizeof path, "%s/settings.conf", dir);
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    fprintf(f, "# MSKman settings (also editable from the Options screen)\n"
               "# corner_assist: pixels a turn is accepted before/after a junction (0-%d)\n"
               "corner_assist = %d\n"
               "# speeds in percent of normal (%d-%d)\n"
               "player_speed = %d\n"
               "ghost_speed = %d\n",
            ASSIST_MAX, settings.corner_assist, SPEED_MIN, SPEED_MAX, settings.player_speed,
            settings.ghost_speed);
    fclose(f);
}

/* ---- speeds (sub-pixels per frame; 20 = full speed) ---- */
static int tier(void) { return level == 1 ? 0 : level < 5 ? 1 : 2; }

static int scale_speed(int s, int percent)
{
    s = (s * percent + 50) / 100;
    return s < 1 ? 1 : s;
}

static int player_speed(void)
{
    static const int normal[] = {16, 18, 20}, fright[] = {18, 19, 20};
    return scale_speed(fright_timer > 0 ? fright[tier()] : normal[tier()], settings.player_speed);
}

static int in_tunnel(const Ghost *g)
{
    int tx = g->x / TU, ty = g->y / TU;
    return !is_wall(tile(0, ty)) && (tx <= 5 || tx >= MW - 6);
}

static int ghost_speed(const Ghost *g)
{
    static const int normal[] = {15, 17, 19}, fright[] = {10, 11, 12}, tunnel[] = {8, 9, 10};
    switch (g->st) {
    case G_EYES:
    case G_ENTERING:
        return 32;
    case G_HOUSE:
    case G_LEAVING:
        return 8;
    default:
        break;
    }
    int s;
    if (in_tunnel(g)) {
        s = tunnel[tier()];
    } else if (g->fright) {
        s = fright[tier()];
    } else {
        s = normal[tier()];
        if (g == &gh[0] && dots_left <= 20 + 10 * tier())
            s += dots_left <= 10 + 5 * tier() ? 3 : 1;
    }
    return scale_speed(s, settings.ghost_speed);
}

/* ---- movement ---- */
/* Scripted straight-line move toward a target; returns 1 on arrival. */
static int approach(int *v, int target, int speed)
{
    if (*v < target)
        *v = *v + speed > target ? target : *v + speed;
    else if (*v > target)
        *v = *v - speed < target ? target : *v - speed;
    return *v == target;
}

typedef int (*CenterFn)(void *self);

/* Move an actor `speed` sub-pixels, calling fn at every tile centre reached.
 * fn may change *dir; returning 0 stops movement for this frame. */
static void advance(int *x, int *y, int *dir, int speed, CenterFn fn, void *self)
{
    while (speed > 0 && *dir != NODIR) {
        int horiz = *dir == LEFT || *dir == RIGHT;
        int sign = (*dir == RIGHT || *dir == DOWN) ? 1 : -1;
        int dc = dist_to_center(horiz ? *x : *y, sign);
        if (dc == 0) {
            if (!fn(self))
                return;
            dc = TU;
        }
        int s = speed < dc ? speed : dc;
        *x = wrapx(*x + DX[*dir] * s);
        *y += DY[*dir] * s;
        speed -= s;
    }
}

static int player_open(int tx, int ty, int d)
{
    char c = tile(tx + DX[d], ty + DY[d]);
    return !is_wall(c) && c != '-';
}

static int player_center(void *self)
{
    (void)self;
    int tx = pl.x / TU, ty = pl.y / TU;
    if (autoplay) {
        /* Demo mode wanders: any open way except straight back. */
        int opts[4], n = 0;
        for (int d = 0; d < 4; d++)
            if (d != OPP(pl.dir) && player_open(tx, ty, d))
                opts[n++] = d;
        pl.want = n ? opts[rand() % n] : OPP(pl.dir);
    }
    if (pl.want != NODIR && player_open(tx, ty, pl.want))
        pl.dir = pl.want;
    pl.moving = player_open(tx, ty, pl.dir);
    return pl.moving;
}

/* Corner assist: a turn pressed a few pixels before or after a junction is
 * taken at once; centre_player() then slides the player onto the new lane. */
static void corner_assist(void)
{
    int d = pl.want;
    if (settings.corner_assist <= 0 || d == NODIR || d == pl.dir || d == OPP(pl.dir) || autoplay)
        return;
    int horiz = pl.dir == LEFT || pl.dir == RIGHT;
    int c = horiz ? pl.x : pl.y;
    int off = c - (c / TU * TU + TU / 2);
    if (off == 0 || abs(off) > settings.corner_assist * SUB)
        return;
    if (player_open(pl.x / TU, pl.y / TU, d)) {
        pl.dir = d;
        pl.moving = 1;
    }
}

static void centre_player(int speed)
{
    int *c = pl.dir == LEFT || pl.dir == RIGHT ? &pl.y : &pl.x;
    approach(c, *c / TU * TU + TU / 2, speed);
}

static void ghost_target(const Ghost *g, int *tx, int *ty)
{
    int ptx = pl.x / TU, pty = pl.y / TU;
    if (g->st == G_EYES) {
        *tx = 13;
        *ty = 11;
        return;
    }
    if (mode_idx % 2 == 0) {
        *tx = g->scatter_tx;
        *ty = g->scatter_ty;
        return;
    }
    switch (g - gh) {
    case 0: /* chases directly */
        *tx = ptx;
        *ty = pty;
        break;
    case 1: /* aims ahead of the player */
        *tx = ptx + DX[pl.dir] * 4;
        *ty = pty + DY[pl.dir] * 4;
        break;
    case 2: { /* flanks, using the red ghost as a pivot */
        int ax = ptx + DX[pl.dir] * 2, ay = pty + DY[pl.dir] * 2;
        *tx = 2 * ax - gh[0].x / TU;
        *ty = 2 * ay - gh[0].y / TU;
        break;
    }
    default: { /* chases from afar, retreats when close */
        int dx = g->x / TU - ptx, dy = g->y / TU - pty;
        if (dx * dx + dy * dy > 64) {
            *tx = ptx;
            *ty = pty;
        } else {
            *tx = g->scatter_tx;
            *ty = g->scatter_ty;
        }
    }
    }
}

static int ghost_center(void *self)
{
    Ghost *g = self;
    int tx = g->x / TU, ty = g->y / TU;
    if (g->st == G_EYES && ty == 11 && (tx == 13 || tx == 14)) {
        g->st = G_ENTERING;
        return 0;
    }
    int opts[4], n = 0;
    for (int d = 0; d < 4; d++) {
        if (d == OPP(g->dir))
            continue;
        char c = tile(tx + DX[d], ty + DY[d]);
        if (!is_wall(c) && c != '-')
            opts[n++] = d;
    }
    if (n == 0) {
        g->dir = OPP(g->dir);
        return 1;
    }
    if (g->fright && g->st == G_ACTIVE) {
        g->dir = opts[rand() % n];
        return 1;
    }
    int gx, gy, best = opts[0], bestd = 1 << 30;
    ghost_target(g, &gx, &gy);
    for (int i = 0; i < n; i++) {
        int d = opts[i];
        int dx = tx + DX[d] - gx, dy = ty + DY[d] - gy;
        if (dx * dx + dy * dy < bestd) {
            bestd = dx * dx + dy * dy;
            best = d;
        }
    }
    g->dir = best;
    return 1;
}

static void update_ghost(Ghost *g)
{
    int sp = ghost_speed(g);
    switch (g->st) {
    case G_HOUSE:
        if (g->dir != UP && g->dir != DOWN)
            g->dir = UP;
        if (approach(&g->y, g->dir == UP ? HOUSE_Y - 3 * SUB : HOUSE_Y + 3 * SUB, sp))
            g->dir = OPP(g->dir);
        break;
    case G_LEAVING:
        if (g->x != EXIT_X) {
            if (approach(&g->y, HOUSE_Y, sp)) {
                g->dir = g->x < EXIT_X ? RIGHT : LEFT;
                approach(&g->x, EXIT_X, sp);
            }
        } else {
            g->dir = UP;
            if (approach(&g->y, EXIT_Y, sp)) {
                g->st = G_ACTIVE;
                g->dir = LEFT;
                g->reverse = 0;
            }
        }
        break;
    case G_ENTERING:
        if (g->y == EXIT_Y && g->x != EXIT_X) {
            g->dir = g->x < EXIT_X ? RIGHT : LEFT;
            approach(&g->x, EXIT_X, sp);
        } else {
            g->dir = DOWN;
            if (approach(&g->y, HOUSE_Y, sp)) {
                g->fright = 0;
                g->st = G_LEAVING;
            }
        }
        break;
    case G_ACTIVE:
    case G_EYES:
        if (g->reverse) {
            g->reverse = 0;
            g->dir = OPP(g->dir);
        }
        advance(&g->x, &g->y, &g->dir, sp, ghost_center, g);
        break;
    }
}

/* ---- game flow ---- */
static void set_state(GameState s, int timer)
{
    state = s;
    state_timer = timer;
}

static void reset_actors(void)
{
    static const char *names[4] = {"BLAZE", "PIXIE", "SKYE", "SUNNY"};
    static const uint32_t *colors[4] = {&C_RED, &C_PINK, &C_CYAN, &C_ORANGE};
    static const int scatter[4][2] = {{25, -3}, {2, -3}, {27, 31}, {0, 31}};
    static const int home[4] = {EXIT_X, EXIT_X, 12 * TU, 16 * TU};

    pl.x = 14 * TU;
    pl.y = START_Y;
    pl.dir = LEFT;
    pl.want = NODIR;
    pl.moving = 1;
    pl.stall = 0;
    pl.anim = 0;

    for (int i = 0; i < 4; i++) {
        Ghost *g = &gh[i];
        g->name = names[i];
        g->color = *colors[i];
        g->scatter_tx = scatter[i][0];
        g->scatter_ty = scatter[i][1];
        g->home_x = home[i];
        g->x = home[i];
        g->y = i == 0 ? EXIT_Y : HOUSE_Y;
        g->st = i == 0 ? G_ACTIVE : G_HOUSE;
        g->dir = i == 0 ? LEFT : (i == 1 ? DOWN : UP);
        g->fright = 0;
        g->reverse = 0;
    }
    /* Dot counts that release each ghost from the house. */
    int lim2 = level == 1 ? 30 : 0, lim3 = level == 1 ? 60 : level == 2 ? 50 : 0;
    gh[1].dot_limit = 0;
    gh[2].dot_limit = lost_life_this_level ? 17 : lim2;
    gh[3].dot_limit = lost_life_this_level ? 32 : lim3;

    mode_idx = 0;
    mode_timer = 0;
    fright_timer = 0;
    life_dots = 0;
    idle_timer = 0;
    fruit_timer = 0;
    popup_timer = 0;
}

static void start_level(void)
{
    load_maze();
    lost_life_this_level = 0;
    fruits_spawned = 0;
    reset_actors();
    if (first_start) {
        first_start = 0;
        audio_play(SND_START);
        set_state(ST_READY, (int)(4.2 * FPS));
    } else {
        set_state(ST_READY, 2 * FPS);
    }
}

/* Copy the active player's progress to and from their turn record. */
static void save_turn(int p)
{
    Turn *t = &turns[p];
    t->score = score;
    t->lives = lives;
    t->level = level;
    t->extra_given = extra_given;
    t->lost_life_this_level = lost_life_this_level;
    t->dots_left = dots_left;
    t->dots_total = dots_total;
    t->dots_eaten = dots_eaten;
    t->fruits_spawned = fruits_spawned;
    memcpy(t->grid, grid, sizeof grid);
}

static void load_turn(int p)
{
    const Turn *t = &turns[p];
    cur_player = p;
    score = t->score;
    lives = t->lives;
    level = t->level;
    extra_given = t->extra_given;
    lost_life_this_level = t->lost_life_this_level;
    dots_left = t->dots_left;
    dots_total = t->dots_total;
    dots_eaten = t->dots_eaten;
    fruits_spawned = t->fruits_spawned;
    memcpy(grid, t->grid, sizeof grid);
    render_maze_bg();
}

static void start_game(void)
{
    game_seed = (uint32_t)rand() ^ ((uint32_t)rand() << 16);
    paused = 0;
    audio_stop_all();
    for (int p = num_players - 1; p >= 0; p--) {
        score = 0;
        lives = 3;
        level = start_level_no;
        extra_given = 0;
        lost_life_this_level = 0;
        fruits_spawned = 0;
        load_maze();
        save_turn(p);
    }
    cur_player = 0;
    first_start = 1;
    start_level();
}

/* After a lost life: pick whose turn is next, or end the game. */
static void next_turn(void)
{
    save_turn(cur_player);
    int next = -1;
    for (int i = 1; i <= num_players; i++) {
        int p = (cur_player + i) % num_players;
        if (turns[p].lives > 0) {
            next = p;
            break;
        }
    }
    if (next < 0) {
        save_hiscore();
        set_state(ST_GAME_OVER, 3 * FPS);
        return;
    }
    if (next != cur_player)
        load_turn(next);
    reset_actors();
    set_state(ST_READY, (num_players > 1 ? 3 : 2) * FPS);
}

static void add_score(int pts)
{
    score += pts;
    if (!extra_given && score >= 10000) {
        extra_given = 1;
        lives++;
        audio_play(SND_EXTRA);
    }
    if (score > hiscore)
        hiscore = score;
}

static void begin_fright(void)
{
    int idx = level - 1 < (int)(sizeof FRIGHT_SECONDS / sizeof *FRIGHT_SECONDS) ? level - 1 : -1;
    int secs = idx >= 0 ? FRIGHT_SECONDS[idx] : 0;
    ghosts_eaten = 0;
    for (int i = 0; i < 4; i++) {
        Ghost *g = &gh[i];
        if (g->st == G_ACTIVE)
            g->reverse = 1;
        if (secs > 0 && g->st != G_EYES && g->st != G_ENTERING)
            g->fright = 1;
    }
    fright_len = fright_timer = secs * FPS;
}

static void end_fright(void)
{
    fright_timer = 0;
    for (int i = 0; i < 4; i++)
        gh[i].fright = 0;
}

static void eat_at_player(void)
{
    int tx = pl.x / TU, ty = pl.y / TU;
    if (tx >= MW)
        tx = MW - 1;
    char *c = &grid[ty][tx];
    if (*c != '.' && *c != 'o')
        return;
    int power = *c == 'o';
    *c = ' ';
    dots_left--;
    dots_eaten++;
    life_dots++;
    idle_timer = 0;
    add_score(power ? 50 : 10);
    pl.stall = power ? 3 : 1;
    audio_play(chomp_alt ? SND_CHOMP_A : SND_CHOMP_B);
    chomp_alt = !chomp_alt;
    if (power)
        begin_fright();
    if ((dots_eaten == 64 || dots_eaten == 176) && fruits_spawned < 2) {
        fruits_spawned++;
        fruit_kind = level - 1 < FRUIT_KINDS ? level - 1 : rand() % FRUIT_KINDS;
        fruit_timer = 9 * FPS + rand() % FPS;
    }
}

static void release_ghosts(void)
{
    for (int i = 1; i < 4; i++) {
        Ghost *g = &gh[i];
        if (g->st != G_HOUSE)
            continue;
        /* Only the first waiting ghost is considered, in priority order. */
        if (life_dots >= g->dot_limit || idle_timer >= 4 * FPS) {
            g->st = G_LEAVING;
            idle_timer = 0;
        }
        return;
    }
}

static int near_player(int x, int y, int r)
{
    int dx = abs(x - pl.x), dy = abs(y - pl.y);
    if (dx > WORLD_W / 2)
        dx = WORLD_W - dx;
    return dx < r * SUB && dy < r * SUB;
}

static void check_collisions(void)
{
    for (int i = 0; i < 4; i++) {
        Ghost *g = &gh[i];
        if (g->st != G_ACTIVE || !near_player(g->x, g->y, 6))
            continue;
        if (g->fright) {
            g->fright = 0;
            g->st = G_EYES;
            popup_value = 200 << ghosts_eaten;
            if (ghosts_eaten < 3)
                ghosts_eaten++;
            add_score(popup_value);
            popup_x = g->x;
            popup_y = g->y;
            audio_play(SND_EAT_GHOST);
            set_state(ST_GHOST_EATEN, FPS);
            return;
        }
        audio_loop(SND_SIREN, 0);
        audio_loop(SND_POWER, 0);
        set_state(ST_DYING, (int)(2.6 * FPS));
        return;
    }
}

static void update_play(void)
{
    /* Ghost behaviour mode schedule pauses while ghosts are frightened. */
    if (fright_timer > 0) {
        if (--fright_timer == 0)
            end_fright();
    } else if (MODE_SECONDS[mode_idx] >= 0 && ++mode_timer >= MODE_SECONDS[mode_idx] * FPS) {
        mode_idx++;
        mode_timer = 0;
        for (int i = 0; i < 4; i++)
            if (gh[i].st == G_ACTIVE)
                gh[i].reverse = 1;
    }

    if (pl.want != NODIR && pl.want == OPP(pl.dir))
        pl.dir = pl.want;
    if (pl.stall > 0) {
        pl.stall--;
    } else {
        int sp = player_speed();
        corner_assist();
        advance(&pl.x, &pl.y, &pl.dir, sp, player_center, NULL);
        centre_player(sp);
        if (pl.moving)
            pl.anim++;
    }
    eat_at_player();
    check_collisions();
    if (state != ST_PLAY)
        return;

    idle_timer++;
    release_ghosts();
    for (int i = 0; i < 4; i++)
        update_ghost(&gh[i]);
    check_collisions();
    if (state != ST_PLAY)
        return;

    if (fruit_timer > 0) {
        fruit_timer--;
        if (near_player(EXIT_X, FRUIT_Y, 6)) {
            int pts = FRUIT_POINTS[fruit_kind];
            add_score(pts);
            fruit_timer = 0;
            popup_value = pts;
            popup_x = EXIT_X;
            popup_y = FRUIT_Y;
            popup_timer = 2 * FPS;
            audio_play(SND_FRUIT);
        }
    }
    if (popup_timer > 0)
        popup_timer--;

    int eyes = 0;
    for (int i = 0; i < 4; i++)
        eyes |= gh[i].st == G_EYES || gh[i].st == G_ENTERING;
    audio_loop(SND_POWER, fright_timer > 0 || eyes);
    audio_loop(SND_SIREN, !(fright_timer > 0 || eyes));
    audio_set_siren_speed(1.0f + 1.2f * (1.0f - (float)dots_left / (float)dots_total));

    if (dots_left == 0) {
        audio_loop(SND_SIREN, 0);
        audio_loop(SND_POWER, 0);
        set_state(ST_CLEAR, (int)(2.8 * FPS));
    }
}

static void update(void)
{
    frame++;
    if (paused)
        return;
    switch (state) {
    case ST_TITLE:
        if (autoplay)
            start_game();
        break;
    case ST_OPTIONS:
        break;
    case ST_READY:
        if (--state_timer <= 0)
            set_state(ST_PLAY, 0);
        break;
    case ST_PLAY:
        update_play();
        break;
    case ST_GHOST_EATEN:
        if (--state_timer <= 0) {
            set_state(ST_PLAY, 0);
            popup_timer = 0;
        }
        break;
    case ST_DYING:
        if (state_timer == (int)(1.6 * FPS))
            audio_play(SND_DEATH);
        if (--state_timer <= 0) {
            lives--;
            lost_life_this_level = 1;
            if (lives <= 0 && num_players > 1)
                set_state(ST_PLAYER_OUT, 2 * FPS);
            else
                next_turn();
        }
        break;
    case ST_PLAYER_OUT:
        if (--state_timer <= 0)
            next_turn();
        break;
    case ST_CLEAR:
        if (--state_timer <= 0) {
            level++;
            start_level();
        }
        break;
    case ST_GAME_OVER:
        if (--state_timer <= 0)
            set_state(ST_TITLE, 0);
        break;
    }
}

/* ---- rendering ---- */
static void draw_maze(void)
{
    int flash = state == ST_CLEAR && state_timer < 2 * FPS && (state_timer / 12) % 2 == 0;
    memcpy(fb + MAZE_Y * SCR_W, bg[flash], sizeof bg[0]);
    if (!flash)
        for (int x = 13 * TILE; x < 15 * TILE; x++)
            fill_rect(x, MAZE_Y + 12 * TILE + 4, 1, 2, C_DOOR);
    int blink = state == ST_PLAY && (frame / 10) % 2;
    for (int y = 0; y < MH; y++)
        for (int x = 0; x < MW; x++) {
            int px = x * TILE, py = MAZE_Y + y * TILE;
            if (grid[y][x] == '.')
                fill_rect(px + 3, py + 3, 2, 2, C_DOT);
            else if (grid[y][x] == 'o' && !blink)
                disc(px + 4.0f, py + 4.0f, 3.6f, C_DOT);
        }
}

static void sprite_pos(int x, int y, float *px, float *py)
{
    *px = (float)x / SUB;
    *py = (float)y / SUB + MAZE_Y;
}

static float mouth_angle(void)
{
    static const float open[4] = {6.0f, 50.0f, 90.0f, 50.0f};
    return pl.moving ? open[(pl.anim / 3) % 4] : 50.0f;
}

static void draw_player(void)
{
    float px, py;
    sprite_pos(pl.x, pl.y, &px, &py);
    float mouth = mouth_angle();
    int dir = pl.dir;
    if (state == ST_DYING && state_timer < (int)(1.6 * FPS)) {
        /* Spin, then fold away. */
        int t = (int)(1.6 * FPS) - state_timer;
        if (t < 40) {
            dir = (pl.dir + t / 5) % 4;
            mouth = 40.0f;
        } else {
            dir = UP;
            mouth = 40.0f + (t - 40) * 7.0f;
            if (mouth >= 360.0f)
                return;
        }
    }
    draw_player_at(px, py, dir, mouth, 1);
    if (px < 8)
        draw_player_at(px + SCR_W, py, dir, mouth, 1);
    if (px > SCR_W - 8)
        draw_player_at(px - SCR_W, py, dir, mouth, 1);
}

static void draw_ghosts(void)
{
    int anim = (frame / 8) % 2;
    for (int i = 3; i >= 0; i--) {
        Ghost *g = &gh[i];
        float px, py;
        sprite_pos(g->x, g->y, &px, &py);
        if (state == ST_GHOST_EATEN && g->x == popup_x && g->y == popup_y)
            continue;
        int mode = 0;
        if (g->st == G_EYES || g->st == G_ENTERING) {
            mode = 3;
        } else if (g->fright) {
            mode = 1;
            if (fright_timer < 2 * FPS && (fright_timer / 12) % 2 == 0)
                mode = 2;
        }
        draw_ghost_at(px, py, g->color, g->dir, mode, anim);
    }
}

static void draw_popup(int value, int x, int y, uint32_t c)
{
    char buf[24];
    float px, py;
    snprintf(buf, sizeof buf, "%d", value);
    sprite_pos(x, y, &px, &py);
    int w = (int)strlen(buf) * 6 - 1;
    draw_text((int)px - w / 2, (int)py - 3, buf, c, 1);
}

static int in_game(void) { return state != ST_TITLE && state != ST_OPTIONS; }

static void draw_hud(void)
{
    char buf[32];
    if (!in_game() || (frame / 16) % 2 || state != ST_PLAY) {
        snprintf(buf, sizeof buf, "%dUP", in_game() ? cur_player + 1 : 1);
        text_at(3, 0, buf, C_WHITE);
    }
    text_at(9, 0, "HIGH SCORE", C_WHITE);
    snprintf(buf, sizeof buf, "%7d", score);
    if (score == 0)
        snprintf(buf, sizeof buf, "%7s", "00");
    text_at(0, 1, buf, C_WHITE);
    if (hiscore > 0) {
        snprintf(buf, sizeof buf, "%7d", hiscore);
        text_at(10, 1, buf, C_WHITE);
    }

    if (!in_game())
        return;
    /* The other players' scores, top right; dimmed once they are out. */
    for (int i = 1, row = 0; i < num_players; i++, row++) {
        int p = (cur_player + i) % num_players;
        snprintf(buf, sizeof buf, "%dUP%6d", p + 1, turns[p].score);
        draw_text(SCR_W - 9 * 6, row * TILE, buf, turns[p].lives > 0 ? RGB(170, 170, 170) : RGB(110, 40, 40), 1);
    }
    int reserve = lives - 1;
    for (int i = 0; i < reserve && i < 5; i++)
        draw_player_at(20.0f + i * 16.0f, SCR_H - 12.0f, LEFT, 50.0f, 1);
    int first = level > 7 ? level - 7 : 1;
    for (int l = level, i = 0; l >= first; l--, i++) {
        int kind = l - 1 < FRUIT_KINDS ? l - 1 : (l * 3) % FRUIT_KINDS;
        draw_fruit_at(SCR_W - 20.0f - i * 16.0f, SCR_H - 12.0f, kind);
    }
}

static void draw_title(void)
{
    text_center(40, "MSKMAN", C_YELLOW, 4);
    text_center(76, "A MAZE CHASE GAME", C_PINK, 1);

    for (int i = 0; i < 4; i++) {
        uint32_t c = gh[i].color;
        int y = 104 + i * 22;
        draw_ghost_at(52.0f, y + 3.0f, c, RIGHT, 0, (frame / 8) % 2);
        draw_text(72, y, gh[i].name, c, 1);
    }
    static const float open[4] = {6.0f, 50.0f, 90.0f, 50.0f};
    draw_player_at(52.0f, 196.0f, RIGHT, open[(frame / 3) % 4], 1);
    draw_text(72, 193, "STARRING MSK", C_YELLOW, 1);

    char buf[32];
    snprintf(buf, sizeof buf, "< %d PLAYER%s >", num_players, num_players > 1 ? "S" : "");
    text_center(212, buf, C_YELLOW, 1);
    if ((frame / 30) % 2 == 0)
        text_center(226, "PRESS ENTER TO PLAY", C_WHITE, 1);
    text_center(242, "LEFT/RIGHT PLAYERS  O OPTIONS", C_CYAN, 1);
    text_center(254, "ARROWS/WASD MOVE  P PAUSE", C_CYAN, 1);
    text_center(266, "M MUTE  F11 FULLSCREEN", C_CYAN, 1);
    text_center(278, "ESC QUITS", C_CYAN, 1);
}

/* ---- options screen ---- */
enum { OPT_ASSIST, OPT_PLAYER_SPEED, OPT_GHOST_SPEED, OPT_DEFAULTS, OPT_BACK, OPT_COUNT };

static void option_change(int delta)
{
    switch (option_sel) {
    case OPT_ASSIST:
        settings.corner_assist = clampi(settings.corner_assist + delta, 0, ASSIST_MAX);
        break;
    case OPT_PLAYER_SPEED:
        settings.player_speed = clampi(settings.player_speed + delta * SPEED_STEP, SPEED_MIN, SPEED_MAX);
        break;
    case OPT_GHOST_SPEED:
        settings.ghost_speed = clampi(settings.ghost_speed + delta * SPEED_STEP, SPEED_MIN, SPEED_MAX);
        break;
    }
}

static void close_options(void)
{
    save_settings();
    set_state(ST_TITLE, 0);
}

static void option_activate(void)
{
    if (option_sel == OPT_DEFAULTS)
        settings = DEFAULT_SETTINGS;
    else if (option_sel == OPT_BACK)
        close_options();
}

static void draw_options(void)
{
    static const char *const help[OPT_COUNT] = {
        "TURN THIS MANY PIXELS EARLY",
        "HOW FAST MSK MOVES",
        "HOW FAST THE GHOSTS MOVE",
        "PRESS ENTER TO RESET",
        "PRESS ENTER OR ESC",
    };
    static const char *const help2[OPT_COUNT] = {
        "OR LATE AT A CORNER", "", "", "", "TO SAVE AND RETURN",
    };
    char val[OPT_COUNT][16];
    snprintf(val[OPT_ASSIST], 16, settings.corner_assist ? "%d PX" : "OFF", settings.corner_assist);
    snprintf(val[OPT_PLAYER_SPEED], 16, "%d%%", settings.player_speed);
    snprintf(val[OPT_GHOST_SPEED], 16, "%d%%", settings.ghost_speed);
    val[OPT_DEFAULTS][0] = val[OPT_BACK][0] = 0;
    static const char *const label[OPT_COUNT] = {
        "CORNER ASSIST", "MSK SPEED", "GHOST SPEED", "DEFAULTS", "BACK",
    };

    text_center(40, "OPTIONS", C_YELLOW, 2);
    for (int i = 0; i < OPT_COUNT; i++) {
        int y = 80 + i * 20 + (i >= OPT_DEFAULTS ? 10 : 0);
        uint32_t c = i == option_sel ? C_YELLOW : C_WHITE;
        if (i == option_sel)
            draw_text(20, y, ">", c, 1);
        draw_text(32, y, label[i], c, 1);
        if (val[i][0]) {
            char buf[96];
            snprintf(buf, sizeof buf, "< %s >", val[i]);
            draw_text(SCR_W - 20 - ((int)strlen(buf) * 6 - 1), y, buf, c, 1);
        }
    }
    text_center(206, help[option_sel], C_PINK, 1);
    text_center(218, help2[option_sel], C_PINK, 1);
    text_center(254, "UP/DOWN CHOOSE", C_CYAN, 1);
    text_center(266, "LEFT/RIGHT CHANGE", C_CYAN, 1);
}

static void render(void)
{
    for (int i = 0; i < SCR_W * SCR_H; i++)
        fb[i] = C_BLACK;
    draw_hud();
    if (state == ST_TITLE) {
        draw_title();
        return;
    }
    if (state == ST_OPTIONS) {
        draw_options();
        return;
    }
    draw_maze();
    if (fruit_timer > 0 && state == ST_PLAY)
        draw_fruit_at((float)EXIT_X / SUB, (float)FRUIT_Y / SUB + MAZE_Y, fruit_kind);
    if (popup_timer > 0)
        draw_popup(popup_value, popup_x, popup_y, C_PINK);

    int show_ghosts = state == ST_PLAY || state == ST_READY || state == ST_GHOST_EATEN ||
                      (state == ST_DYING && state_timer >= (int)(1.6 * FPS)) ||
                      (state == ST_CLEAR && state_timer > 2 * FPS);
    if (show_ghosts)
        draw_ghosts();
    if (state == ST_GHOST_EATEN)
        draw_popup(popup_value, popup_x, popup_y, C_CYAN);
    else if (state != ST_GAME_OVER && state != ST_PLAYER_OUT)
        draw_player();

    if (state == ST_READY)
        text_at(11, 3 + 17, "READY!", C_YELLOW);
    if (state == ST_GAME_OVER || state == ST_PLAYER_OUT)
        text_at(9, 3 + 17, "GAME  OVER", C_RED);
    if ((state == ST_READY && state_timer > FPS) || state == ST_PLAYER_OUT) {
        char buf[24];
        if (num_players > 1)
            snprintf(buf, sizeof buf, "PLAYER %d", cur_player + 1);
        else if (level > 1)
            snprintf(buf, sizeof buf, "LEVEL %d", level);
        else
            buf[0] = 0;
        text_center((3 + 11) * TILE, buf, num_players > 1 ? C_CYAN : C_WHITE, 1);
    }
    if (paused) {
        fill_rect(70, 132, 84, 20, C_BLACK);
        text_center(138, "PAUSED", C_YELLOW, 1);
    }
}

/* ---- input ---- */
static int key_dir(SDL_Keycode k)
{
    switch (k) {
    case SDLK_UP: case SDLK_w: case SDLK_k: return UP;
    case SDLK_LEFT: case SDLK_a: case SDLK_h: return LEFT;
    case SDLK_DOWN: case SDLK_s: case SDLK_j: return DOWN;
    case SDLK_RIGHT: case SDLK_d: case SDLK_l: return RIGHT;
    default: return NODIR;
    }
}

static int button_dir(int b)
{
    switch (b) {
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return UP;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return LEFT;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return DOWN;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return RIGHT;
    default: return NODIR;
    }
}

static void toggle_fullscreen(SDL_Window *win)
{
    Uint32 f = SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP;
    SDL_SetWindowFullscreen(win, f ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
}

static void press_start(void)
{
    if (state == ST_TITLE)
        start_game();
    else if (state == ST_OPTIONS)
        option_activate();
}

static void press_dir(int d)
{
    if (d == NODIR)
        return;
    if (state == ST_TITLE) {
        if (d == LEFT || d == RIGHT)
            num_players = clampi(num_players + (d == LEFT ? -1 : 1), 1, MAX_PLAYERS);
    } else if (state == ST_OPTIONS) {
        if (d == UP || d == DOWN)
            option_sel = imod(option_sel + (d == UP ? -1 : 1), OPT_COUNT);
        else
            option_change(d == LEFT ? -1 : 1);
    } else {
        pl.want = d;
    }
}

static void open_options(void)
{
    if (state == ST_TITLE) {
        option_sel = 0;
        set_state(ST_OPTIONS, 0);
    }
}

/* Esc (or B on a pad): back out one screen, quitting from the title. */
static int press_back(void)
{
    if (state == ST_TITLE)
        return 0;
    if (state == ST_OPTIONS) {
        close_options();
        return 1;
    }
    save_hiscore();
    audio_stop_all();
    paused = 0;
    set_state(ST_TITLE, 0);
    return 1;
}

static void toggle_pause(void)
{
    if (!in_game() || state == ST_GAME_OVER)
        return;
    paused = !paused;
    if (paused) {
        audio_loop(SND_SIREN, 0);
        audio_loop(SND_POWER, 0);
    }
}

/* ---- maze self-test ---- */
/* Checks the loaded maze: symmetric, no dead ends, every open tile reachable
 * from the start. Returns a description of the first problem, or NULL. */
static const char *maze_problem(void)
{
    static char msg[96];
    int seen[MH][MW] = {{0}}, stack[MH * MW], n = 0, open = 0, reached = 0;
    for (int y = 0; y < MH; y++)
        for (int x = 0; x < MW; x++) {
            if (grid[y][x] != grid[y][MW - 1 - x])
                return "not symmetric";
            if (is_wall(grid[y][x]) || grid[y][x] == '-' || (y >= 13 && y <= 15 && x >= 11 && x <= 16))
                continue;
            open++;
            int exits = 0;
            for (int d = 0; d < 4; d++)
                exits += player_open(x, y, d);
            if (exits < 2) {
                snprintf(msg, sizeof msg, "dead end at %d,%d", x, y);
                return msg;
            }
        }
    seen[23][13] = 1;
    stack[n++] = 23 * MW + 13;
    while (n > 0) {
        int y = stack[--n] / MW, x = stack[n] % MW;
        reached++;
        for (int d = 0; d < 4; d++) {
            int nx = imod(x + DX[d], MW), ny = y + DY[d];
            if (player_open(x, y, d) && !seen[ny][nx]) {
                seen[ny][nx] = 1;
                stack[n++] = ny * MW + nx;
            }
        }
    }
    if (reached != open) {
        snprintf(msg, sizeof msg, "%d of %d open tiles unreachable", open - reached, open);
        return msg;
    }
    if (tile(13, 11) != ' ' || tile(13, 17) != ' ' || tile(13, 12) != '-')
        return "ghost house surroundings changed";
    if (dots_total < 150)
        return "too few dots";
    return NULL;
}

static int check_mazes(int count)
{
    int bad = 0, min_dots = 1 << 30, max_dots = 0;
    for (int i = 0; i < MAZE_COUNT + count; i++) {
        game_seed = (uint32_t)(i / 4) * 104729u;
        level = i < MAZE_COUNT ? i + 1 : MAZE_COUNT + 1 + i % 4;
        load_maze();
        const char *why = maze_problem();
        if (why) {
            printf("maze %d (seed %u, level %d): %s\n", i, game_seed, level, why);
            bad++;
        }
        if (dots_total < min_dots)
            min_dots = dots_total;
        if (dots_total > max_dots)
            max_dots = dots_total;
    }
    printf("mskman: checked %d mazes, %d bad, %d-%d dots\n", MAZE_COUNT + count, bad, min_dots, max_dots);
    return bad ? 1 : 0;
}

static void usage(const char *argv0)
{
    printf("Usage: %s [options]\n"
           "  -f, --fullscreen   start in fullscreen\n"
           "  -s, --scale N      window scale factor (default 3)\n"
           "  -m, --mute         start with sound muted\n"
           "  -V, --version      print version and exit\n"
           "  -h, --help         show this help\n"
           "  -p, --players N    preselect 1-4 players (they take turns)\n"
           "      --autoplay     let the computer play (demo / smoke test)\n"
           "      --level N      start at level N (for testing)\n"
           "      --check-mazes N  verify the built-in and N generated mazes, then exit\n"
           "      --frames N     quit after N frames (for testing)\n"
           "      --screenshot F save the last frame to F as BMP\n",
           argv0);
}

int main(int argc, char **argv)
{
    int fullscreen = 0, scale = 3, sound = 1;
    long max_frames = -1;
    const char *shot = NULL;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-f") || !strcmp(a, "--fullscreen")) {
            fullscreen = 1;
        } else if ((!strcmp(a, "-s") || !strcmp(a, "--scale")) && i + 1 < argc) {
            scale = atoi(argv[++i]);
            if (scale < 1 || scale > 10)
                scale = 3;
        } else if (!strcmp(a, "-m") || !strcmp(a, "--mute")) {
            sound = 0;
        } else if ((!strcmp(a, "-p") || !strcmp(a, "--players")) && i + 1 < argc) {
            num_players = clampi(atoi(argv[++i]), 1, MAX_PLAYERS);
        } else if (!strcmp(a, "--level") && i + 1 < argc) {
            start_level_no = clampi(atoi(argv[++i]), 1, 255);
        } else if (!strcmp(a, "--check-mazes") && i + 1 < argc) {
            return check_mazes(atoi(argv[++i]));
        } else if (!strcmp(a, "--autoplay")) {
            autoplay = 1;
        } else if (!strcmp(a, "--frames") && i + 1 < argc) {
            max_frames = atol(argv[++i]);
        } else if (!strcmp(a, "--screenshot") && i + 1 < argc) {
            shot = argv[++i];
        } else if (!strcmp(a, "-V") || !strcmp(a, "--version")) {
            printf("mskman %s\n", VERSION);
            return 0;
        } else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "mskman: unknown option '%s'\n", a);
            usage(argv[0]);
            return 2;
        }
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "mskman: SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");

    SDL_Window *win = SDL_CreateWindow("MSKman", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       SCR_W * scale, SCR_H * scale,
                                       SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!win) {
        fprintf(stderr, "mskman: cannot create window: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_SetWindowMinimumSize(win, SCR_W, SCR_H);
    if (fullscreen)
        toggle_fullscreen(win);

    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren)
        ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    if (!ren) {
        fprintf(stderr, "mskman: cannot create renderer: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_RendererInfo info;
    int vsync = SDL_GetRendererInfo(ren, &info) == 0 && (info.flags & SDL_RENDERER_PRESENTVSYNC);
    SDL_RenderSetLogicalSize(ren, SCR_W, SCR_H);
    SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, SCR_W, SCR_H);
    if (!tex) {
        fprintf(stderr, "mskman: cannot create texture: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    audio_init(sound);
    srand((unsigned)SDL_GetTicks() ^ (unsigned)SDL_GetPerformanceCounter());
    load_hiscore();
    load_settings();
    level = 1;
    load_maze();
    reset_actors();
    set_state(ST_TITLE, 0);

    SDL_GameController *pad = NULL;
    const double step = 1.0 / FPS;
    double acc = 0.0;
    Uint64 prev = SDL_GetPerformanceCounter();
    int running = 1;
    long frames_run = 0;

    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_QUIT:
                running = 0;
                break;
            case SDL_KEYDOWN: {
                SDL_Keycode k = ev.key.keysym.sym;
                if (k == SDLK_ESCAPE) {
                    if (!press_back())
                        running = 0;
                } else if (state == ST_TITLE && k >= SDLK_1 && k < SDLK_1 + MAX_PLAYERS) {
                    num_players = (int)(k - SDLK_1) + 1;
                } else if (state == ST_TITLE && k == SDLK_o) {
                    open_options();
                } else if (k == SDLK_RETURN && (ev.key.keysym.mod & KMOD_ALT)) {
                    toggle_fullscreen(win);
                } else if (k == SDLK_F11) {
                    toggle_fullscreen(win);
                } else if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) {
                    press_start();
                } else if (k == SDLK_p) {
                    toggle_pause();
                } else if (k == SDLK_m) {
                    audio_toggle_mute();
                } else {
                    press_dir(key_dir(k));
                }
                break;
            }
            case SDL_CONTROLLERDEVICEADDED:
                if (!pad)
                    pad = SDL_GameControllerOpen(ev.cdevice.which);
                break;
            case SDL_CONTROLLERDEVICEREMOVED:
                if (pad && ev.cdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad))) {
                    SDL_GameControllerClose(pad);
                    pad = NULL;
                }
                break;
            case SDL_CONTROLLERBUTTONDOWN:
                if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_START)
                    in_game() ? toggle_pause() : press_start();
                else if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_A)
                    press_start();
                else if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_B)
                    press_back();
                else if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_Y)
                    open_options();
                else
                    press_dir(button_dir(ev.cbutton.button));
                break;
            case SDL_CONTROLLERAXISMOTION: {
                /* Act once per push so menus don't race through their choices. */
                static int held[2];
                int ax = ev.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX ? 0
                         : ev.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY ? 1 : -1;
                if (ax < 0)
                    break;
                int dir = abs(ev.caxis.value) <= 16000 ? NODIR
                          : ax == 0 ? (ev.caxis.value < 0 ? LEFT : RIGHT)
                                    : (ev.caxis.value < 0 ? UP : DOWN);
                if (dir != held[ax] - 1)
                    press_dir(dir);
                held[ax] = dir + 1;
                break;
            }
            case SDL_WINDOWEVENT:
                if (ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST && !paused &&
                    (state == ST_PLAY || state == ST_READY) && !autoplay)
                    toggle_pause();
                break;
            }
        }

        Uint64 now = SDL_GetPerformanceCounter();
        acc += (double)(now - prev) / (double)SDL_GetPerformanceFrequency();
        prev = now;
        if (acc > 0.25)
            acc = 0.25;
        if (max_frames >= 0)
            acc = step; /* tests run one update per loop, as fast as possible */
        while (acc >= step) {
            update();
            acc -= step;
            if (max_frames >= 0 && ++frames_run >= max_frames)
                running = 0;
        }

        render();
        SDL_UpdateTexture(tex, NULL, fb, SCR_W * (int)sizeof(uint32_t));
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, NULL, NULL);
        SDL_RenderPresent(ren);
        if (max_frames < 0 && !vsync)
            SDL_Delay(1);
    }
    if (shot && SDL_SaveBMP(SDL_CreateRGBSurfaceWithFormatFrom(fb, SCR_W, SCR_H, 32, SCR_W * 4,
                                                                SDL_PIXELFORMAT_ARGB8888), shot) != 0)
        fprintf(stderr, "mskman: screenshot failed: %s\n", SDL_GetError());

    if (max_frames >= 0) {
        printf("mskman: ran %ld frames, player %d, level %d, score %d, lives %d\n", frames_run,
               cur_player + 1, level, score, lives);
        for (int p = 0; p < num_players && num_players > 1; p++)
            printf("mskman:   player %d score %d lives %d level %d\n", p + 1,
                   p == cur_player ? score : turns[p].score, p == cur_player ? lives : turns[p].lives,
                   p == cur_player ? level : turns[p].level);
    }
    if (score >= hiscore && score > 0)
        save_hiscore();
    if (pad)
        SDL_GameControllerClose(pad);
    audio_quit();
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
