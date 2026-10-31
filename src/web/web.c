/*
 * web.c - what the browser build needs before the game starts: its files.
 *
 * The game folder is served beside the engine's script, under data/, with
 * a data/manifest.json naming every file in it (tools/web_manifest.py
 * writes it). WASMFS's fetch backend reads each file from the server a piece at a
 * time as the game opens it, so nothing is downloaded that is not played
 * and the game starts at once; the names come from the manifest, since a
 * web server cannot list a directory, and win32hle's path layer matches
 * the game's requests against them without case. What the game writes
 * (its settings and scores: cstats.cfg, scores.cfg) goes to the browser's
 * Origin Private File System, save/, laid over the game folder as
 * win32hle's overlay, so nothing is lost on reload.
 *
 * Runs on the game's thread (PROXY_TO_PTHREAD): WASMFS's fetch and OPFS
 * backends cannot be made on the browser's main thread, and the manifest is
 * read with a synchronous request, which a worker may make.
 *
 * After sp00nznet/redalert2-recomp's src/web/web.c (MIT).
 */
#ifdef __EMSCRIPTEN__
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <strings.h>
#include <pthread.h>
#include <emscripten.h>
#include <emscripten/wasmfs.h>
#include <emscripten/threading.h>
#include "web.h"

#define FETCH_CHUNK (1u << 20)

EM_JS(char *, web_base_url, (void), {
    // The worker's script (hotd1.js) and the game folder (data/) share a directory.
    var u = new URL('./', self.location.href);
    return stringToNewUTF8(u.href);
});
/* The manifest, read synchronously (this is a worker): the text, or NULL. */
EM_JS(char *, web_fetch_text, (const char *url), {
    try {
        var x = new XMLHttpRequest();
        x.open('GET', UTF8ToString(url), false);
        x.send(null);
        if (x.status < 200 || x.status >= 300) return 0;
        return stringToNewUTF8(x.responseText);
    } catch (e) { return 0; }
});
EM_JS(void, web_status, (const char *text), {
    err('[web] ' + UTF8ToString(text));
});
void web_set_status(const char *text) { web_status(text); }

/* mkdir -p for a path inside the fetched tree, each directory on the backend */
static void make_dirs(const char *path, backend_t be) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s", path);
    char *slash = strrchr(tmp, '/');
    if (!slash || slash == tmp) return;
    *slash = 0;
    struct stat st;
    if (stat(tmp, &st) == 0) return;
    make_dirs(tmp, be);
    wasmfs_create_directory(tmp, 0777, be);
}

/* The manifest: a JSON array of paths relative to game/, with either slash
 * ("ART/FOO.BIN"), or an object {"files": [...]}. Only the strings matter. */
static int build_tree(const char *json, backend_t be, const char *root) {
    int n = 0;
    const char *p = json;
    while ((p = strchr(p, '"')) != NULL) {
        const char *e = strchr(p + 1, '"');
        if (!e) break;
        size_t len = (size_t)(e - p - 1);
        char name[PATH_MAX];
        if (len && len < sizeof name - 1 && !(len == 5 && !strncmp(p + 1, "files", 5))) {
            memcpy(name, p + 1, len), name[len] = 0;
            for (char *c = name; *c; c++) if (*c == '\\') *c = '/';
            if (name[0] != '/' && !strstr(name, "..") && !strchr(name, ':') && name[len - 1] != '/') {
                char path[PATH_MAX];
                snprintf(path, sizeof path, "%s/%s", root, name);
                make_dirs(path, be);
                if (wasmfs_create_file(path, 0666, be) >= 0) n++;   /* a descriptor, or -1 */
            }
        }
        p = e + 1;
    }
    return n;
}

static int has_file(const char *dir, const char *name) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *e;
    int found = 0;
    while (!found && (e = readdir(d))) found = strcasecmp(e->d_name, name) == 0;
    closedir(d);
    return found;
}

/* The fonts a Windows face is drawn with (gdidc.c looks here): the game's
 * Options dialogs are Win32 dialogs, their text drawn through GDI. */
static const char *const k_fonts[] = {
    "LiberationSans-Regular.ttf", "LiberationSans-Bold.ttf", "LiberationSans-Italic.ttf", "LiberationSans-BoldItalic.ttf",
    "LiberationSerif-Regular.ttf", "LiberationSerif-Bold.ttf", "LiberationSerif-Italic.ttf", "LiberationSerif-BoldItalic.ttf",
    "LiberationMono-Regular.ttf", "LiberationMono-Bold.ttf", "LiberationMono-Italic.ttf", "LiberationMono-BoldItalic.ttf" };

