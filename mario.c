/*
 * SUPER MEOS BROS. - a side-scrolling platformer for MeOS (M5Stack Tab5).
 *
 * Arrows / A D: run     Up / W / Space: jump     Enter: start     Q: quit
 * Stomp the goombas, bump the ? blocks, grab the coins and reach the flag.
 *
 * Everything is drawn with fills/triangles (no per-pixel loops), 30 fps, double buffered.
 */
#include "meos.h"
#include "meos_util.h"

#ifdef __wasm__
/* clang may turn init loops / struct copies into these calls, and there is no libc */
void *memset(void *d, int c, unsigned n) { volatile unsigned char *p = (volatile unsigned char *)d; while (n--) *p++ = (unsigned char)c; return d; }
void *memcpy(void *d, const void *s, unsigned n) { volatile unsigned char *p = (volatile unsigned char *)d; const unsigned char *q = (const unsigned char *)s; while (n--) *p++ = *q++; return d; }
#endif

#define SW 1280
#define SH 720
#define TS 48                 /* tile size in pixels */
#define LW 212                /* level width in tiles */
#define LH 15                 /* level height in tiles (15 * 48 = 720) */
#define GROUND 13             /* first ground row */
#define PW 32
#define PH 44
#define EW 40
#define EH 40
#define MAXE 32
#define POLE_X 196

#define C(r, g, b) MEOS_RGB(r, g, b)
#define SKY C(92, 148, 252)

static char map[LH][LW + 1];

enum { S_TITLE, S_PLAY, S_DYING, S_FLAG, S_WALK, S_OVER, S_CLEAR };

static float px, py, vx, vy;
static int face = 1, onground, state, lives, score, coins, time_left, tick, phase, state_t;
static int left_t, right_t, jump_buf, cam, hi_score, time_sub;

typedef struct { float x, y, vx, vy; int alive, squash; } Enemy;
static Enemy en[MAXE];
static int n_en;

typedef struct { int x, y, t, v; } Pop;           /* score/coin popups and block bumps */
static Pop pops[8];
static int bump_x = -1, bump_y, bump_t;

/* ---------- level ---------- */

static void put(int x, int y, char c) { if (x >= 0 && x < LW && y >= 0 && y < LH) map[y][x] = c; }

static void pipe(int x, int h)
{
    int top = GROUND - h;
    put(x, top, 'l'); put(x + 1, top, 'r');
    for (int y = top + 1; y < GROUND; y++) { put(x, y, 'L'); put(x + 1, y, 'R'); }
}

static void row(int x, int y, const char *s) { while (*s) put(x++, y, *s++); }

static void build(void)
{
    for (int y = 0; y < LH; y++) { for (int x = 0; x < LW; x++) map[y][x] = '.'; map[y][LW] = 0; }
    for (int x = 0; x < LW; x++) { put(x, GROUND, 'G'); put(x, GROUND + 1, 'G'); }

    static const short pits[][2] = { {52, 54}, {86, 88}, {104, 105}, {126, 128}, {154, 157}, {170, 172} };
    for (int i = 0; i < 6; i++) for (int x = pits[i][0]; x <= pits[i][1]; x++) { put(x, GROUND, '.'); put(x, GROUND + 1, '.'); }

    put(16, 9, '?');
    row(20, 9, "B?B?B");
    pipe(28, 2); pipe(38, 3); pipe(46, 2);
    row(32, 10, "cccc");

    row(58, 9, "B?BBB?B");
    row(70, 8, "cccc");
    pipe(74, 3); pipe(80, 2);
    row(92, 9, "?B?B");
    row(85, 10, "ccccc");

    for (int i = 0; i < 4; i++) for (int h = 0; h <= i; h++) put(100 + i, GROUND - 1 - h, 'S');
    for (int i = 0; i < 4; i++) for (int h = 0; h < 4 - i; h++) put(106 + i, GROUND - 1 - h, 'S');

    pipe(114, 2);
    row(120, 9, "?B?");
    pipe(132, 3);
    row(124, 10, "ccccc");

    row(142, 9, "BBB?BBB?B");
    row(153, 9, "cccccc");
    pipe(162, 2);
    row(168, 10, "ccccc");
    for (int i = 0; i < 8; i++) for (int h = 0; h <= i; h++) put(180 + i, GROUND - 1 - h, 'S');

    for (int y = 3; y < GROUND; y++) put(POLE_X, y, 'F');
    put(POLE_X, 2, 'O');
    for (int x = 200; x <= 204; x++) { put(x, 11, 'K'); put(x, 12, 'K'); }
    for (int x = 200; x <= 204; x += 2) put(x, 10, 'K');
    put(202, 11, 'D'); put(202, 12, 'D');

    static const short gx[] = { 22, 34, 44, 58, 63, 70, 78, 91, 97, 111, 118, 123, 134, 146, 149, 160, 165, 175, 190, 193 };
    n_en = 0;
    for (unsigned i = 0; i < sizeof(gx) / sizeof(gx[0]); i++) {
        Enemy *e = &en[n_en++];
        e->x = (float)(gx[i] * TS + 4); e->y = (float)(GROUND * TS - EH); e->vx = -2.0f; e->vy = 0; e->alive = 1; e->squash = 0;
    }
}

