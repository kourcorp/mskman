/*
 * Procedural mazes.
 *
 * The left half of the maze is a lattice of corridors: corridor columns at
 * COLS and corridor rows at ROWS, joined by segments. Starting from the full
 * lattice, random segments are removed as long as no corridor becomes a dead
 * end and the half stays connected, which merges the wall blocks between them
 * into larger shapes. The ghost house, its surrounding ring, the player start
 * and the fruit spot are fixed so the game logic works on every maze.
 */
#include <string.h>

#include "mazegen.h"

enum { H = 31, HW = 14, NC = 4, NR = 10 };
static const int COLS[NC] = {1, 6, 9, 12};
static const int ROWS[NR] = {1, 5, 8, 11, 14, 17, 20, 23, 26, 29};
/* Row indices of special rows. */
enum { R_RING_TOP = 3, R_HOUSE = 4, R_RING_BOT = 5, R_START = 7 };

/* Segment states. */
enum { OFF, ON, FIXED };

typedef struct {
    unsigned char h[NR][NC - 1]; /* row r, between column c and c+1 */
    unsigned char v[NR - 1][NC]; /* column c, between row r and r+1 */
    unsigned char ctr[NR];       /* from the last column across the centre */
    unsigned char tun[NR];       /* from the first column out through the side */
} Lattice;

static uint32_t next(uint32_t *s)
{
    *s ^= *s << 13;
    *s ^= *s >> 17;
    *s ^= *s << 5;
    return *s;
}

static int degree(const Lattice *L, int r, int c)
{
    int d = 0;
    if (c > 0) d += L->h[r][c - 1] != OFF;
    if (c < NC - 1) d += L->h[r][c] != OFF;
    if (r > 0) d += L->v[r - 1][c] != OFF;
    if (r < NR - 1) d += L->v[r][c] != OFF;
    if (c == NC - 1) d += L->ctr[r] != OFF;
    if (c == 0) d += L->tun[r] != OFF;
    return d;
}

/* All nodes with corridors must be reachable from the start node. */
static int connected(const Lattice *L)
{
    int seen[NR][NC] = {{0}}, stack[NR * NC], n = 0;
    seen[R_START][NC - 1] = 1;
    stack[n++] = R_START * NC + NC - 1;
    while (n > 0) {
        int r = stack[--n] / NC, c = stack[n] % NC;
        int nb[4][3] = {
            {r, c - 1, c > 0 && L->h[r][c - 1] != OFF},
            {r, c + 1, c < NC - 1 && L->h[r][c] != OFF},
            {r - 1, c, r > 0 && L->v[r - 1][c] != OFF},
            {r + 1, c, r < NR - 1 && L->v[r][c] != OFF},
        };
        for (int i = 0; i < 4; i++)
            if (nb[i][2] && !seen[nb[i][0]][nb[i][1]]) {
                seen[nb[i][0]][nb[i][1]] = 1;
                stack[n++] = nb[i][0] * NC + nb[i][1];
            }
    }
    for (int r = 0; r < NR; r++)
        for (int c = 0; c < NC; c++)
            if (!seen[r][c] && degree(L, r, c) > 0)
                return 0;
    return 1;
}

static void open_tile(char half[H][HW + 1], int x, int y)
{
    if (half[y][x] == '#')
        half[y][x] = '.';
}

/* Puts an energizer on the dot closest to (tx, ty). */
static void place_energizer(char half[H][HW + 1], int tx, int ty)
{
    int best = -1, bx = 0, by = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < HW; x++) {
            int d = (x > tx ? x - tx : tx - x) * 2 + (y > ty ? y - ty : ty - y);
            if (half[y][x] == '.' && (best < 0 || d < best)) {
                best = d;
                bx = x;
                by = y;
            }
        }
    if (best >= 0)
        half[by][bx] = 'o';
}

