/*
 * The SDL3 front end: window, input, sound, saves, and the loop that decides
 * how much to run.
 *
 * Sound is the master clock: every frame the machine runs for exactly the
 * time the audio device still needs samples for, so emulated time stays
 * locked to real time with no drift and no gaps. Without an audio device
 * (or while fast forwarding) the wall clock is used instead.
 *
 * Written against SDL's main callbacks so the same file runs natively and
 * under Emscripten, where the browser owns the main loop.
 */
#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <stdio.h>
#include <string.h>
#include "esp8266.h"
#include "audio.h"
#include "gif.h"
#include "rom_path.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#define W 128
#define H 128
#define AUDIO_RATE      48000
#define AUDIO_TARGET_S  0.05                    /* keep this much sound queued */
#define MAX_SLICE_S     (1.0 / 15.0)            /* never catch up more than this at once */
#define FAST_FORWARD    4
#define BAR_H           24
#define SAVE_NS         3000000000ull           /* the flash is written back this often when it changed */

/* the ESPboy's buttons, as MCP23017 port A bits */
enum {
    KEY_LEFT = 0x01, KEY_UP = 0x02, KEY_DOWN = 0x04, KEY_RIGHT = 0x08,
    KEY_ACT = 0x10, KEY_ESC = 0x20, KEY_LFT = 0x40, KEY_RGT = 0x80,
};

static const char *help_text[] = {
    "Arrows           d-pad",
    "X, Space / Z     ACT (A) / ESC (B)",
    "A / S            left / right side buttons",
    "",
    "F1               this help",
    "F2               reset",
    "F3               open a program",
    "R                turn the screen 90 degrees (d-pad follows)",
    "P  /  hold Tab   pause / fast forward",
    "+ / -            volume",
    "F6               record a GIF / stop and save it",
    "F7               speaker sound / bare pin signal",
    "F8               scaling: fill the window / whole multiples",
    "F9               stats overlay",
    "F10              screenshot",
    "F11, Alt+Enter   fullscreen",
    "Esc              quit",
    "",
    "Drop a .bin (an ESPboy / Arduino flash image) on the window",
    "Command line: espboy_emulator game.bin [--rom esp8266_rom.bin]",
};

typedef struct {
    Uint64 t0;              /* ns, start of the current second */
    Uint64 run_ns;          /* host time spent emulating this second */
    uint64_t now0;
    uint32_t lcd0;
    int frames;
    float render_fps, game_fps, speed, host_load, max_speed;
} Stats;

typedef struct {
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *screen;
    SDL_AudioStream *audio_stream;
    EspAudio audio;
    float volume;
    Esp *s;
    char rom[1024];
    char program[1024];
    bool rom_ok;
    bool loaded, paused, help, stats;
    bool integer_scale;
    int rotation;           /* R: the screen turned clockwise by this many quarter turns */
    char message[256];
    Uint64 message_until;
    Uint64 last_ticks;
    Uint64 saved_at;
    double wall_carry;
    uint32_t pixels[W * H];
    float samples[4096];
    SDL_Gamepad *pads[8];
    Stats st;
    /* F6: the screen recorded as an animated GIF */
    Gif gif;
    bool recording;
    uint64_t gif_tick;      /* emulated time of the last frame recorded */
} App;

static void program_stem(App *app, char *out, size_t n);

static void show_message(App *app, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    SDL_vsnprintf(app->message, sizeof app->message, fmt, ap);
    va_end(ap);
    app->message_until = SDL_GetTicks() + 2500;
}

static void set_scale_mode(App *app)
{
    SDL_SetTextureScaleMode(app->screen, app->integer_scale ? SDL_SCALEMODE_NEAREST : SDL_SCALEMODE_PIXELART);
}

/* The scaling choice, the volume and the speaker filter are kept between
   runs: natively in settings.txt in SDL's per-user folder, in the browser in
   localStorage. The screen's turn is kept per program (load_rotation) */
#define SETTINGS_ORG "joyrider3774"
#define SETTINGS_APP "espboy_emulator"

static void load_settings(App *app)
{
#ifdef __EMSCRIPTEN__
    app->integer_scale = EM_ASM_INT({
        try { return localStorage.getItem('espboy_integer_scale') === '1' ? 1 : 0; } catch (e) { return 0; }
    });
    const int vol = EM_ASM_INT({
        try { var v = localStorage.getItem('espboy_volume'); return v === null ? 100 : parseInt(v); } catch (e) { return 100; }
    });
    app->volume = SDL_clamp(vol, 0, 200) / 100.0f;
    app->audio.speaker = EM_ASM_INT({
        try { return localStorage.getItem('espboy_speaker') === '1' ? 1 : 0; } catch (e) { return 0; }
    });
#else
    char *dir = SDL_GetPrefPath(SETTINGS_ORG, SETTINGS_APP);
    if (dir) {
        char path[1024];
        SDL_snprintf(path, sizeof path, "%ssettings.txt", dir);
        char *text = SDL_LoadFile(path, NULL);
        if (text) {
            app->integer_scale = SDL_strstr(text, "integer_scale=1") != NULL;
            app->audio.speaker = SDL_strstr(text, "speaker=1") != NULL;
            const char *v = SDL_strstr(text, "volume=");
            if (v) app->volume = SDL_clamp(SDL_atoi(v + 7), 0, 200) / 100.0f;
            SDL_free(text);
        }
        SDL_free(dir);
    }
#endif
}