static int is_solid(char c)
{
    return c == 'G' || c == 'B' || c == '?' || c == 'U' || c == 'S' || c == 'l' || c == 'r' || c == 'L' || c == 'R';
}

static int hit_rect(float x, float y, int w, int h)
{
    if (x < 0) return 1;
    int x0 = (int)x / TS, x1 = (int)(x + (float)w - 1) / TS;
    if (y + (float)h - 1 < 0) return 0;
    int y0 = y < 0 ? 0 : (int)y / TS, y1 = (int)(y + (float)h - 1) / TS;
    if (x1 >= LW) return 1;
    if (y1 >= LH) y1 = LH - 1;
    for (int ty = y0; ty <= y1; ty++) for (int tx = x0; tx <= x1; tx++) if (is_solid(map[ty][tx])) return 1;
    return 0;
}

/* ---------- popups ---------- */

static void add_pop(int x, int y, int v)
{
    for (int i = 0; i < 8; i++) if (pops[i].t <= 0) { pops[i].x = x; pops[i].y = y; pops[i].t = 24; pops[i].v = v; return; }
}

static void add_score(int v, int x, int y) { score += v; add_pop(x, y, v); }

/* ---------- game flow ---------- */

static void reset_player(void)
{
    px = 2 * TS; py = GROUND * TS - PH; vx = vy = 0; face = 1; onground = 1;
    left_t = right_t = jump_buf = 0; cam = 0; time_left = 300;
}

static void new_game(void)
{
    build();
    lives = 3; score = 0; coins = 0;
    for (int i = 0; i < 8; i++) pops[i].t = 0;
    bump_x = -1;
    reset_player();
    state = S_PLAY;
}

static void die(void)
{
    if (state != S_PLAY) return;
    state = S_DYING; state_t = 0; vx = 0; vy = -16;
}

static void bump_block(int tx, int ty)
{
    char c = map[ty][tx];
    if (c == '?') {
        map[ty][tx] = 'U';
        coins++; add_score(200, tx * TS, ty * TS - 20);
        bump_x = tx; bump_y = ty; bump_t = 6;
    } else if (c == 'B') {
        map[ty][tx] = '.';
        add_score(50, tx * TS, ty * TS - 20);
        bump_x = tx; bump_y = ty; bump_t = 6;
    } else if (c == 'U') {
        bump_x = tx; bump_y = ty; bump_t = 6;
    }
    /* kill walkers standing on top of the block */
    for (int i = 0; i < n_en; i++) {
        Enemy *e = &en[i];
        if (e->alive && !e->squash && e->x + EW > tx * TS && e->x < (tx + 1) * TS && e->y + EH >= ty * TS - 6 && e->y + EH <= ty * TS + 6) {
            e->alive = 0; add_score(100, (int)e->x, (int)e->y);
        }
    }
}

