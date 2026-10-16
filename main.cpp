#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <algorithm>
constexpr uint32_t BG_COLOR = 0x000000BB;
constexpr uint32_t FG_COLOR = 0xcdd6f4ff;
constexpr uint32_t SEL_BG = 0x6f44f4ff;
constexpr uint32_t SEL_FG = 0x11111bff;
constexpr const char *FONT_NAME = "fixed";
constexpr int BAR_HEIGHT = 24;
constexpr int PADDING = 8;
constexpr unsigned long to_x11_pixel(uint32_t rgba, bool has_alpha_visual) {
    const uint8_t r = (rgba >> 24) & 0xFF;
    const uint8_t g = (rgba >> 16) & 0xFF;
    const uint8_t b = (rgba >> 8) & 0xFF;
    const uint8_t a = rgba & 0xFF;
    if (!has_alpha_visual)return static_cast<unsigned long>((r << 16) | (g << 8) | b);
    return (static_cast<unsigned long>(a) << 24) | (r << 16) | (g << 8) | b;
}

struct CacheHeader {
    uint32_t count;
    uint32_t total_str_bytes;
};
static void scan_cache(const char *filepath) {
    const char *env_path = getenv("PATH");
    if (!env_path)return;
    std::vector<std::string> bins;
    bins.reserve(2048);
    char *paths = strdup(env_path);
    if (!paths)return;
    char *saveptr = nullptr;
    for (char *dir = strtok_r(paths, ":", &saveptr); dir; dir = strtok_r(nullptr, ":", &saveptr)) {
        DIR * dp = opendir(dir);
        if (!dp)continue;
        while (struct dirent * ep = readdir(dp)) {
            if (ep->d_name[0] == '.')continue;
            std::string full = std::string(dir) + "/" + ep->d_name;
            if (access(full.c_str(), X_OK) == 0)bins.emplace_back(ep->d_name);
        }
        closedir(dp);
    }
    free(paths);
    std::sort(bins.begin(), bins.end());
    bins.erase(std::unique(bins.begin(), bins.end()), bins.end());
    CacheHeader header {
        static_cast<uint32_t>(bins.size()), 0
    };
    for (const auto &b : bins)header.total_str_bytes += (uint32_t)b.size() + 1;
    const size_t offsets_bytes = (size_t)header.count * sizeof(uint32_t);
    const size_t total_bytes = sizeof(CacheHeader) + offsets_bytes + header.total_str_bytes;
    int fd = open(filepath, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)return;
    if (ftruncate(fd, (off_t)total_bytes) != 0) {
        close(fd);
        return;
    }
    auto *mem = static_cast<uint8_t * >(mmap(nullptr, total_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    if (mem != MAP_FAILED) {
        memcpy(mem, &header, sizeof(CacheHeader));
        auto *offsets = reinterpret_cast<uint32_t * >(mem + sizeof(CacheHeader));
        char *pool = reinterpret_cast<char * >(mem + sizeof(CacheHeader) + offsets_bytes);
        uint32_t cur = 0;
        for (size_t i = 0; i < bins.size(); ++i) {
            offsets[i] = cur;
            const size_t n = bins[i].size() + 1;
            memcpy(pool + cur, bins[i].c_str(), n);
            cur += (uint32_t)n;
        }
        munmap(mem, total_bytes);
    }
    close(fd);
}

static void print_help(const char *prog) {
    printf("Usage: %s [option]\n\n", prog);
    printf("Options:\n");
    printf("  (none)   Launch the launcher (uses cache, builds it if missing)\n");
    printf("  -r       Rescan PATH to rebuild the cache, then launch\n");
    printf("  -e       Rescan PATH to rebuild the cache and exit without launching.\n");
    printf("           Use from your WM's exec/autostart to warm the cache.\n");
    printf("  -h       Print this help and exit\n\n");
    printf("Controls (when launched):\n");
    printf("  <type>       Fuzzy-filter entries (chars in order)\n");
    printf("  Tab          Complete input to selected entry\n");
    printf("  Up / Down    Select previous / next match\n");
    printf("  Left / Right Select previous / next match\n");
    printf("  Backspace    Delete last character\n");
    printf("  Enter        Launch selected entry\n");
    printf("  Escape       Quit without launching\n");
}

struct Match {
    uint32_t idx;
    int score;
};
static inline char lower_ascii(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
}

static bool fuzzy_match(const char *name, const char *query, int &out_score) {
    out_score = 0;
    if (! *query)return true;
    int score = 0;
    int streak = 0;
    const char *n = name;
    const char *prev_hit = nullptr;
    for (const char *q = query; *q; ++q) {
        const char qc = lower_ascii( *q);
        const char *hit = nullptr;
        for (const char *s = n; *s; ++s) {
            if (lower_ascii( *s) == qc) {
                hit = s;
                break;
            }
        }
        if (!hit)return false;
        const bool at_start = (hit == name);
        const bool at_sep = hit > name && (hit[ -1] == '-' || hit[ -1] == '_' || hit[ -1] == '/' || hit[ -1] == '.');
        const bool consecutive = prev_hit && (hit == prev_hit + 1);
        if (at_start)score += 100;
        else if (at_sep)score += 50;
        if (consecutive) {
            streak++;
            score += 10 + streak * 5;
        } else streak = 0;
        score += 5;
        prev_hit = hit;
        n = hit + 1;
    }
    score -= static_cast<int>(strlen(name)) / 4;
    out_score = score;
    return true;
}

int main(int argc, char *argv[]) {
    bool force_refresh = false;
    bool exit_after = false;
    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        if (a[0] != '-' || a[1] == '\0')continue;
        if (!strcmp(a, "-r"))force_refresh = true;
        else if (!strcmp(a, "-e")) {
            force_refresh = true;
            exit_after = true;
        } else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            print_help(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", a);
            print_help(argv[0]);
            return 1;
        }
    }
    char cache_file[64];
    snprintf(cache_file, sizeof(cache_file), "/dev/shm/mothl%u.cache", (unsigned)getuid());
    if (force_refresh)scan_cache(cache_file);
    if (exit_after)return 0;
    int fd = open(cache_file, O_RDONLY);
    if (fd < 0) {
        scan_cache(cache_file);
        fd = open(cache_file, O_RDONLY);
        if (fd < 0)return 1;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || (size_t)st.st_size < sizeof(CacheHeader)) {
        close(fd);
        return 1;
    }
    void *mapped = mmap(nullptr, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (mapped == MAP_FAILED)return 1;
    const auto *header = static_cast<const CacheHeader * >(mapped);
    const auto *offsets = reinterpret_cast<const uint32_t * >(static_cast<const uint8_t * >(mapped) + sizeof(CacheHeader));
    const char *str_pool = reinterpret_cast<const char * >(static_cast<const uint8_t * >(mapped) + sizeof(CacheHeader) + (size_t)header->count * sizeof(uint32_t));
    Display * dpy = XOpenDisplay(nullptr);
    if (!dpy) {
        munmap(mapped, (size_t)st.st_size);
        return 1;
    }
    const int scr = DefaultScreen(dpy);
    const Window root = RootWindow(dpy, scr);
    const int screen_w = DisplayWidth(dpy, scr);
    XVisualInfo vinfo;
    const bool has_alpha = XMatchVisualInfo(dpy, scr, 32, TrueColor, &vinfo) != 0;
    Visual * vis = has_alpha ? vinfo.visual : DefaultVisual(dpy, scr);
    const int depth = has_alpha ? 32 : DefaultDepth(dpy, scr);
    Colormap cm = has_alpha ? XCreateColormap(dpy, root, vis, AllocNone) : DefaultColormap(dpy, scr);
    const unsigned long px_bg = to_x11_pixel(BG_COLOR, has_alpha);
    const unsigned long px_fg = to_x11_pixel(FG_COLOR, has_alpha);
    const unsigned long px_sbg = to_x11_pixel(SEL_BG, has_alpha);
    const unsigned long px_sfg = to_x11_pixel(SEL_FG, has_alpha);
    XSetWindowAttributes wa {
    };
    wa.override_redirect = True;
    wa.colormap = cm;
    wa.border_pixel = 0;
    wa.background_pixel = px_bg;
    wa.event_mask = ExposureMask | KeyPressMask | FocusChangeMask;
    Window win = XCreateWindow(dpy, root, 0, 0, (unsigned)screen_w, BAR_HEIGHT, 0, depth, InputOutput, vis, CWOverrideRedirect | CWColormap | CWBorderPixel | CWBackPixel | CWEventMask, &wa);
    XMapRaised(dpy, win);
    XFlush(dpy);
    XSetInputFocus(dpy, win, RevertToPointerRoot, CurrentTime);
    for (int i = 0; i < 50; ++i) {
        if (XGrabKeyboard(dpy, root, True, GrabModeAsync, GrabModeAsync, CurrentTime) == GrabSuccess)break;
        usleep(2000);
    }
    GC gc = XCreateGC(dpy, win, 0, nullptr);
    XFontStruct * font = XLoadQueryFont(dpy, FONT_NAME);
    if (!font)font = XLoadQueryFont(dpy, "fixed");
    if (font)XSetFont(dpy, gc, font->fid);
    const int font_ascent = font ? font->ascent : 10;
    const int font_descent = font ? font->descent : 2;
    auto text_width = [ & ](const char *s, int len)->int {
        return font ? XTextWidth(font, s, len) : len * 6;
    };
    std::string input;
    input.reserve(64);
    std::vector<Match> matches;
    matches.reserve(256);
    uint32_t sel_idx = 0;
    uint32_t scroll_idx = 0;
    auto update_filter = [ & ]() {
        matches.clear();
        const char *q = input.c_str();
        if (! *q) {
            for (uint32_t i = 0; i < header->count; ++i)matches.push_back( {
                i, 0
            });
        } else {
            for (uint32_t i = 0; i < header->count; ++i) {
                int s;
                if (fuzzy_match(str_pool + offsets[i], q, s))matches.push_back( {
                    i, s
                });
            }
            std::stable_sort(matches.begin(), matches.end(), [](const Match & a, const Match & b) {
                return a.score > b.score;
            });
        }
        if (sel_idx >= matches.size())sel_idx = 0;
        scroll_idx = 0;
    };
    update_filter();
    auto render = [ & ]() {
        XSetForeground(dpy, gc, px_bg);
        XFillRectangle(dpy, win, gc, 0, 0, (unsigned)screen_w, BAR_HEIGHT);
        const int y = (BAR_HEIGHT + font_ascent - font_descent) / 2;
        char prompt[160];
        int plen = 0;
        prompt[plen++] = '>';
        prompt[plen++] = ' ';
        for (size_t i = 0; i < input.size() && plen < (int)sizeof(prompt) - 1; ++i)prompt[plen++] = input[i];
        prompt[plen] = '\0';
        XSetForeground(dpy, gc, px_fg);
        XDrawString(dpy, win, gc, PADDING, y, prompt, plen);
        const int x0 = PADDING + text_width(prompt, plen) + PADDING * 2;
        if (matches.empty()) {
            XFlush(dpy);
            return;
        }
        if (sel_idx < scroll_idx) {
            scroll_idx = sel_idx;
        } else if (scroll_idx < sel_idx) {
            while (scroll_idx < sel_idx) {
                int x = x0;
                bool visible = false;
                for (uint32_t i = scroll_idx; i <= sel_idx; ++i) {
                    const char *item = str_pool + offsets[matches[i].idx];
                    const int w = text_width(item, (int)strlen(item));
                    if (i == sel_idx) {
                        visible = (x + w <= screen_w - PADDING);
                        break;
                    }
                    x += w + PADDING * 2;
                    if (x >= screen_w)break;
                }
                if (visible)break;
                ++scroll_idx;
            }
        }
        int x = x0;
        for (uint32_t i = scroll_idx; i < matches.size() && x < screen_w; ++i) {
            const char *item = str_pool + offsets[matches[i].idx];
            const int len = (int)strlen(item);
            const int width = text_width(item, len);
            if (i == sel_idx) {
                XSetForeground(dpy, gc, px_sbg);
                XFillRectangle(dpy, win, gc, x - 4, 2, (unsigned)(width + 8), BAR_HEIGHT - 4);
                XSetForeground(dpy, gc, px_sfg);
            } else {
                XSetForeground(dpy, gc, px_fg);
            }
            XDrawString(dpy, win, gc, x, y, item, len);
            x += width + PADDING * 2;
        }
        XFlush(dpy);
    };
    render();
    XEvent ev;
    char buf[32];
    KeySym ksym;
    for (; ; ) {
        XNextEvent(dpy, &ev);
        if (ev.type == Expose) {
            render();
        } else if (ev.type == KeyPress) {
            const int len = XLookupString( &ev.xkey, buf, sizeof(buf) - 1, &ksym, nullptr);
            buf[len] = '\0';
            if (ksym == XK_Escape) {
                break;
            } else if (ksym == XK_Return) {
                if (!matches.empty()) {
                    const char *cmd = str_pool + offsets[matches[sel_idx].idx];
                    if (fork() == 0) {
                        setsid();
                        execlp(cmd, cmd, nullptr);
                        _exit(1);
                    }
                }
                break;
            } else if (ksym == XK_BackSpace) {
                if (!input.empty()) {
                    input.pop_back();
                    update_filter();
                    render();
                }
            }else if (ksym == XK_Tab) {
                if (!matches.empty()) {
                    std::string lcp;
                    bool have = false;
                    const size_t ilen = input.size();

                    for (const auto& m : matches) {
                        const char* name = str_pool + offsets[m.idx];
                        size_t i = 0;
                        while (i < ilen && name[i] &&
                               lower_ascii(name[i]) == lower_ascii(input[i])) ++i;
                        if (i != ilen) continue;

                        if (!have) { lcp = name; have = true; }
                        else {
                            size_t j = 0;
                            while (j < lcp.size() && name[j] && lcp[j] == name[j]) ++j;
                            lcp.resize(j);
                        }
                    }

                    if (have && lcp.size() > input.size()) {
                        input      = lcp;
                        sel_idx    = 0;
                        scroll_idx = 0;
                        update_filter();
                        render();
                    } else {                 
                        sel_idx = (sel_idx + 1) % (uint32_t)matches.size();
                        render();
                    }
                }
            } else if (ksym == XK_Down || ksym == XK_Right) {
                if (!matches.empty()) {
                    sel_idx = (sel_idx + 1) % (uint32_t)matches.size();
                    render();
                }
            } else if (ksym == XK_Up || ksym == XK_Left) {
                if (!matches.empty()) {
                    sel_idx = (sel_idx == 0) ? (uint32_t)matches.size() - 1 : sel_idx - 1;
                    render();
                }
            } else if (len > 0 && (unsigned char)buf[0] >= 32) {
                input += buf;
                update_filter();
                render();
            }
        }
    }
    XUngrabKeyboard(dpy, CurrentTime);
    if (font)XFreeFont(dpy, font);
    XFreeGC(dpy, gc);
    if (has_alpha)XFreeColormap(dpy, cm);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    munmap(mapped, (size_t)st.st_size);
    return 0;
}