static void save_settings(App *app)
{
    const int vol = (int)(app->volume * 100 + 0.5f);
#ifdef __EMSCRIPTEN__
    EM_ASM({ try { localStorage.setItem('espboy_integer_scale', $0 ? '1' : '0');
                   localStorage.setItem('espboy_volume', String($1));
                   localStorage.setItem('espboy_speaker', $2 ? '1' : '0'); } catch (e) {} },
           app->integer_scale, vol, app->audio.speaker);
#else
    char *dir = SDL_GetPrefPath(SETTINGS_ORG, SETTINGS_APP);
    if (dir) {
        char path[1024], text[96];
        SDL_snprintf(text, sizeof text, "integer_scale=%d\nvolume=%d\nspeaker=%d\n", app->integer_scale ? 1 : 0, vol,
                     app->audio.speaker ? 1 : 0);
        SDL_snprintf(path, sizeof path, "%ssettings.txt", dir);
        SDL_SaveFile(path, text, SDL_strlen(text));
        SDL_free(dir);
    }
#endif
}

/* The screen's turn (R) is kept per program, by its file name: natively as
   "name<TAB>quarter turns" lines in rotations.txt in SDL's per-user folder
   (a program not listed is not turned), in the browser in localStorage
   ("espboy_rotation:<name>") */
static const char *program_name(const App *app)
{
    const char *name = SDL_strrchr(app->program, '/');
    const char *bname = SDL_strrchr(app->program, '\\');
    if (bname > name) name = bname;
    return name ? name + 1 : app->program;
}

static void load_rotation(App *app)
{
    app->rotation = 0;
#ifdef __EMSCRIPTEN__
    app->rotation = EM_ASM_INT({
        try { return parseInt(localStorage.getItem('espboy_rotation:' + UTF8ToString($0))) || 0; } catch (e) { return 0; }
    }, program_name(app)) & 3;
#else
    char *dir = SDL_GetPrefPath(SETTINGS_ORG, SETTINGS_APP);
    if (!dir) return;
    char path[1024];
    SDL_snprintf(path, sizeof path, "%srotations.txt", dir);
    SDL_free(dir);
    char *text = SDL_LoadFile(path, NULL);
    if (!text) return;
    const char *name = program_name(app);
    const size_t len = SDL_strlen(name);
    for (char *line = text; *line; ) {
        char *end = SDL_strchr(line, '\n');
        if (!SDL_strncmp(line, name, len) && line[len] == '\t') app->rotation = SDL_atoi(line + len + 1) & 3;
        if (!end) break;
        line = end + 1;
    }
    SDL_free(text);
#endif
}

static void save_rotation(App *app)
{
    if (!app->loaded) return;
#ifdef __EMSCRIPTEN__
    EM_ASM({ try { var k = 'espboy_rotation:' + UTF8ToString($0);
                   if ($1) localStorage.setItem(k, String($1)); else localStorage.removeItem(k); } catch (e) {} },
           program_name(app), app->rotation);
#else
    char *dir = SDL_GetPrefPath(SETTINGS_ORG, SETTINGS_APP);
    if (!dir) return;
    char path[1024];
    SDL_snprintf(path, sizeof path, "%srotations.txt", dir);
    SDL_free(dir);
    const char *name = program_name(app);
    const size_t len = SDL_strlen(name);
    char *old = SDL_LoadFile(path, NULL);
    const size_t cap = (old ? SDL_strlen(old) : 0) + len + 16;
    char *out = SDL_malloc(cap);
    if (!out) { SDL_free(old); return; }
    size_t n = 0;
    /* the other programs' lines as they were, this one's dropped... */
    for (char *line = old; line && *line; ) {
        char *end = SDL_strchr(line, '\n');
        const size_t l = end ? (size_t)(end - line) + 1 : SDL_strlen(line);
        if (!(!SDL_strncmp(line, name, len) && line[len] == '\t')) {
            SDL_memcpy(out + n, line, l);
            n += l;
            if (!end) out[n++] = '\n';
        }
        line += l;
    }
    /* ...and written again when it is turned */
    if (app->rotation) n += (size_t)SDL_snprintf(out + n, cap - n, "%s\t%d\n", name, app->rotation);
    SDL_SaveFile(path, out, n);
    SDL_free(out);
    SDL_free(old);
#endif
}

/* The flash as the program left it (EEPROM saves, LittleFS/SPIFFS files) is
   kept whole in <program>.sav and comes back with the program */