static void update_player(void)
{
    /* horizontal */
    if (right_t > 0 && left_t <= 0) { vx += 1.0f; if (vx > 7) vx = 7; face = 1; }
    else if (left_t > 0 && right_t <= 0) { vx -= 1.0f; if (vx < -7) vx = -7; face = -1; }
    else { vx *= 0.7f; if (vx < 0.4f && vx > -0.4f) vx = 0; }
    if (left_t > 0) left_t--;
    if (right_t > 0) right_t--;

    if (jump_buf > 0) { jump_buf--; if (onground) { vy = -19.5f; onground = 0; jump_buf = 0; } }
    vy += 1.0f; if (vy > 16) vy = 16;

    /* move X */
    px += vx;
    if (px < 0) { px = 0; vx = 0; }
    if (px < (float)cam) { px = (float)cam; if (vx < 0) vx = 0; }       /* can't walk back off-screen */
    if (hit_rect(px, py, PW, PH)) {
        if (vx > 0) px = (float)((((int)(px + PW)) / TS) * TS - PW);
        else px = (float)((((int)px) / TS + 1) * TS);
        vx = 0;
    }
    /* move Y */
    py += vy; onground = 0;
    if (hit_rect(px, py, PW, PH)) {
        if (vy > 0) { py = (float)((((int)(py + PH)) / TS) * TS - PH); onground = 1; }
        else {
            int ty = ((int)py) / TS;
            py = (float)((ty + 1) * TS);
            int tx = ((int)(px + PW / 2)) / TS;
            if (!is_solid(map[ty][tx])) tx = ((int)px) / TS;
            if (!is_solid(map[ty][tx])) tx = ((int)(px + PW - 1)) / TS;
            bump_block(tx, ty);
        }
        vy = 0;
    }
    phase += (int)(vx < 0 ? -vx : vx);

    /* coins */
    int x0 = (int)px / TS, x1 = (int)(px + PW - 1) / TS, y0 = py < 0 ? 0 : (int)py / TS, y1 = (int)(py + PH - 1) / TS;
    if (y1 >= LH) y1 = LH - 1;
    for (int ty = y0; ty <= y1; ty++) for (int tx = x0; tx <= x1; tx++)
        if (map[ty][tx] == 'c') { map[ty][tx] = '.'; coins++; add_score(100, tx * TS, ty * TS - 10); }
    if (coins >= 100) { coins -= 100; lives++; }

    if (py > (float)(LH * TS)) die();
    if (px + PW > POLE_X * TS + 18 && state == S_PLAY) {            /* the flag */
        int h = (GROUND * TS - (int)py) / TS;                       /* higher grab, bigger bonus */
        add_score(500 + h * 200, POLE_X * TS - 40, (int)py);
        state = S_FLAG; state_t = 0; vx = 0; vy = 0; px = (float)(POLE_X * TS + 10);
    }
}

static void update_enemies(void)
{
    for (int i = 0; i < n_en; i++) {
        Enemy *e = &en[i];
        if (!e->alive) continue;
        if (e->squash) { if (--e->squash <= 0) e->alive = 0; continue; }
        if (e->x > (float)(cam + SW + 96) || e->x < (float)(cam - 200)) { if (e->x < (float)(cam - 200) && e->x < 0) e->alive = 0; continue; }   /* asleep until near */
        e->x += e->vx;
        if (hit_rect(e->x, e->y, EW, EH)) { e->x -= e->vx; e->vx = -e->vx; }
        e->vy += 1.0f; if (e->vy > 14) e->vy = 14;
        e->y += e->vy;
        if (hit_rect(e->x, e->y, EW, EH)) {
            if (e->vy > 0) e->y = (float)((((int)(e->y + EH)) / TS) * TS - EH);
            e->vy = 0;
        }
        if (e->y > (float)(LH * TS)) { e->alive = 0; continue; }
        /* vs player */
        if (state == S_PLAY && px + PW > e->x + 4 && px < e->x + EW - 4 && py + PH > e->y + 4 && py < e->y + EH) {
            if (vy > 0 && py + PH < e->y + 28) {
                e->squash = 12; vy = -11; add_score(100, (int)e->x, (int)e->y - 20);
            } else die();
        }
    }
}

/* ---------- drawing ---------- */

/* fills must lie inside the screen, so clip here */
static void rect(int x, int y, int w, int h, unsigned c)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > SW) w = SW - x;
    if (y + h > SH) h = SH - y;
    if (w > 0 && h > 0) meos_display_fill(x, y, w, h, c);
}

static void text(int x, int y, const char *s, unsigned fg, unsigned bg, int scale)
{
    if (x < 0 || y < 0 || x + meos_strlen(s) * 8 * scale > SW || y + 8 * scale > SH) return;
    meos_display_text(x, y, s, meos_strlen(s), fg, bg, scale);
}