void mazegen_build(uint32_t seed, char half[H][HW + 1])
{
    Lattice L;
    uint32_t s = seed * 2654435761u ^ 0x9E3779B9u;
    if (!s)
        s = 1;
    for (int i = 0; i < 8; i++)
        next(&s);

    memset(&L, ON, sizeof L);
    memset(L.tun, OFF, sizeof L.tun);
    /* The ghost house blocks the last column and the row through its middle. */
    L.v[R_RING_TOP][NC - 1] = OFF;
    L.v[R_HOUSE][NC - 1] = OFF;
    L.h[R_HOUSE][NC - 2] = OFF;
    L.ctr[R_HOUSE] = OFF;
    /* Ring around the house, the ghost exit and the start corridor. */
    L.v[R_RING_TOP][NC - 2] = L.v[R_HOUSE][NC - 2] = FIXED;
    L.h[R_RING_TOP][NC - 2] = L.h[R_RING_BOT][NC - 2] = FIXED;
    L.ctr[R_RING_TOP] = L.ctr[R_RING_BOT] = L.ctr[R_START] = FIXED;
    /* One or two side tunnels. */
    static const int tunnel_rows[3] = {2, 4, 6};
    int t = next(&s) % 3;
    L.tun[tunnel_rows[t]] = FIXED;
    if (next(&s) % 2)
        L.tun[tunnel_rows[(t + 1 + next(&s) % 2) % 3]] = FIXED;
    /* Edge rows and columns get fewer centre links. */
    L.ctr[0] = next(&s) % 3 == 0 ? ON : OFF;
    L.ctr[NR - 1] = next(&s) % 3 == 0 ? ON : OFF;

    /* Candidate segments to remove: encoded as kind*100 + r*10 + c. */
    int cand[NR * NC * 3], n = 0;
    for (int r = 0; r < NR; r++)
        for (int c = 0; c < NC; c++) {
            if (c < NC - 1 && L.h[r][c] == ON) cand[n++] = 0 * 100 + r * 10 + c;
            if (r < NR - 1 && L.v[r][c] == ON) cand[n++] = 1 * 100 + r * 10 + c;
            if (c == NC - 1 && L.ctr[r] == ON) cand[n++] = 2 * 100 + r * 10 + c;
        }
    for (int i = n - 1; i > 0; i--) {
        int j = (int)(next(&s) % (uint32_t)(i + 1)), tmp = cand[i];
        cand[i] = cand[j];
        cand[j] = tmp;
    }
    int target = 18 + (int)(next(&s) % 9), removed = 0;
    for (int i = 0; i < n && removed < target; i++) {
        int kind = cand[i] / 100, r = cand[i] / 10 % 10, c = cand[i] % 10;
        unsigned char *seg = kind == 0 ? &L.h[r][c] : kind == 1 ? &L.v[r][c] : &L.ctr[r];
        int r2 = kind == 1 ? r + 1 : r, c2 = kind == 0 ? c + 1 : c;
        *seg = OFF;
        if (degree(&L, r, c) == 1 || degree(&L, r2, c2) == 1 || !connected(&L))
            *seg = ON;
        else
            removed++;
    }

    /* Paint corridors (dots) into a solid block of wall. */
    for (int y = 0; y < H; y++) {
        memset(half[y], '#', HW);
        half[y][HW] = 0;
    }
    for (int r = 0; r < NR; r++)
        for (int c = 0; c < NC; c++) {
            if (c < NC - 1 && L.h[r][c])
                for (int x = COLS[c]; x <= COLS[c + 1]; x++)
                    open_tile(half, x, ROWS[r]);
            if (r < NR - 1 && L.v[r][c])
                for (int y = ROWS[r]; y <= ROWS[r + 1]; y++)
                    open_tile(half, COLS[c], y);
        }
    for (int r = 0; r < NR; r++) {
        if (L.ctr[r])
            for (int x = COLS[NC - 1]; x < HW; x++)
                open_tile(half, x, ROWS[r]);
        if (L.tun[r]) {
            /* No dots near the tunnel mouth. */
            half[ROWS[r]][0] = ' ';
            for (int x = 1; x < COLS[1]; x++)
                if (half[ROWS[r]][x] != '#')
                    half[ROWS[r]][x] = ' ';
        }
    }

    /* Ring and house: no dots, fixed walls and door. */
    for (int y = ROWS[R_RING_TOP]; y <= ROWS[R_RING_BOT]; y++)
        for (int x = COLS[NC - 2]; x < HW; x++)
            if (half[y][x] != '#')
                half[y][x] = ' ';
    for (int x = 10; x < HW; x++) {
        half[12][x] = x == HW - 1 ? '-' : '#';
        half[16][x] = '#';
    }
    for (int y = 13; y <= 15; y++) {
        half[y][10] = '#';
        for (int x = 11; x < HW; x++)
            half[y][x] = ' ';
    }

    place_energizer(half, 1, 3);
    place_energizer(half, 1, 26);
}