static void save_path(App *app, char *path, size_t n)
{
#ifdef __EMSCRIPTEN__
    const char *base = SDL_strrchr(app->program, '/');
    SDL_snprintf(path, n, "/espboy/saves/%s.sav", base ? base + 1 : app->program);
#else
    SDL_snprintf(path, n, "%s.sav", app->program);
#endif
}

static void save_flash(App *app)
{
    Esp *s = app->s;
    if (!app->loaded || !s->flash_dirty) return;
    char path[1100];
    save_path(app, path, sizeof path);
    SDL_IOStream *f = SDL_IOFromFile(path, "wb");
    if (!f) return;
    if (SDL_WriteIO(f, s->flash, ESP_FLASH_SIZE) == ESP_FLASH_SIZE) s->flash_dirty = false;
    SDL_CloseIO(f);
#ifdef __EMSCRIPTEN__
    EM_ASM(FS.syncfs(false, function(err) {}););
#endif
}

static void load_flash_save(App *app)
{
    char path[1100];
    save_path(app, path, sizeof path);
    size_t n = 0;
    void *data = SDL_LoadFile(path, &n);
    /* the saves of this program (<program>.sav): only the flash past the
       program image comes back, so a newer build keeps its own code */
    if (data) esp_apply_save(app->s, data, n);
    SDL_free(data);
    app->s->flash_dirty = false;
}

#ifndef __EMSCRIPTEN__
/* The ROM next to the emulator, where the release builds put it, or in a
   bios folder beside it. Started from a file manager the current folder is
   not the emulator's. In a macOS .app SDL's base path is the bundle's
   Contents/Resources: there, and then next to the bundle (and in a rom
   folder next to it) */
static void find_rom_near_app(char *out, size_t n)
{
    const char *base = SDL_GetBasePath();
    if (!base) { out[0] = 0; return; }
    char dirs[2][1024];
    int ndirs = 0;
    SDL_strlcpy(dirs[ndirs++], base, sizeof dirs[0]);
    const char *bundle = SDL_strstr(base, ".app/Contents/");
    if (bundle) {
        /* the folder the .app is in */
        SDL_strlcpy(dirs[ndirs], base, sizeof dirs[0]);
        char *end = dirs[ndirs] + (bundle - base);
        *end = 0;
        char *slash = SDL_strrchr(dirs[ndirs], '/');
        if (slash) { slash[1] = 0; ndirs++; }
    }
    for (int i = 0; i < ndirs; i++) {
        static const char *const names[] = { "esp8266_rom.bin", "bios/esp8266_rom.bin" };
        for (size_t k = 0; k < SDL_arraysize(names); k++) {
            SDL_snprintf(out, n, "%s%s", dirs[i], names[k]);
            SDL_IOStream *f = SDL_IOFromFile(out, "rb");
            if (f) { SDL_CloseIO(f); return; }
        }
    }
    SDL_snprintf(out, n, "%sesp8266_rom.bin", base);
}
#endif

static void power_on(App *app)
{
    esp_reset(app->s, true);
    esp_audio_init(&app->audio, AUDIO_RATE, esp_now(app->s));
    if (app->audio_stream) SDL_ClearAudioStream(app->audio_stream);
    app->paused = false;
}

static bool load_program(App *app, const char *path)
{
    char err[256];
    if (!app->rom_ok) {
        show_message(app, "No ESP8266 ROM: pass --rom esp8266_rom.bin");
        return false;
    }
    save_flash(app);
    if (!esp_load_program(app->s, path, err, sizeof err)) {
        show_message(app, "%s", err);
        return false;
    }
    SDL_strlcpy(app->program, path, sizeof app->program);
    load_flash_save(app);
    load_rotation(app);         /* how this program's screen was turned last time */
    power_on(app);
    app->loaded = true;

    const char *name = SDL_strrchr(path, '/');
    const char *bname = SDL_strrchr(path, '\\');
    if (bname > name) name = bname;
    name = name ? name + 1 : path;
    char title[512];
    SDL_snprintf(title, sizeof title, "ESPboy Emulator - %s", name);
    SDL_SetWindowTitle(app->window, title);
    show_message(app, "%s", name);
    return true;
}

#ifdef __EMSCRIPTEN__
static App *web_app;

EMSCRIPTEN_KEEPALIVE int espboy_web_load(const char *path)
{
    return web_app && load_program(web_app, path);
}

/* the page fetched the ROM: it is loaded, and a program handed over before
   can start */
EMSCRIPTEN_KEEPALIVE int espboy_web_rom(const char *path)
{
    if (!web_app) return 0;
    char err[256];
    web_app->rom_ok = esp_load_rom(web_app->s, path, err, sizeof err);
    if (!web_app->rom_ok) show_message(web_app, "%s", err);
    return web_app->rom_ok;
}
#endif

#ifndef __EMSCRIPTEN__
static void SDLCALL file_chosen(void *userdata, const char *const *files, int filter)
{
    (void)filter;
    App *app = userdata;
    if (files && files[0])
        load_program(app, files[0]);
}
#endif