static void num_text(int x, int y, const char *label, unsigned v, int digits, unsigned fg, unsigned bg)
{
    char b[40]; int n = 0;
    if (x < 0 || y < 0 || x + 160 > SW) return;
    while (*label) b[n++] = *label++;
    char d[10]; int k = 0;
    do { d[k++] = (char)('0' + v % 10); v /= 10; } while (v && k < 9);
    for (int i = k; i < digits; i++) b[n++] = '0';
    while (k) b[n++] = d[--k];
    meos_display_text(x, y, b, n, fg, bg, 2);
}

static void draw_background(int top)                          /* top: first row to repaint (the HUD strip is kept) */
{
    rect(0, top, SW, SH - top, SKY);
    int c3 = cam / 3;
    for (int i = -1; i < 3; i++) {                               /* clouds */
        int cx = i * 520 + 120 - c3 % 520, cy = 90 + ((i + 5) % 3) * 46;
        rect(cx, cy, 120, 34, C(255, 255, 255));
        rect(cx + 20, cy - 18, 70, 24, C(255, 255, 255));
    }
}

static void draw_tile(char c, int x, int y, int ty)
{
    switch (c) {
    case 'G':
        rect(x, y, TS, TS, C(200, 100, 40));
        rect(x, y + TS - 3, TS, 3, C(110, 50, 10));
        rect(x + TS - 3, y, 3, TS, C(110, 50, 10));
        if (ty == GROUND) { rect(x, y, TS, 10, C(80, 200, 60)); rect(x, y + 10, TS, 3, C(40, 130, 30)); }
        else rect(x + 8, y + 22, 14, 3, C(150, 70, 20));
        break;
    case 'B':
        rect(x, y, TS, TS, C(190, 90, 30));
        rect(x, y + 23, TS, 3, C(80, 30, 5));
        rect(x + 22, y, 3, 23, C(80, 30, 5)); rect(x + 10, y + 26, 3, 22, C(80, 30, 5));
        break;
    case '?': {
        int f = (tick / 6) % 6;
        unsigned col = f == 0 ? C(255, 235, 120) : f == 1 ? C(255, 210, 70) : C(250, 170, 30);
        rect(x, y, TS, TS, C(110, 60, 10));
        rect(x + 3, y + 3, TS - 6, TS - 6, col);
        text(x + 12, y + 10, "?", C(110, 60, 10), col, 3);
        break; }
    case 'U':
        rect(x, y, TS, TS, C(90, 60, 30));
        rect(x + 3, y + 3, TS - 6, TS - 6, C(160, 120, 80));
        break;
    case 'S':
        rect(x, y, TS, TS, C(120, 80, 50));
        rect(x + 3, y + 3, TS - 6, TS - 6, C(190, 140, 100));
        rect(x + 3, y + TS - 9, TS - 6, 6, C(140, 100, 70));
        break;
    case 'l': case 'r': case 'L': case 'R': {
        int top = (c == 'l' || c == 'r'), lft = (c == 'l' || c == 'L');
        rect(x, y, TS, TS, C(10, 90, 20));
        int ix = x + (top ? 3 : 5), iw = TS - (top ? 6 : 8);
        if (!top) ix = lft ? x + 6 : x + 2;
        if (!top) iw = TS - 8;
        rect(ix, y + (top ? 3 : 0), iw, TS - (top ? 6 : 0), C(40, 190, 60));
        rect(lft ? ix + 6 : ix + iw - 14, y + (top ? 6 : 0), 6, TS - (top ? 12 : 0), C(150, 240, 150));
        break; }
    case 'c': {
        int w = 8 + ((tick / 3 + x / TS) % 4) * 4;
        if (w > 20) w = 40 - w;
        rect(x + 24 - w / 2, y + 8, w, 32, C(255, 210, 40));
        rect(x + 24 - w / 2 + 3, y + 12, w > 8 ? w - 6 : 2, 24, C(255, 240, 130));
        break; }
    case 'F': rect(x + 20, y, 8, TS, C(210, 230, 210)); break;
    case 'O': rect(x + 14, y + 14, 20, 20, C(40, 190, 60)); rect(x + 20, y + 28, 8, 20, C(210, 230, 210)); break;
    case 'K': rect(x, y, TS, TS, C(190, 100, 50)); rect(x, y + 23, TS, 3, C(90, 40, 10)); rect(x + 22, y, 3, 24, C(90, 40, 10)); rect(x + 10, y + 26, 3, 22, C(90, 40, 10)); break;
    case 'D': rect(x + 6, y, TS - 12, TS, C(20, 10, 10)); break;
    default: break;
    }
}