int web_mount(const char *exe_name, char *game_dir, size_t gn, char *save_dir, size_t sn) {
    char *base = web_base_url();
    if (!base) return 0;
    char url[1024];

    /* the fonts, from beside the engine (fonts/) */
    snprintf(url, sizeof url, "%sfonts", base);
    backend_t fontfb = wasmfs_create_fetch_backend(url, FETCH_CHUNK);
    if (fontfb) {
        mkdir("/usr", 0777); mkdir("/usr/share", 0777); mkdir("/usr/share/fonts", 0777); mkdir("/usr/share/fonts/truetype", 0777);
        if (wasmfs_create_directory("/usr/share/fonts/truetype/liberation", 0777, fontfb) == 0)
            for (size_t i = 0; i < sizeof k_fonts / sizeof *k_fonts; i++) {
                char path[256];
                snprintf(path, sizeof path, "/usr/share/fonts/truetype/liberation/%s", k_fonts[i]);
                wasmfs_create_file(path, 0666, fontfb);
            }
    }

    /* the game folder, from beside the page */
    snprintf(url, sizeof url, "%sdata", base);
    backend_t fb = wasmfs_create_fetch_backend(url, FETCH_CHUNK);
    if (!fb || wasmfs_create_directory("/game", 0777, fb) != 0) {
        fprintf(stderr, "[web] no fetch backend for %s\n", url);
        free(base);
        return 0;
    }
    snprintf(url, sizeof url, "%sdata/manifest.json", base);
    char *manifest = web_fetch_text(url);
    if (!manifest) {
        fprintf(stderr, "[web] no %s\n", url);
        web_set_status("No game files on this site (data/manifest.json).");
        free(base);
        return 0;
    }
    int n = build_tree(manifest, fb, "/game");
    free(manifest);
    fprintf(stderr, "[web] game folder: %d files from %sdata/\n", n, base);

    /* the browser's own storage for what the game writes */
    backend_t opfs = wasmfs_create_opfs_backend();
    int have_opfs = opfs && wasmfs_create_directory("/opfs", 0777, opfs) == 0;
    if (have_opfs) {
        mkdir("/opfs/save", 0777);
        snprintf(save_dir, sn, "/opfs/save");
    } else {
        fprintf(stderr, "[web] no OPFS: settings and scores will not persist\n");
        mkdir("/save", 0777);
        snprintf(save_dir, sn, "/save");
    }
    free(base);
    if (!has_file("/game", exe_name)) {
        fprintf(stderr, "[web] the game folder has no %s\n", exe_name);
        web_set_status("The game folder on this site is incomplete.");
        return 0;
    }
    snprintf(game_dir, gn, "/game");
    web_set_status("Starting...");
    return 1;
}

/* WASMFS cannot name a directory that is a backend's root (its parent's
 * getName finds nothing), so getcwd() inside /game answered "/". The host
 * absolutises relative paths with getcwd, so chdir and getcwd are kept
 * here: chdir does the real one and remembers the path. */
int __syscall_chdir(const char *path);
static char g_cwd[PATH_MAX] = "/";
static pthread_mutex_t g_cwd_lock = PTHREAD_MUTEX_INITIALIZER;
int chdir(const char *path) {
    int r = __syscall_chdir(path);
    if (r < 0) { errno = -r; return -1; }
    char joined[PATH_MAX * 2], out[PATH_MAX] = "/";
    pthread_mutex_lock(&g_cwd_lock);
    if (path[0] == '/') snprintf(joined, sizeof joined, "%s", path);
    else snprintf(joined, sizeof joined, "%s/%s", g_cwd, path);
    size_t o = 1;
    for (const char *p = joined; *p;) {
        while (*p == '/') p++;
        const char *e = p;
        while (*e && *e != '/') e++;
        size_t len = (size_t)(e - p);
        if (len == 2 && p[0] == '.' && p[1] == '.') { while (o > 1 && out[o - 1] != '/') o--; if (o > 1) o--; }
        else if (len && !(len == 1 && p[0] == '.') && o + len + 1 < sizeof out) {
            if (o > 1) out[o++] = '/';
            memcpy(out + o, p, len); o += len;
        }
        p = e;
    }
    out[o] = 0;
    snprintf(g_cwd, sizeof g_cwd, "%s", out);
    pthread_mutex_unlock(&g_cwd_lock);
    return 0;
}
char *getcwd(char *buf, size_t n) {
    pthread_mutex_lock(&g_cwd_lock);
    size_t l = strlen(g_cwd);
    if (!buf) { buf = malloc(n ? n : l + 1); n = n ? n : l + 1; }
    if (!buf || l >= n) { pthread_mutex_unlock(&g_cwd_lock); errno = buf ? ERANGE : ENOMEM; return NULL; }
    memcpy(buf, g_cwd, l + 1);
    pthread_mutex_unlock(&g_cwd_lock);
    return buf;
}

/* The pump's chance to run what the browser's main thread queued for this
 * one (input events SDL registered from here). */
void web_pump(void) { emscripten_current_thread_process_queued_calls(); }
#endif