static void open_dialog(App *app)
{
#ifdef __EMSCRIPTEN__
    (void)app;
    EM_ASM(document.getElementById('file').click(););
#else
    static const SDL_DialogFileFilter filters[] = {
        { "ESPboy programs", "bin" },
        { "All files", "*" },
    };
    SDL_ShowOpenFileDialog(file_chosen, app, app->window, filters, 2, NULL, false);
#endif
}

static void screenshot(App *app)
{
    SDL_Surface *s = SDL_CreateSurfaceFrom(W, H, SDL_PIXELFORMAT_XRGB8888, app->pixels, W * 4);
    if (!s) return;
    char path[1200], stem[1024];
    program_stem(app, stem, sizeof stem);
    for (int i = 0; i < 1000; i++) {
        SDL_snprintf(path, sizeof path, "%s_%03d.bmp", stem, i);
        SDL_IOStream *probe = SDL_IOFromFile(path, "rb");
        if (!probe) break;
        SDL_CloseIO(probe);
    }
    if (SDL_SaveBMP(s, path)) show_message(app, "Saved %s", path);
    else show_message(app, "Screenshot failed: %s", SDL_GetError());
    SDL_DestroySurface(s);
}

/* ------------------------------------------------------------------------ */
/* GIF recording (F6)                                                        */
/* ------------------------------------------------------------------------ */

typedef struct { uint8_t *data; size_t size; } GifFile;

/* the program's path without its extension, for names of files made from it */
static void program_stem(App *app, char *out, size_t n)
{
    SDL_strlcpy(out, app->loaded ? app->program : "espboy", n);
    char *dot = SDL_strrchr(out, '.');
    char *slash = SDL_strrchr(out, '/');
    char *bslash = SDL_strrchr(out, '\\');
    if (bslash > slash) slash = bslash;
    if (dot && (!slash || dot > slash)) *dot = 0;
}

/* a name next to the program that is not taken yet: <program>_NNN.gif */
static void gif_name(App *app, char *out, size_t n)
{
    char stem[1024];
    program_stem(app, stem, sizeof stem);
    for (int i = 0; i < 1000; i++) {
        SDL_snprintf(out, n, "%s_%03d.gif", stem, i);
        SDL_IOStream *probe = SDL_IOFromFile(out, "rb");
        if (!probe) return;
        SDL_CloseIO(probe);
    }
}

#ifndef __EMSCRIPTEN__
static void SDLCALL gif_save_chosen(void *userdata, const char *const *files, int filter)
{
    (void)filter;
    GifFile *f = userdata;
    if (files && files[0]) {
        char path[1100];
        SDL_strlcpy(path, files[0], sizeof path);
        /* the dialog may leave the extension off */
        const size_t len = SDL_strlen(path);
        if (len < 4 || SDL_strcasecmp(path + len - 4, ".gif")) SDL_strlcat(path, ".gif", sizeof path);
        SDL_IOStream *io = SDL_IOFromFile(path, "wb");
        if (io) {
            SDL_WriteIO(io, f->data, f->size);
            SDL_CloseIO(io);
        }
    }
    free(f->data);
    SDL_free(f);
}
#endif

static void gif_toggle(App *app)
{
    if (!app->recording) {
        if (!gif_begin(&app->gif, W, H)) {
            show_message(app, "GIF: out of memory");
            return;
        }
        app->recording = true;
        app->gif_tick = esp_now(app->s);
        show_message(app, "Recording a GIF - F6 to stop");
        return;
    }
    app->recording = false;
    size_t size = 0;
    uint8_t *data = gif_end(&app->gif, &size);
    if (!data) {
        show_message(app, "GIF: nothing recorded");
        return;
    }
    char name[1100];
    gif_name(app, name, sizeof name);
#ifdef __EMSCRIPTEN__
    /* the browser asks where to save it (or downloads it) */
    const char *base = SDL_strrchr(name, '/');
    EM_ASM({
        var bytes = HEAPU8.slice($0, $0 + $1);
        var name = UTF8ToString($2);
        var blob = new Blob([bytes], { type: 'image/gif' });
        function download() {
            var a = document.createElement('a');
            a.href = URL.createObjectURL(blob);
            a.download = name;
            a.style.display = 'none';
            document.body.appendChild(a);
            a.click();
            setTimeout(function () { URL.revokeObjectURL(a.href); a.remove(); }, 10000);
        }
        if (window.showSaveFilePicker) {
            window.showSaveFilePicker({ suggestedName: name, types: [{ description: 'GIF image', accept: { 'image/gif': ['.gif'] } }] })
                .then(function (h) { return h.createWritable(); })
                .then(function (w) { return w.write(blob).then(function () { return w.close(); }); })
                .catch(function (e) { if (e.name !== 'AbortError') download(); });
        } else {
            download();
        }
    }, data, (int)size, base ? base + 1 : name);
    free(data);
    show_message(app, "GIF recorded");
#else
    GifFile *f = SDL_malloc(sizeof(GifFile));
    if (!f) { free(data); return; }
    f->data = data;
    f->size = size;
    static const SDL_DialogFileFilter filters[] = { { "GIF images", "gif" } };
    SDL_ShowSaveFileDialog(gif_save_chosen, f, app->window, filters, 1, name);
    show_message(app, "GIF recorded - choose where to save it");
#endif
}