static void draw_tiles(void)
{
    int t0 = cam / TS, t1 = (cam + SW) / TS + 1;
    if (t1 >= LW) t1 = LW - 1;
    for (int ty = GROUND; ty <= GROUND + 1; ty++) {                 /* ground: one fill per run of tiles, not per tile */
        int tx = t0;
        while (tx <= t1) {
            if (map[ty][tx] != 'G') { tx++; continue; }
            int e = tx;
            while (e + 1 <= t1 && map[ty][e + 1] == 'G') e++;
            int x = tx * TS - cam, w = (e - tx + 1) * TS, y = ty * TS;
            rect(x, y, w, TS, C(200, 100, 40));
            rect(x, y + TS - 3, w, 3, C(110, 50, 10));
            if (ty == GROUND) { rect(x, y, w, 10, C(80, 200, 60)); rect(x, y + 10, w, 3, C(40, 130, 30)); }
            for (int k = tx; k <= e; k += 2) rect(k * TS - cam + TS - 3, y, 3, TS, C(110, 50, 10));
            tx = e + 1;
        }
    }
    for (int ty = 0; ty < LH; ty++)
        for (int tx = t0; tx <= t1; tx++) {
            char c = map[ty][tx];
            if (c == '.' || c == 'G') continue;
            int y = ty * TS;
            if (tx == bump_x && ty == bump_y && bump_t > 0) y -= (bump_t > 3 ? 3 - (6 - bump_t) : bump_t) * 4;
            draw_tile(c, tx * TS - cam, y, ty);
        }
    /* flag on the pole */
    int fx = POLE_X * TS - cam;
    int fy = (state == S_FLAG || state == S_WALK || state == S_CLEAR) ? (state_t < 40 ? 3 * TS + state_t * 10 : 3 * TS + 400) : 3 * TS;
    if (fy > (GROUND - 1) * TS - 16) fy = (GROUND - 1) * TS - 16;
    meos_display_tri(fx + 20, fy, fx - 26, fy + 18, fx + 20, fy + 36, C(255, 255, 255));
}

static void draw_enemy(Enemy *e)
{
    int x = (int)e->x - cam, y = (int)e->y;
    if (x < -64 || x > SW) return;
    unsigned body = C(160, 80, 20), dark = C(90, 40, 10);
    if (e->squash) {
        rect(x, y + EH - 14, EW, 14, body);
        rect(x + 8, y + EH - 10, 8, 4, C(255, 255, 255)); rect(x + 24, y + EH - 10, 8, 4, C(255, 255, 255));
        return;
    }
    int step = (tick / 6) & 1;
    meos_display_tri(x + EW / 2, y, x - 2, y + 30, x + EW + 2, y + 30, body);     /* head */
    rect(x + 4, y + 24, EW - 8, 8, body);
    rect(x + 6, y + 14, 12, 10, C(255, 255, 255)); rect(x + 22, y + 14, 12, 10, C(255, 255, 255));
    rect(x + (e->vx < 0 ? 6 : 12), y + 18, 6, 6, C(0, 0, 0)); rect(x + (e->vx < 0 ? 22 : 28), y + 18, 6, 6, C(0, 0, 0));
    rect(x + 4, y + 12, 14, 3, dark); rect(x + 22, y + 12, 14, 3, dark);
    rect(x + 10, y + 32, 20, 4, C(250, 220, 170));                                    /* body */
    rect(x + (step ? 0 : 4), y + 34, 16, 6, dark); rect(x + (step ? 24 : 20), y + 34, 16, 6, dark);   /* feet */
}

static void draw_player(void)
{
    int x = (int)px - cam, y = (int)py;
    int walking = onground && (vx > 0.5f || vx < -0.5f);
    int leg = walking ? (phase / 16) % 2 : 0;
    int dir = face;
    unsigned red = C(230, 40, 30), blue = C(40, 60, 220), skin = C(255, 204, 150), brown = C(110, 50, 10);
    rect(x + 3, y, PW - 6, 8, red);                                       /* cap */
    rect(dir > 0 ? x + 10 : x - 2, y + 4, 24, 5, red);                    /* brim */
    rect(x + 4, y + 8, PW - 8, 13, skin);                                 /* face */
    rect(dir > 0 ? x + 18 : x + 8, y + 10, 5, 7, C(0, 0, 0));             /* eye */
    rect(dir > 0 ? x + 14 : x + 4, y + 17, 14, 4, brown);                 /* moustache */
    rect(dir > 0 ? x + 2 : x + PW - 8, y + 8, 6, 8, brown);              /* hair */
    rect(x + 2, y + 21, PW - 4, 11, red);                                 /* shirt */
    rect(x + 6, y + 24, PW - 12, 12, blue);                               /* overalls */
    rect(x + 12, y + 26, 4, 4, C(255, 220, 60)); rect(x + PW - 16, y + 26, 4, 4, C(255, 220, 60));
    if (!onground) {
        rect(x + (dir > 0 ? PW - 6 : -4), y + 20, 10, 8, skin);           /* arm up */
        rect(x - 2, y + 34, 14, 10, brown); rect(x + PW - 12, y + 32, 14, 10, brown);
    } else if (leg) {
        rect(x - 2, y + 36, 16, 8, brown); rect(x + PW - 10, y + 34, 14, 8, brown);
    } else {
        rect(x, y + 36, 14, 8, brown); rect(x + PW - 14, y + 36, 14, 8, brown);
    }
}

static void draw_hud(void)
{
    rect(0, 0, SW, 56, SKY);
    text(24, 8, "MEOS", C(255, 255, 255), SKY, 2);
    num_text(24, 28, "", (unsigned)score, 6, C(255, 255, 255), SKY);
    num_text(300, 28, "x", (unsigned)coins, 2, C(255, 230, 80), SKY);
    rect(272, 30, 14, 20, C(255, 210, 40));
    text(520, 8, "WORLD", C(255, 255, 255), SKY, 2);
    text(520, 28, "1-1", C(255, 255, 255), SKY, 2);
    text(760, 8, "LIVES", C(255, 255, 255), SKY, 2);
    num_text(760, 28, "x", (unsigned)lives, 1, C(255, 255, 255), SKY);
    text(1040, 8, "TIME", C(255, 255, 255), SKY, 2);
    num_text(1040, 28, "", (unsigned)(time_left < 0 ? 0 : time_left), 3, C(255, 255, 255), SKY);
}

static void draw_pops(void)
{
    for (int i = 0; i < 8; i++) {
        if (pops[i].t <= 0) continue;
        num_text(pops[i].x - cam, pops[i].y - (24 - pops[i].t) * 2, "", (unsigned)pops[i].v, 1, C(255, 255, 255), 0);
    }
}

static int hud_sig[5] = { -1, -1, -1, -1, -1 };

static int hud_changed(void)                                     /* the HUD strip survives between frames: redraw it only on change */
{
    int v[5] = { score, coins, lives, time_left, state };
    int ch = 0;
    for (int i = 0; i < 5; i++) if (v[i] != hud_sig[i]) { hud_sig[i] = v[i]; ch = 1; }
    return ch;
}

static void draw_scene(void)
{
    int hud = hud_changed();
    draw_background(hud ? 0 : 56);
    draw_tiles();
    for (int i = 0; i < n_en; i++) if (en[i].alive) draw_enemy(&en[i]);
    draw_player();
    draw_pops();
    if (hud) draw_hud();
}

static void center_text(int y, const char *s, unsigned fg, unsigned bg, int scale)
{
    int w = meos_strlen(s) * 8 * scale;
    text((SW - w) / 2, y, s, fg, bg, scale);
}

/* ---------- main loop ---------- */