/* this frame of the screen, as long as emulated time has gone on since the last */
static void gif_capture(App *app)
{
    if (!app->recording) return;
    const uint64_t now = esp_now(app->s);
    const double secs = now > app->gif_tick ? (double)(now - app->gif_tick) / ESP_TICK_HZ : 0.0;
    app->gif_tick = now;
    gif_frame(&app->gif, app->pixels, secs);
    if (app->gif.failed) {
        gif_abort(&app->gif);
        app->recording = false;
        show_message(app, "GIF: out of memory, recording stopped");
    }
}

/* The d-pad as the turned screen has it (R): a direction pressed is the
   one that points that way on the window. With the screen a quarter turn
   clockwise the device's top is on the right, so up on the keyboard is the
   device's left, right is its up, and so on. The other buttons stay */
static uint8_t rotate_dpad(uint8_t b, int rotation)
{
    static const uint8_t ring[4] = { KEY_UP, KEY_RIGHT, KEY_DOWN, KEY_LEFT };   /* clockwise */
    if (!rotation) return b;
    uint8_t out = b & (uint8_t)~(KEY_UP | KEY_RIGHT | KEY_DOWN | KEY_LEFT);
    for (int i = 0; i < 4; i++)
        if (b & ring[i]) out |= ring[(i - rotation + 4) % 4];
    return out;
}

static void read_input(App *app)
{
    const bool *k = SDL_GetKeyboardState(NULL);
    uint8_t b = 0;
    if (k[SDL_SCANCODE_UP]) b |= KEY_UP;
    if (k[SDL_SCANCODE_DOWN]) b |= KEY_DOWN;
    if (k[SDL_SCANCODE_LEFT]) b |= KEY_LEFT;
    if (k[SDL_SCANCODE_RIGHT]) b |= KEY_RIGHT;
    if (k[SDL_SCANCODE_X] || k[SDL_SCANCODE_SPACE] || k[SDL_SCANCODE_K]) b |= KEY_ACT;
    if (k[SDL_SCANCODE_Z] || k[SDL_SCANCODE_J]) b |= KEY_ESC;
    /* the side buttons: the keys above ESC (Z) and ACT (X) */
    if (k[SDL_SCANCODE_A]) b |= KEY_LFT;
    if (k[SDL_SCANCODE_S]) b |= KEY_RGT;
    if (k[SDL_SCANCODE_RETURN] && !(SDL_GetModState() & SDL_KMOD_ALT)) b |= KEY_ACT;
    for (int i = 0; i < 8; i++) {
        SDL_Gamepad *g = app->pads[i];
        if (!g) continue;
        if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_UP)) b |= KEY_UP;
        if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_DOWN)) b |= KEY_DOWN;
        if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_LEFT)) b |= KEY_LEFT;
        if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) b |= KEY_RIGHT;
        if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_SOUTH)) b |= KEY_ACT;
        if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_EAST)) b |= KEY_ESC;
        if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)) b |= KEY_LFT;
        if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) b |= KEY_RGT;
        if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_START)) b |= KEY_ACT;
        if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_BACK)) b |= KEY_ESC;
        const float gx = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0f;
        const float gy = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0f;
        if (gx < -0.5f) b |= KEY_LEFT;
        if (gx > 0.5f) b |= KEY_RIGHT;
        if (gy < -0.5f) b |= KEY_UP;
        if (gy > 0.5f) b |= KEY_DOWN;
    }
    app->s->buttons = rotate_dpad(b, app->rotation);
}

/* The speaker's samples to the audio device (or dropped) */
static void feed_audio(App *app, bool play)
{
    Esp *s = app->s;
    if (!play || !app->audio_stream) {
        esp_audio_skip(&app->audio, s);
        return;
    }
    app->audio.volume = app->volume;
    int n;
    while ((n = esp_audio_render(&app->audio, s, app->samples, (int)SDL_arraysize(app->samples))) > 0)
        SDL_PutAudioStreamData(app->audio_stream, app->samples, n * (int)sizeof(float));
}

/* Runs the machine for this frame */
static void emulate(App *app)
{
    const Uint64 now = SDL_GetTicksNS();
    const double elapsed = (double)(now - app->last_ticks) / 1e9;
    app->last_ticks = now;
    if (!app->loaded || app->paused)
        return;

    Esp *s = app->s;
    read_input(app);
    const Uint64 run0 = SDL_GetTicksNS();
    const bool fast = SDL_GetKeyboardState(NULL)[SDL_SCANCODE_TAB];

    if (fast) {
        esp_run(s, (uint64_t)(SDL_min(elapsed, MAX_SLICE_S) * ESP_TICK_HZ * FAST_FORWARD));
        feed_audio(app, false);
    } else if (app->audio_stream) {
        /* run for as long as the queued sound falls short */
        const double queued = SDL_GetAudioStreamQueued(app->audio_stream) / (double)sizeof(float) / AUDIO_RATE;
        const double need = SDL_min(AUDIO_TARGET_S - queued, MAX_SLICE_S);
        if (need > 0) esp_run(s, (uint64_t)(need * ESP_TICK_HZ));
        feed_audio(app, true);
    } else {
        app->wall_carry += SDL_min(elapsed, MAX_SLICE_S) * ESP_TICK_HZ;
        const uint64_t ticks = (uint64_t)app->wall_carry;
        app->wall_carry -= (double)ticks;
        esp_run(s, ticks);
        feed_audio(app, false);
    }
    app->st.run_ns += SDL_GetTicksNS() - run0;

    if (SDL_GetTicksNS() - app->saved_at > SAVE_NS) {
        save_flash(app);
        app->saved_at = SDL_GetTicksNS();
    }
}

static void draw_text(SDL_Renderer *r, float x, float y, const char *s)
{
    SDL_RenderDebugText(r, x, y, s);
}

static void update_stats(App *app)
{
    Stats *st = &app->st;
    Esp *s = app->s;
    st->frames++;
    const Uint64 now = SDL_GetTicksNS();
    const double secs = (double)(now - st->t0) / 1e9;
    if (secs < 1.0)
        return;
    const double emu = (double)(esp_now(s) - st->now0) / ESP_TICK_HZ;
    const double run = (double)st->run_ns / 1e9;
    st->render_fps = (float)(st->frames / secs);
    st->game_fps = (float)((s->lcd.frame_starts - st->lcd0) / secs);
    st->speed = (float)(emu / secs * 100.0);
    st->host_load = (float)(run / secs * 100.0);
    st->max_speed = run > 0 ? (float)(emu / run) : 0;
    st->t0 = now;
    st->run_ns = 0;
    st->frames = 0;
    st->now0 = esp_now(s);
    st->lcd0 = s->lcd.frame_starts;
}

static void draw_stats(App *app, int w)
{
    const Stats *st = &app->st;
    const Esp *s = app->s;
    char lines[8][64];
    int n = 0;
    SDL_snprintf(lines[n++], 64, "game   %5.1f fps", st->game_fps);
    SDL_snprintf(lines[n++], 64, "speed  %5.1f%%  cpu %u MHz", st->speed, s->cpu_hz / 1000000u);
    SDL_snprintf(lines[n++], 64, "host   %5.1f%% cpu  max %.1fx", st->host_load, st->max_speed);
    SDL_snprintf(lines[n++], 64, "render %5.1f fps", st->render_fps);
    SDL_snprintf(lines[n++], 64, "backlight %3d%%  led %06X", (int)(backlight_level(&s->i2c) * 100), s->led_rgb);
    SDL_snprintf(lines[n++], 64, "pc %08X", s->core.pc);
    SDL_Renderer *r = app->renderer;
    float bw = 0;
    for (int i = 0; i < n; i++) bw = SDL_max(bw, (float)SDL_strlen(lines[i]) * 8);
    const float x = (float)w - bw - 12;
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, 0, 0, 0, 170);
    SDL_FRect box = { x - 4, 4, bw + 8, 12.0f * (float)n + 6 };
    SDL_RenderFillRect(r, &box);
    SDL_SetRenderDrawColor(r, 120, 255, 120, 255);
    for (int i = 0; i < n; i++)
        draw_text(r, x, 8 + 12.0f * (float)i, lines[i]);
}

/* The picture turned clockwise by app->rotation quarter turns (R), so what
   is shown, recorded (F6) and saved (F10) is the screen as the player holds it */
static void rotate_screen(App *app)
{
    static uint32_t turned[W * H];
    if (!app->rotation) return;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int tx, ty;
            switch (app->rotation) {
            case 1: tx = H - 1 - y; ty = x; break;            /* 90 degrees clockwise */
            case 2: tx = W - 1 - x; ty = H - 1 - y; break;    /* 180 */
            default: tx = y; ty = W - 1 - x; break;           /* 270 */
            }
            turned[ty * W + tx] = app->pixels[y * W + x];
        }
    SDL_memcpy(app->pixels, turned, sizeof turned);
}

/* The glass as it is lit: the panel's picture times the backlight */
static void render_screen(App *app)
{
    st7735_render(&app->s->lcd, app->pixels);
    rotate_screen(app);
    const float bl = backlight_level(&app->s->i2c);
    if (bl >= 0.999f) return;
    const uint32_t k = (uint32_t)(bl * 256.0f);
    for (int i = 0; i < W * H; i++) {
        const uint32_t c = app->pixels[i];
        app->pixels[i] = (((c >> 16 & 255) * k >> 8) << 16) | (((c >> 8 & 255) * k >> 8) << 8) | ((c & 255) * k >> 8);
    }
}