void _start(void)
{
    if (meos_display_open() < 0) { meos_puts("this app needs the display permission\n"); return; }
    meos_display_clip(0, 0, SW, SH);
    unsigned last = meos_uptime_ms(), prev = last;
    int acc = 0;
    state = S_TITLE; state_t = 0; hi_score = 0;
    build(); reset_player();

    for (;;) {
        tick++; state_t++;
        /* The game runs at 30 steps per second of real time: if drawing is slow, several steps are done per picture. */
        unsigned now_ms = meos_uptime_ms();
        acc += (int)(now_ms - prev); prev = now_ms;
        int steps = acc / 33;
        if (steps < 1) steps = 1;
        if (steps > 3) { steps = 3; acc = 0; } else { acc -= steps * 33; if (acc < 0) acc = 0; }
        int k;
        int quit = 0;
        while ((k = meos_getkey()) >= 0) {
            if (k == 'q' || k == 'Q') { quit = 1; break; }
            if (state == S_TITLE || state == S_OVER || state == S_CLEAR) {
                if ((k == '\r' || k == '\n' || k == ' ') && state_t > 20) { if (score > hi_score) hi_score = score; new_game(); }
                continue;
            }
            if (k == MEOS_KEY_RIGHT || k == 'd' || k == 'D') { right_t = 10; left_t = 0; }
            else if (k == MEOS_KEY_LEFT || k == 'a' || k == 'A') { left_t = 10; right_t = 0; }
            else if (k == MEOS_KEY_UP || k == 'w' || k == 'W' || k == ' ') jump_buf = 7;
            else if (k == MEOS_KEY_DOWN || k == 's') { vx *= 0.5f; right_t = left_t = 0; }
        }
        if (quit) { meos_display_close(); return; }

        for (int n = state == S_PLAY ? steps : 1; n > 0; n--) {
            for (int i = 0; i < 8; i++) if (pops[i].t > 0) pops[i].t--;
            if (bump_t > 0) bump_t--;
        }

        switch (state) {
        case S_TITLE:
            cam = (tick * 3) % 3000; if (cam > (LW * TS - SW)) cam = 0;
            draw_background(0); draw_tiles(); hud_sig[4] = -1;
            center_text(150, "SUPER MEOS BROS.", C(255, 255, 255), SKY, 6);
            center_text(300, "Arrows / A D: run     Up / W / Space: jump", C(255, 255, 255), SKY, 2);
            center_text(340, "Stomp the goombas, reach the flag!", C(255, 255, 255), SKY, 2);
            if ((tick / 15) & 1) center_text(440, "PRESS ENTER", C(255, 230, 80), SKY, 4);
            center_text(530, "Q: quit", C(230, 240, 255), SKY, 2);
            if (hi_score) num_text(520, 600, "TOP ", (unsigned)hi_score, 6, C(255, 255, 255), SKY);
            break;

        case S_PLAY:
            for (int n = 0; n < steps && state == S_PLAY; n++) {
                update_player();
                update_enemies();
                if (++time_sub >= 24) { time_sub = 0; if (--time_left <= 0) die(); }
            }
            cam = (int)px - 480;
            if (cam < 0) cam = 0;
            if (cam > LW * TS - SW) cam = LW * TS - SW;
            draw_scene();
            break;

        case S_DYING:
            if (state_t > 12) { vy += 1.0f; if (vy > 16) vy = 16; py += vy; } else if (state_t == 12) vy = -14;
            draw_scene();
            if (state_t > 70) {
                if (--lives <= 0) { state = S_OVER; state_t = 0; }
                else { reset_player(); state = S_PLAY; }
            }
            break;

        case S_FLAG:                                    /* slide down the pole */
            py += 8;
            if (py >= (float)(GROUND * TS - PH)) { py = (float)(GROUND * TS - PH); state = S_WALK; state_t = 0; face = 1; }
            draw_scene();
            break;

        case S_WALK:                                    /* walk to the castle */
            if (state_t > 4) { px += 4; vx = 4; phase += 4; onground = 1; }
            if (px > 202 * TS) { state = S_CLEAR; state_t = 0; score += time_left * 10; time_left = 0; }
            draw_scene();
            break;

        case S_CLEAR:
            draw_scene();
            center_text(240, "COURSE CLEAR!", C(255, 255, 255), 0, 6);
            if ((tick / 15) & 1) center_text(360, "PRESS ENTER", C(255, 230, 80), 0, 3);
            break;

        case S_OVER:
            draw_scene();
            center_text(260, "GAME OVER", C(255, 255, 255), 0, 7);
            if ((tick / 15) & 1) center_text(400, "PRESS ENTER", C(255, 230, 80), 0, 3);
            break;
        }
        frame_wait(&last, 33);
    }
}