SDL_AppResult SDL_AppInit(void **appstate, int argc, char *argv[])
{
    SDL_SetAppMetadata("ESPboy Emulator", "0.1", "com.joyrider3774.espboy_emulator");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        SDL_Log("SDL_Init: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }
    App *app = SDL_calloc(1, sizeof(App));
    if (!app) return SDL_APP_FAILURE;
    app->s = esp_create();
    if (!app->s) return SDL_APP_FAILURE;
    *appstate = app;
    app->volume = 1.0f;
    load_settings(app);

    const int scale = 4;
    if (!SDL_CreateWindowAndRenderer("ESPboy Emulator", W * scale, H * scale + BAR_H,
                                     SDL_WINDOW_RESIZABLE, &app->window, &app->renderer)) {
        SDL_Log("window: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }
    SDL_SetRenderVSync(app->renderer, 1);
    app->screen = SDL_CreateTexture(app->renderer, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, W, H);

    SDL_AudioSpec spec = { SDL_AUDIO_F32, 1, AUDIO_RATE };
    app->audio_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (app->audio_stream) SDL_ResumeAudioStreamDevice(app->audio_stream);
    else SDL_Log("no audio: %s", SDL_GetError());
    esp_audio_init(&app->audio, AUDIO_RATE, 0);

    const char *program = NULL;
    for (int i = 1; i < argc; i++) {
        if (!SDL_strcmp(argv[i], "--rom") && i + 1 < argc) SDL_strlcpy(app->rom, argv[++i], sizeof app->rom);
        else if (!SDL_strcmp(argv[i], "--integer-scale")) app->integer_scale = true;
        else if (!SDL_strcmp(argv[i], "--no-integer-scale")) app->integer_scale = false;
        else if (!SDL_strcmp(argv[i], "--speaker")) app->audio.speaker = true;
        else if (!SDL_strcmp(argv[i], "--no-speaker")) app->audio.speaker = false;
        else if (argv[i][0] != '-') program = argv[i];
    }
    set_scale_mode(app);

#ifdef __EMSCRIPTEN__
    /* saves live in IndexedDB, read in by the page before it hands over the
       first program; the ROM is fetched by the page (espboy_web_rom) */
    web_app = app;
    EM_ASM(
        FS.mkdir('/espboy');
        FS.mkdir('/espboy/saves');
        FS.mount(IDBFS, {}, '/espboy/saves');
    );
#else
    if (!app->rom[0] && !find_rom(app->rom, sizeof app->rom))
        find_rom_near_app(app->rom, sizeof app->rom);
    char err[256];
    app->rom_ok = esp_load_rom(app->s, app->rom, err, sizeof err);
#endif

    if (program) load_program(app, program);
    if (!app->loaded) {
#ifdef __EMSCRIPTEN__
        show_message(app, "Open an ESPboy .bin to start");
#else
        show_message(app, app->rom_ok ? "Drop an ESPboy .bin here, or press F3"
                                      : "No ESP8266 ROM found: pass --rom esp8266_rom.bin");
#endif
    }
    app->last_ticks = SDL_GetTicksNS();
    app->st.t0 = SDL_GetTicksNS();
    app->saved_at = SDL_GetTicksNS();
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *e)
{
    App *app = appstate;
    switch (e->type) {
    case SDL_EVENT_QUIT:
        return SDL_APP_SUCCESS;
    case SDL_EVENT_DROP_FILE:
        load_program(app, e->drop.data);
        break;
    case SDL_EVENT_GAMEPAD_ADDED:
        for (int i = 0; i < 8; i++)
            if (!app->pads[i]) { app->pads[i] = SDL_OpenGamepad(e->gdevice.which); break; }
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        for (int i = 0; i < 8; i++)
            if (app->pads[i] && SDL_GetGamepadID(app->pads[i]) == e->gdevice.which) {
                SDL_CloseGamepad(app->pads[i]);
                app->pads[i] = NULL;
            }
        break;
    case SDL_EVENT_KEY_DOWN:
        if (e->key.repeat) break;
        switch (e->key.key) {
#ifndef __EMSCRIPTEN__
        case SDLK_ESCAPE: return SDL_APP_SUCCESS;
#endif
        case SDLK_F1: app->help = !app->help; break;
        case SDLK_F2:
            if (app->loaded) {
                save_flash(app);
                power_on(app);
                show_message(app, "Reset");
            }
            break;
        case SDLK_F3: open_dialog(app); break;
        case SDLK_R:
            app->rotation = (app->rotation + 1) & 3;
            save_rotation(app);
            show_message(app, "Screen turned %d degrees", app->rotation * 90);
            break;
        case SDLK_P:
            app->paused = !app->paused;
            show_message(app, app->paused ? "Paused" : "Running");
            break;
        case SDLK_F8:
            app->integer_scale = !app->integer_scale;
            set_scale_mode(app);
            save_settings(app);
            show_message(app, app->integer_scale ? "Scaling: whole multiples" : "Scaling: fill the window");
            break;
        case SDLK_F6: gif_toggle(app); break;
        case SDLK_F7:
            app->audio.speaker = !app->audio.speaker;
            save_settings(app);
            show_message(app, app->audio.speaker ? "Sound: through the speaker" : "Sound: the bare pin signal");
            break;
        case SDLK_F9: app->stats = !app->stats; break;
        case SDLK_F10: screenshot(app); break;
        case SDLK_F11:
            SDL_SetWindowFullscreen(app->window, !(SDL_GetWindowFlags(app->window) & SDL_WINDOW_FULLSCREEN));
            break;
        case SDLK_RETURN:
            if (e->key.mod & SDL_KMOD_ALT)
                SDL_SetWindowFullscreen(app->window, !(SDL_GetWindowFlags(app->window) & SDL_WINDOW_FULLSCREEN));
            break;
        case SDLK_EQUALS: case SDLK_PLUS: case SDLK_KP_PLUS:
            app->volume = SDL_min(app->volume + 0.1f, 2.0f);
            save_settings(app);
            show_message(app, "Volume %d%%", (int)(app->volume * 100 + 0.5f));
            break;
        case SDLK_MINUS: case SDLK_KP_MINUS:
            app->volume = SDL_max(app->volume - 0.1f, 0.0f);
            save_settings(app);
            show_message(app, "Volume %d%%", (int)(app->volume * 100 + 0.5f));
            break;
        }
        break;
    }
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void *appstate)
{
    App *app = appstate;
    emulate(app);

    SDL_Renderer *r = app->renderer;
    int w, h;
    SDL_GetCurrentRenderOutputSize(r, &w, &h);
    SDL_SetRenderDrawColor(r, 24, 24, 28, 255);
    SDL_RenderClear(r);

    const bool full = (SDL_GetWindowFlags(app->window) & SDL_WINDOW_FULLSCREEN) != 0;
    const int bar = full ? 0 : BAR_H;
    const int avail_h = h - bar;
    const int scale = SDL_min(w / W, avail_h / H);
    float dw, dh;
    if (app->integer_scale && scale >= 1) { dw = (float)(scale * W); dh = (float)(scale * H); }
    else {
        const float f = SDL_min((float)w / W, (float)avail_h / H);
        dw = W * f; dh = H * f;
    }
    SDL_FRect dst = { (w - dw) / 2.0f, (avail_h - dh) / 2.0f, dw, dh };

    render_screen(app);
    gif_capture(app);
    SDL_UpdateTexture(app->screen, NULL, app->pixels, W * 4);
    SDL_RenderTexture(r, app->screen, NULL, &dst);

    if (!full) {
        const float by = (float)(h - bar);
        SDL_SetRenderDrawColor(r, 40, 40, 46, 255);
        SDL_FRect barRect = { 0, by, (float)w, (float)bar };
        SDL_RenderFillRect(r, &barRect);
        /* the RGB LED */
        const uint32_t led = app->s->led_rgb;
        SDL_SetRenderDrawColor(r, 20, 20, 20, 255);
        SDL_FRect ledRect = { 6, by + 5, 14, 14 };
        SDL_RenderFillRect(r, &ledRect);
        if (led) {
            SDL_SetRenderDrawColor(r, (Uint8)SDL_min(255, (led >> 16 & 255) * 4), (Uint8)SDL_min(255, (led >> 8 & 255) * 4),
                                   (Uint8)SDL_min(255, (led & 255) * 4), 255);
            SDL_FRect on = { 8, by + 7, 10, 10 };
            SDL_RenderFillRect(r, &on);
        }
        SDL_SetRenderDrawColor(r, 200, 200, 200, 255);
        char status[160];
        SDL_snprintf(status, sizeof status, "%s%s%.0f fps  %.0f%%", app->recording ? "REC  " : "",
                     app->paused ? "PAUSED  " : "", app->st.game_fps, app->st.speed);
        draw_text(r, 28, by + 8, status);
        draw_text(r, (float)w - 8 * 9, by + 8, "F1: help");
    }

    if (app->help) {
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(r, 0, 0, 0, 210);
        SDL_FRect all = { 0, 0, (float)w, (float)h };
        SDL_RenderFillRect(r, &all);
        SDL_SetRenderDrawColor(r, 230, 230, 230, 255);
        for (size_t i = 0; i < SDL_arraysize(help_text); i++)
            draw_text(r, 16, 16 + 12.0f * (float)i, help_text[i]);
    }
    if (app->stats)
        draw_stats(app, w);
    if (SDL_GetTicks() < app->message_until) {
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
        const float tw = (float)SDL_strlen(app->message) * 8;
        SDL_SetRenderDrawColor(r, 0, 0, 0, 180);
        SDL_FRect box = { 4, 4, tw + 8, 16 };
        SDL_RenderFillRect(r, &box);
        SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
        draw_text(r, 8, 8, app->message);
    }
    SDL_RenderPresent(r);

    update_stats(app);
    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void *appstate, SDL_AppResult result)
{
    (void)result;
    App *app = appstate;
    if (!app) return;
    if (app->recording) gif_abort(&app->gif);
    if (app->s) {
        save_flash(app);
        esp_destroy(app->s);
    }
    SDL_free(app);
}
