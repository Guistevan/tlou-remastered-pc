#include "runtime_menu_saves.h"
#include "gpu/gow3gpu.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>

/* The game's own texts for the page title: "LOAD GAME", "SAVE GAME" and "Delete". */
#define TEXT_LOAD_GAME 0x7c894f27u
#define TEXT_SAVE_GAME 0x0615b1acu
enum { MAX_ROWS = 33, LABEL_SIZE = 160 };

typedef ABI void (*PageOpen)(void *menu, uint32_t page, uint32_t a, uint32_t b);
typedef ABI void (*PageBack)(void *menu, uint32_t a);
typedef ABI void (*Dispatch)(void *menu, uint32_t hash, uint32_t a);
static PageOpen page_open;
static PageBack page_back;
static Dispatch dispatch;
static atomic_int page_added, update_hooked;

static char labels[MAX_ROWS][LABEL_SIZE], names[MAX_ROWS][LABEL_SIZE], dates[MAX_ROWS][LABEL_SIZE];
/* Each save's chapter (its detail's first line, in the language it was saved in) and the area of
 * the game's Chapters list it belongs to (-1: none found; AREA_UNKNOWN: not looked up yet). */
enum { AREA_UNKNOWN = -2 };
static char chapters[MAX_ROWS][LABEL_SIZE];
/* The rest of its detail: completion and play time, collectibles (lines). */
static char details[MAX_ROWS][LABEL_SIZE * 2];
static uint32_t subchapter_text[MAX_ROWS];
/* Its sub-chapter's place in the story (-1: none, as COMPLETE): its picture. The 29 sub-chapters'
 * text ids in story order (the Chapters list's areas, each area's sub-chapters as the game lists
 * them), and the game's image id of the first one's picture. */
static int subchapter_index[MAX_ROWS];
static const uint32_t subchapter_order[] = {
    0xe2db0a5du,                                         /* Hometown: Prologue */
    0xef982c84u, 0xeb593133u, 0xf51e6136u, 0xf1df7c81u, /* The Quarantine Zone */
    0xfc9c5a58u, 0xf85d47efu, 0xc012fa52u, 0xc4d3e7e5u, /* The Outskirts */
    0xc3132d87u, 0xc7d23030u, 0xca9116e9u, 0xce500b5eu, /* Bill's Town */
    0xd0175b5bu, 0xd4d646ecu, 0xd9956035u, 0xdd547d82u, /* Pittsburgh */
    0xe51bc03fu, 0xe1dadd88u,                            /* The Suburbs */
    0xb1f87254u, 0xb5396fe3u,                            /* Tommy's Dam */
    0xb87a493au, 0xbcbb548du,                            /* The University */
    0xa2fc0488u, 0xa63d193fu,                            /* Lakeside Resort */
    0xab7e3fe6u, 0xafbf2251u,                            /* Bus Depot */
    0x97f09fecu,                                         /* The Firefly Lab: The Hospital */
    0x9331825bu,                                         /* Jackson: Epilogue */
};
enum { SUBCHAPTER_COUNT = sizeof(subchapter_order) / sizeof(subchapter_order[0]) };
/* Left Behind's areas come after the game's 12 (the Chapters list in Left Behind shows its own
 * table, rows 0-5, and its own art, shot-chapters-spdlc1.tga, those rows in the same cells). */
enum { LB_AREA_FIRST = 12, LB_AREAS = 6 };
#define SUBCHAPTER_IMAGE_FIRST 0x38u
static int areas[MAX_ROWS];
/* The game had no picture for the row's area (its Chapters art is not loaded, as in the pause
 * menu): the overlay draws the save's own picture where the game's would be. */
static atomic_int no_game_picture[MAX_ROWS];
static atomic_int row_count, new_first, list_type;
/* The list's value (the shown row), read and written by the game's widget. */
static _Alignas(16) int32_t selected;

/* The game's Load Game runs a script (update 1.11: 0xb9b887ba, from the dispatcher's LOAD GAME
 * button) that opens the save dialog and loads the save; it does not survive the main page being
 * left and opened again. So the page runs apart from it:
 *   the first dialog is answered "cancelled" at once (as Back in the overlay list) and its rows
 *   are shown on our page (ASKED: the menu's next update opens it, OPEN);
 *   a row's press closes the page with the game's back() (SETTLING); once the main page has been
 *   on top for SETTLE_FRAMES updates, LOAD GAME is dispatched again (as the game's own save
 *   callback 0x309900 does) and the dialog it opens is answered with the chosen save at once. */
enum { CLOSED, ASKED, OPEN, SETTLING, ASKING, CONFIRMED };
/* Saving over a save: the game's Yes/No page on top of ours (ASKING; No closes it, back to OPEN);
 * its Yes (CONFIRMED): the update closes that page with back(), then the row is chosen as above. */
typedef ABI void (*ConfirmOpen)(void *menu, uint32_t title, uint32_t yes_label, uint32_t yes_button,
                                uint32_t no_label, uint32_t no_button, uint32_t extra);
static ConfirmOpen confirm_open;
static int asked_row, confirm_closing, asked_seen, asked_frames;
enum { ASK_FRAMES = 30 };
enum { SETTLE_FRAMES = 15 };
/* The button the dialog came from (Load Game, Save Game, ...): pressed again after a choice. */
static atomic_uint last_button, origin_button;
static atomic_int state;
static atomic_int pressed = -1, chosen = -1;
static int seen_page, settle;
static void *_Atomic open_menu;

/* A new game's or a chapter's save list (its dialog comes from the difficulty choice) can go on
 * without saving, as Back did in the overlay list: the game then asks "Auto-save will be
 * disabled until you manually save. Start ... without saving?". The page shows it in its footer
 * as the game's Square prompt (the page flag the cinematics' SUBTITLES prompt uses, its text from
 * the text hook), and Square on the page (the pad as the game read it) chooses it. */
#define BUTTON_NEW_GAME_DIFFICULTY 0xd8e3038bu
#define PAD_SQUARE 0x8000u
enum { PAGE_OBJECT_FLAGS = 0xa8 };
#define PAGE_FLAG_SQUARE_PROMPT 0x10000000u
static atomic_int no_save;
static uint32_t previous_buttons;
/* The footer's Triangle prompt off (read by the footer's stub): set while our page is on top. */
static int hide_commentary;
const int *runtime_menu_saves_commentary_flag(void) { return &hide_commentary; }

/* The page object on top of the menu's page stack, read on the menu's update. */
static unsigned char *top_page_object(const unsigned char *menu) {
    if (!menu) return NULL;
    const int32_t top = (int32_t)gow3_menu_u32(menu + GOW3_MENU_PAGE_TOP);
    if (top < 0 || top >= GOW3_MENU_PAGE_STACK_SIZE) return NULL;
    return (unsigned char *)(uintptr_t)gow3_menu_u64(menu + GOW3_MENU_PAGE_STACK + 8 * top);
}
/* Its hash. */
static uint32_t top_page(const unsigned char *menu) {
    const unsigned char *page = top_page_object(menu);
    return page ? gow3_menu_u32(page + GOW3_MENU_PAGE_OBJECT_HASH) : 0;
}

/* The row is chosen: the page closes with back(), the update presses the button again. */
static void choose(void *menu, int row) {
    atomic_store(&pressed, row);
    settle = 0;
    atomic_store(&state, SETTLING);
    gow3gpu_preview_show(-1);
    if (page_back && menu) page_back(menu, 0);
}

void runtime_menu_saves_set_navigation(void *open, void *back, void *dispatcher) {
    page_open = (PageOpen)open;
    page_back = (PageBack)back;
    dispatch = (Dispatch)dispatcher;
}

void runtime_menu_saves_set_update_hooked(void) {
    atomic_store(&update_hooked, 1);
}

void runtime_menu_saves_page_added(void) {
    atomic_store(&page_added, 1);
}

/* The pause menu's pages on the stack (PAGE_PAUSE_GAME): in game, where the Chapters art is not
 * loaded (the game answers the area's picture but draws nothing). */
#define PAGE_PAUSE_GAME 0xf05cd578u
static int in_game(const unsigned char *menu) {
    const int32_t top = (int32_t)gow3_menu_u32(menu + GOW3_MENU_PAGE_TOP);
    for (int32_t i = 0; i <= top && i < GOW3_MENU_PAGE_STACK_SIZE; ++i) {
        const unsigned char *page = (const unsigned char *)(uintptr_t)gow3_menu_u64(menu + GOW3_MENU_PAGE_STACK + 8 * i);
        if (page && gow3_menu_u32(page + GOW3_MENU_PAGE_OBJECT_HASH) == PAGE_PAUSE_GAME) return 1;
    }
    return 0;
}
static atomic_int paused;

/* The overlay's picture for saves whose area is not known, and in game (the game draws the
 * others). */
static void show_preview(int on_top) {
    const int row = selected;
    const int valid = row >= 0 && row < atomic_load(&row_count);
    const int known = valid && areas[row] >= 0;
    if (!on_top || !valid) gow3gpu_preview_show(-1);
    else if (!known) gow3gpu_preview_show(row);
    else if (atomic_load(&paused) || atomic_load(&no_game_picture[row])) gow3gpu_preview_show(row | GOW3_PREVIEW_PICTURE_BOX);
    else gow3gpu_preview_show(-1);
}

/* The menu's per-frame update (its entry hook), in the menu's own context. */
ABI void runtime_menu_saves_update(void *menu, uint32_t unused) {
    (void)unused;
    if (!menu) return;
    const int st = atomic_load(&state);
    if (st == ASKED) {
        seen_page = 0;
        previous_buttons = runtime_pad_buttons();
        atomic_store(&paused, in_game(menu));
        atomic_store(&open_menu, menu);
        atomic_store(&state, OPEN);
        page_open(menu, GOW3_MENU_SAVES_PAGE, 0, 0);
        printf("Menu: save list on the game's own page (%d rows%s)\n", atomic_load(&row_count),
               atomic_load(&paused) ? ", in game: the saves' own pictures" : "");
        return;
    }
    if (menu != atomic_load(&open_menu)) return;
    const int on_top = top_page(menu) == GOW3_MENU_SAVES_PAGE;
    __atomic_store_n(&hide_commentary, st == OPEN && on_top && atomic_load(&no_save), __ATOMIC_RELAXED);
    if (st == ASKING) {
        /* The question opens on the menu's next updates; ours on top again once it was seen
         * (or if it never came): No closed it. */
        if (top_page(menu) == GOW3_MENU_CONFIRM_PAGE) asked_seen = 1;
        else if (on_top && (asked_seen || ++asked_frames > ASK_FRAMES)) {
            puts("Menu: save list, not overwritten");
            atomic_store(&state, OPEN);
        }
        return;
    }
    if (st == CONFIRMED) {
        if (on_top) {
            printf("Menu: save list row %d, overwrite confirmed\n", asked_row);
            choose(menu, asked_row);
        } else if (top_page(menu) == GOW3_MENU_CONFIRM_PAGE && !confirm_closing && page_back) {
            confirm_closing = 1;
            page_back(menu, 0);
        }
        return;
    }
    if (st == OPEN) {
        if (on_top) seen_page = 1;
        else if (seen_page) { /* Back: left without a choice */
            atomic_store(&state, CLOSED);
            gow3gpu_preview_begin(NULL, 0); /* the rows' pictures go */
            return;
        }
        /* The menu's state (menu+8): 0xc while it is on screen; Start closes it (5, then 0 while
         * the game runs, our page staying on top of its stack) and opens it again (3, then 0xc). */
        const int shown = on_top && gow3_menu_u32((const unsigned char *)menu + GOW3_MENU_STATE) == GOW3_MENU_STATE_SHOWN;
        if (on_top) {
            /* The footer's Square prompt only on a save list that can go on without saving (the
             * page is the same for the others). */
            unsigned char *page = top_page_object(menu);
            const uint32_t flags = gow3_menu_u32(page + PAGE_OBJECT_FLAGS);
            const uint32_t want = atomic_load(&no_save) ? flags | PAGE_FLAG_SQUARE_PROMPT : flags & ~PAGE_FLAG_SQUARE_PROMPT;
            if (want != flags) memcpy(page + PAGE_OBJECT_FLAGS, &want, 4);
        }
        const uint32_t buttons = runtime_pad_buttons();
        const int square = (buttons & PAD_SQUARE) && !(previous_buttons & PAD_SQUARE);
        previous_buttons = buttons;
        if (shown && square && atomic_load(&no_save)) {
            atomic_store(&pressed, GOW3_MENU_SAVES_NO_SAVE);
            settle = 0;
            atomic_store(&state, SETTLING);
            gow3gpu_preview_show(-1);
            if (page_back) page_back(menu, 0);
            puts("Menu: save list, continue without saving");
            return;
        }
        show_preview(shown);
    } else if (st == SETTLING) {
        gow3gpu_preview_show(-1);
        if (on_top) settle = 0;
        else if (++settle >= SETTLE_FRAMES) {
            atomic_store(&chosen, atomic_load(&pressed));
            atomic_store(&state, CLOSED);
            gow3gpu_preview_begin(NULL, 0); /* the rows' pictures go */
            printf("Menu: save list row %d chosen, button %#x again\n", atomic_load(&chosen), atomic_load(&origin_button));
            if (dispatch) dispatch(menu, atomic_load(&origin_button), 0);
        }
    }
}

static int area_of(int row);

/* The Chapters page's area and sub-chapter pictures, and Left Behind's, as PNG files in
 * <user>/chapter-art-4 (gow3_chapter_art.cpp), made from the game's common.psarc the first time; 1
 * when they are there. */
static int chapter_art(void) {
    static int ready = -1;
    if (ready >= 0) return ready;
    char dir[600], last[680];
    snprintf(dir, sizeof(dir), "%s/chapter-art-4", runtime_file_user_dir()); /* -4: Left Behind's too */
    snprintf(last, sizeof(last), "%s/lb-5.png", dir);
    if (access(last, 0) == 0) return ready = 1;
    mkdir(dir, 0755);
    char psarc[1024];
    if (runtime_file_translate("/app0/build/ps4/main/common.psarc", psarc, sizeof(psarc))) {
        puts("Menu: no common.psarc for the Chapters pictures");
        return ready = 0;
    }
    ready = gow3gpu_chapter_art(psarc, dir) == 12 + SUBCHAPTER_COUNT + LB_AREAS;
    return ready;
}

int runtime_menu_saves_open(int type, int count, int has_new, const char *const *row_labels,
                            const char *const *pictures, const char *const *row_chapters) {
    static int disabled = -1;
    if (disabled < 0) {
        const char *env = getenv("GOW3_NATIVE_SAVE_LIST");
        disabled = env && env[0] == '0';
    }
    const char *why = disabled ? "GOW3_NATIVE_SAVE_LIST=0"
                    : type != 1 && type != 2 ? "only Load and Save are on the game's page"
                    : !atomic_load(&last_button) ? "no button seen"
                    : !page_open || !page_back || !dispatch ? "page navigation not found"
                    : !atomic_load(&update_hooked) ? "menu update not hooked"
                    : !atomic_load(&page_added) ? "page not in the menu data"
                    : atomic_load(&state) != CLOSED ? "page busy"
                    : count <= 0 || count > MAX_ROWS ? "row count"
                                                     : NULL;
    if (why) {
        printf("Menu: save list in the overlay (%s)\n", why);
        return 0;
    }
    /* "name - date" rows: the name and the date apart. */
    for (int i = 0; i < count; ++i) {
        snprintf(labels[i], LABEL_SIZE, "%s", row_labels[i] ? row_labels[i] : "");
        const char *dash = strstr(labels[i], " - ");
        snprintf(names[i], LABEL_SIZE, "%.*s", dash ? (int)(dash - labels[i]) : (int)strlen(labels[i]), labels[i]);
        snprintf(dates[i], LABEL_SIZE, "%s", dash ? dash + 3 : "");
        /* The detail: its first line the chapter, the others shown under the place. */
        const char *detail = row_chapters && row_chapters[i] ? row_chapters[i] : "";
        const char *rest = strchr(detail, '\n');
        snprintf(chapters[i], LABEL_SIZE, "%.*s", rest ? (int)(rest - detail) : (int)strlen(detail), detail);
        snprintf(details[i], sizeof(details[i]), "%s", rest ? rest + 1 : "");
        for (size_t n = strlen(details[i]); n && (details[i][n - 1] == '\n' || details[i][n - 1] == '\r'); --n)
            details[i][n - 1] = 0;
        areas[i] = AREA_UNKNOWN;
        atomic_store(&no_game_picture[i], 0);
    }
    atomic_store(&row_count, count);
    atomic_store(&new_first, has_new);
    atomic_store(&list_type, type);
    selected = 0;
    atomic_store(&pressed, -1);
    atomic_store(&chosen, -1);
    /* Saves whose sub-chapter is known show its picture, those with only an area known that
     * area's (the Chapters art, read from the game's files once); the others their own. */
    const char *shown[MAX_ROWS];
    static char art[MAX_ROWS][600];
    const int have_art = chapter_art();
    for (int i = 0; i < count; ++i) {
        shown[i] = pictures ? pictures[i] : NULL;
        const int area = area_of(i);
        if (have_art && subchapter_index[i] >= 0) {
            snprintf(art[i], sizeof(art[i]), "%s/chapter-art-4/sub-%d.png", runtime_file_user_dir(), subchapter_index[i]);
            shown[i] = art[i];
        } else if (have_art && area >= LB_AREA_FIRST) {
            snprintf(art[i], sizeof(art[i]), "%s/chapter-art-4/lb-%d.png", runtime_file_user_dir(), area - LB_AREA_FIRST);
            shown[i] = art[i];
        } else if (have_art && area >= 0) {
            snprintf(art[i], sizeof(art[i]), "%s/chapter-art-4/area-%d.png", runtime_file_user_dir(), area);
            shown[i] = art[i];
        }
    }
    gow3gpu_preview_begin(shown, count);
    atomic_store(&origin_button, atomic_load(&last_button));
    atomic_store(&no_save, type == 1 && atomic_load(&origin_button) == BUTTON_NEW_GAME_DIFFICULTY);
    atomic_store(&state, ASKED);
    return 1;
}

void runtime_menu_saves_note_button(uint32_t hash) {
    if (hash != GOW3_MENU_SAVES_LIST && hash != GOW3_MENU_SAVES_OVERWRITE_YES && hash != GOW3_MENU_CONFIRM_NO)
        atomic_store(&last_button, hash);
}

void runtime_menu_saves_set_confirm(void *open) {
    confirm_open = (ConfirmOpen)open;
}

int runtime_menu_saves_take_choice(void) {
    return atomic_exchange(&chosen, -1);
}

int runtime_menu_saves_no_save_offered(void) {
    return atomic_load(&no_save) && atomic_load(&state) == OPEN;
}

/* Runs inside the dispatcher, in the menu's update: the shown row is chosen and the page closes. */
int runtime_menu_saves_button(void *menu, uint32_t hash) {
    if (hash == GOW3_MENU_SAVES_OVERWRITE_YES) {
        /* Yes on the overwrite question: the update closes that page, then chooses the row. */
        if (atomic_load(&state) == ASKING) {
            confirm_closing = 0;
            atomic_store(&state, CONFIRMED);
        }
        return 1;
    }
    if (hash != GOW3_MENU_SAVES_LIST) return 0;
    if (atomic_load(&state) != OPEN) return 1;
    const int count = atomic_load(&row_count);
    const int row = selected < 0 ? 0 : selected >= count ? count - 1 : selected;
    /* Saving over a save (not the new save row): the game's Yes/No page asks first. */
    if (atomic_load(&list_type) == 1 && row >= atomic_load(&new_first) && confirm_open && menu) {
        asked_row = row;
        asked_seen = 0;
        asked_frames = 0;
        atomic_store(&state, ASKING);
        gow3gpu_preview_show(-1);
        confirm_open(menu, GOW3_MENU_SAVES_OVERWRITE_TEXT, GOW3_MENU_TEXT_YES, GOW3_MENU_SAVES_OVERWRITE_YES,
                     GOW3_MENU_TEXT_NO, GOW3_MENU_CONFIRM_NO, 0);
        printf("Menu: save list row %d, overwrite? (the game's Yes/No page)\n", row);
        return 1;
    }
    choose(menu, row);
    return 1;
}

/* --- The list's binding --------------------------------------------------------------------- */
static uint64_t vtable[GOW3_BINDING_VTABLE_SLOTS];
static atomic_int vtable_ready;

/* label(self, row, part), as Chapters' list provider answers it: part 0 the row's name, both in
 * the list and under the picture beside it; the other parts are a second column and panel lines
 * that the picture layout does not show. The list asks for every row each frame, the panel only
 * for the shown one: a caller (its return address) that ever asked for another row is the
 * list's. The panel's line gets the save's date too. */
enum { MAX_LIST_SITES = 8 };
static uintptr_t list_sites[MAX_LIST_SITES];
static int list_site_count;
static char panel_text[LABEL_SIZE * 8];
static void place_of(int row, char *out, size_t size);

static int is_list_site(uintptr_t site, int index) {
    for (int i = 0; i < list_site_count; ++i)
        if (list_sites[i] == site) return 1;
    if (index != selected && list_site_count < MAX_LIST_SITES) {
        list_sites[list_site_count++] = site;
        return 1;
    }
    return 0;
}

static ABI __attribute__((noinline)) const char *row_label(void *self, int index, int part) {
    (void)self;
    if (index < 0 || index >= atomic_load(&row_count)) return "";
    if (index == 0 && atomic_load(&new_first)) return part == 0 ? runtime_menu_pc_new_save_text() : "";
    if (part != 0) return "";
    if (is_list_site((uintptr_t)__builtin_return_address(0), index) || !dates[index][0]) return names[index];
    /* Name, where the save is (area - sub-chapter), the detail's other lines (completion and play
     * time, collectibles), date. */
    char place[LABEL_SIZE * 2];
    place_of(index, place, sizeof(place));
    size_t at = (size_t)snprintf(panel_text, sizeof(panel_text), "%s", names[index]);
    if (place[0] && at < sizeof(panel_text)) at += (size_t)snprintf(panel_text + at, sizeof(panel_text) - at, "\n%s", place);
    if (details[index][0] && at < sizeof(panel_text))
        at += (size_t)snprintf(panel_text + at, sizeof(panel_text) - at, "\n%s", details[index]);
    if (at < sizeof(panel_text)) snprintf(panel_text + at, sizeof(panel_text) - at, "\n%s", dates[index]);
    return panel_text;
}

/* The game's chapters (update 1.11, dc1/tasks.bin): each sub-chapter's name, in every language
 * of the game (text1/<N>.common), and its area, in the order of the Chapters list (the area list
 * provider counts them: 1, 4, 4, 4, 4, 2, 2, 2, 2, 2, 1, 1). A save's detail starts with the name
 * in the language it was saved in; its picture is its area's, as Chapters shows it. Generated
 * from the 29 sub-chapter text ids (no name belongs to two areas). */
static const struct { const char *name; int area; uint32_t text; } subchapters[] = {
    {"Proloog", 0, 0xe2db0a5du},
    {"プロローグ", 0, 0xe2db0a5du},
    {"프롤로그", 0, 0xe2db0a5du},
    {"Prologi", 0, 0xe2db0a5du},
    {"Prolog", 0, 0xe2db0a5du},
    {"序幕", 0, 0xe2db0a5du},
    {"Пролог", 0, 0xe2db0a5du},
    {"Prólogo", 0, 0xe2db0a5du},
    {"Prologue", 0, 0xe2db0a5du},
    {"Πρόλογος", 0, 0xe2db0a5du},
    {"Başlangıç", 0, 0xe2db0a5du},
    {"Prologo", 0, 0xe2db0a5du},
    {"Twintig jaar later", 1, 0xef982c84u},
    {"２０年後", 1, 0xef982c84u},
    {"20년 후", 1, 0xef982c84u},
    {"20 vuotta myöhemmin", 1, 0xef982c84u},
    {"20 år senere", 1, 0xef982c84u},
    {"20 år senare", 1, 0xef982c84u},
    {"二十年後", 1, 0xef982c84u},
    {"Карантинная зона", 1, 0xef982c84u},
    {"20 lat później", 1, 0xef982c84u},
    {"20 años después", 1, 0xef982c84u},
    {"20 Years Later", 1, 0xef982c84u},
    {"Vinte anos depois", 1, 0xef982c84u},
    {"20 χρόνια μετά", 1, 0xef982c84u},
    {"O 20 let později", 1, 0xef982c84u},
    {"20 Yıl Sonra", 1, 0xef982c84u},
    {"20 ans plus tard", 1, 0xef982c84u},
    {"20 anni dopo", 1, 0xef982c84u},
    {"20 Jahre später", 1, 0xef982c84u},
    {"20 Anos Depois", 1, 0xef982c84u},
    {"Voorbij de muur", 1, 0x6b7c145cu},
    {"壁の向こう", 1, 0x6b7c145cu},
    {"벽을 넘어", 1, 0x6b7c145cu},
    {"Muurin tuolla puolen", 1, 0x6b7c145cu},
    {"Bortenfor muren", 1, 0x6b7c145cu},
    {"Utanför muren", 1, 0x6b7c145cu},
    {"牆外", 1, 0x6b7c145cu},
    {"За стеной", 1, 0x6b7c145cu},
    {"Poza murem", 1, 0x6b7c145cu},
    {"Más allá de la pared", 1, 0x6b7c145cu},
    {"Beyond the Wall", 1, 0x6b7c145cu},
    {"Além do muro", 1, 0x6b7c145cu},
    {"Πέρα από τον Τοίχο", 1, 0x6b7c145cu},
    {"Za zdí", 1, 0x6b7c145cu},
    {"Duvarın Ötesi", 1, 0x6b7c145cu},
    {"Derrière le mur", 1, 0x6b7c145cu},
    {"Oltre il muro", 1, 0x6b7c145cu},
    {"Jenseits der Mauer", 1, 0x6b7c145cu},
    {"Al otro lado del muro", 1, 0x6b7c145cu},
    {"Uden for muren", 1, 0x6b7c145cu},
    {"Para lá do muro", 1, 0x6b7c145cu},
    {"抜け穴", 1, 0xeb593133u},
    {"高牆之外", 1, 0xeb593133u},
    {"Beyond The Wall", 1, 0xeb593133u},
    {"Hinter der Mauer", 1, 0xeb593133u},
    {"Para Lá do Muro", 1, 0xeb593133u},
    {"De vracht", 1, 0xf1df7c81u},
    {"仕事の依頼", 1, 0xf1df7c81u},
    {"짐짝", 1, 0xf1df7c81u},
    {"Salakuljetettava", 1, 0xf1df7c81u},
    {"Lasten", 1, 0xf1df7c81u},
    {"貨物", 1, 0xf1df7c81u},
    {"Цикады", 1, 0xf1df7c81u},
    {"Ładunek", 1, 0xf1df7c81u},
    {"El cargamento", 1, 0xf1df7c81u},
    {"The Cargo", 1, 0xf1df7c81u},
    {"A carga", 1, 0xf1df7c81u},
    {"Το Φορτίο", 1, 0xf1df7c81u},
    {"Náklad", 1, 0xf1df7c81u},
    {"Kargo", 1, 0xf1df7c81u},
    {"Le cargo", 1, 0xf1df7c81u},
    {"Il carico", 1, 0xf1df7c81u},
    {"Die Ladung", 1, 0xf1df7c81u},
    {"A Carga", 1, 0xf1df7c81u},
    {"De sloppenwijken", 1, 0xf51e6136u},
    {"スラム街", 1, 0xf51e6136u},
    {"빈민가", 1, 0xf51e6136u},
    {"Slummit", 1, 0xf51e6136u},
    {"Slummen", 1, 0xf51e6136u},
    {"貧民窟", 1, 0xf51e6136u},
    {"Трущобы", 1, 0xf51e6136u},
    {"Slumsy", 1, 0xf51e6136u},
    {"Los barrios bajos", 1, 0xf51e6136u},
    {"The Slums", 1, 0xf51e6136u},
    {"A favela", 1, 0xf51e6136u},
    {"Οι Φτωχογειτονιές", 1, 0xf51e6136u},
    {"Slumy", 1, 0xf51e6136u},
    {"Kenar Semt", 1, 0xf51e6136u},
    {"Bidonville", 1, 0xf51e6136u},
    {"I bassifondi", 1, 0xf51e6136u},
    {"Die Slums", 1, 0xf51e6136u},
    {"Barrios bajos", 1, 0xf51e6136u},
    {"Bairro de Lata", 1, 0xf51e6136u},
    {"Buiten", 2, 0xfc9c5a58u},
    {"壁の外", 2, 0xfc9c5a58u},
    {"바깥 세상", 2, 0xfc9c5a58u},
    {"Ulkona", 2, 0xfc9c5a58u},
    {"Utsiden", 2, 0xfc9c5a58u},
    {"På utsidan", 2, 0xfc9c5a58u},
    {"外面的世界", 2, 0xfc9c5a58u},
    {"Пустыри", 2, 0xfc9c5a58u},
    {"Na zewnątrz", 2, 0xfc9c5a58u},
    {"Afuera", 2, 0xfc9c5a58u},
    {"Outside", 2, 0xfc9c5a58u},
    {"Lá fora", 2, 0xfc9c5a58u},
    {"Έξω", 2, 0xfc9c5a58u},
    {"Venku", 2, 0xfc9c5a58u},
    {"Dışarısı", 2, 0xfc9c5a58u},
    {"Sortie", 2, 0xfc9c5a58u},
    {"Là fuori", 2, 0xfc9c5a58u},
    {"Draußen", 2, 0xfc9c5a58u},
    {"Fuera", 2, 0xfc9c5a58u},
    {"Udenfor!", 2, 0xfc9c5a58u},
    {"Lá fora, pela primeira vez", 2, 0xfc9c5a58u},
    {"De stad", 2, 0xf85d47efu},
    {"ダウンタウン", 2, 0xf85d47efu},
    {"도심지", 2, 0xf85d47efu},
    {"Keskusta", 2, 0xf85d47efu},
    {"Sentrum", 2, 0xf85d47efu},
    {"City", 2, 0xf85d47efu},
    {"市區", 2, 0xf85d47efu},
    {"Центр", 2, 0xf85d47efu},
    {"Śródmieście", 2, 0xf85d47efu},
    {"Centro", 2, 0xf85d47efu},
    {"Downtown", 2, 0xf85d47efu},
    {"Centro da cidade", 2, 0xf85d47efu},
    {"Κέντρο της Πόλης", 2, 0xf85d47efu},
    {"Centrum města", 2, 0xf85d47efu},
    {"Şehir Merkezi", 2, 0xf85d47efu},
    {"Centre ville", 2, 0xf85d47efu},
    {"Centro città", 2, 0xf85d47efu},
    {"Zentrum", 2, 0xf85d47efu},
    {"Midtbyen", 2, 0xf85d47efu},
    {"Baixa", 2, 0xf85d47efu},
    {"Museum", 2, 0xc012fa52u},
    {"美術館", 2, 0xc012fa52u},
    {"박물관", 2, 0xc012fa52u},
    {"Museo", 2, 0xc012fa52u},
    {"Museet", 2, 0xc012fa52u},
    {"博物館", 2, 0xc012fa52u},
    {"Музей", 2, 0xc012fa52u},
    {"Muzeum", 2, 0xc012fa52u},
    {"Museu", 2, 0xc012fa52u},
    {"Μουσείο", 2, 0xc012fa52u},
    {"Müze", 2, 0xc012fa52u},
    {"Musée", 2, 0xc012fa52u},
    {"Het gemeentehuis", 2, 0xc4d3e7e5u},
    {"州議事堂", 2, 0xc4d3e7e5u},
    {"중앙청 건물", 2, 0xc4d3e7e5u},
    {"Valtiontalo", 2, 0xc4d3e7e5u},
    {"Statshuset", 2, 0xc4d3e7e5u},
    {"Stadshuset", 2, 0xc4d3e7e5u},
    {"議會大廈", 2, 0xc4d3e7e5u},
    {"Рандеву", 2, 0xc4d3e7e5u},
    {"Budynek ratusza", 2, 0xc4d3e7e5u},
    {"El edificio del capitolio", 2, 0xc4d3e7e5u},
    {"The Capitol Building", 2, 0xc4d3e7e5u},
    {"O prédio do congresso", 2, 0xc4d3e7e5u},
    {"Καπιτώλιο", 2, 0xc4d3e7e5u},
    {"Budova kapitolu", 2, 0xc4d3e7e5u},
    {"Hükümet Binası", 2, 0xc4d3e7e5u},
    {"Le capitole", 2, 0xc4d3e7e5u},
    {"L'edificio governativo", 2, 0xc4d3e7e5u},
    {"Das Rathaus", 2, 0xc4d3e7e5u},
    {"El ayuntamiento", 2, 0xc4d3e7e5u},
    {"Parlamentsbygning", 2, 0xc4d3e7e5u},
    {"Câmara Municipal", 2, 0xc4d3e7e5u},
    {"De bossen", 3, 0xc3132d87u},
    {"森", 3, 0xc3132d87u},
    {"숲", 3, 0xc3132d87u},
    {"Metsä", 3, 0xc3132d87u},
    {"Skogen", 3, 0xc3132d87u},
    {"樹林", 3, 0xc3132d87u},
    {"Американский городок", 3, 0xc3132d87u},
    {"Las", 3, 0xc3132d87u},
    {"El bosque", 3, 0xc3132d87u},
    {"The Woods", 3, 0xc3132d87u},
    {"O bosque", 3, 0xc3132d87u},
    {"Το Δάσος", 3, 0xc3132d87u},
    {"Lesy", 3, 0xc3132d87u},
    {"Ormanlık Alan", 3, 0xc3132d87u},
    {"Les bois", 3, 0xc3132d87u},
    {"I boschi", 3, 0xc3132d87u},
    {"Der Wald", 3, 0xc3132d87u},
    {"Los bosques", 3, 0xc3132d87u},
    {"Skoven", 3, 0xc3132d87u},
    {"Os Bosques", 3, 0xc3132d87u},
    {"Schuilplaats", 3, 0xc7d23030u},
    {"ビルのアジト", 3, 0xc7d23030u},
    {"안전가옥", 3, 0xc7d23030u},
    {"Turvatalo", 3, 0xc7d23030u},
    {"Skjulested", 3, 0xc7d23030u},
    {"Gömstället", 3, 0xc7d23030u},
    {"藏身處", 3, 0xc7d23030u},
    {"Ответная услуга", 3, 0xc7d23030u},
    {"Kryjówka", 3, 0xc7d23030u},
    {"Refugio", 3, 0xc7d23030u},
    {"Safehouse", 3, 0xc7d23030u},
    {"Esconderijo", 3, 0xc7d23030u},
    {"Κρησφύγετο", 3, 0xc7d23030u},
    {"Úkryt", 3, 0xc7d23030u},
    {"Güvenli Ev", 3, 0xc7d23030u},
    {"Abri", 3, 0xc7d23030u},
    {"Rifugio", 3, 0xc7d23030u},
    {"Unterschlupf", 3, 0xc7d23030u},
    {"Casa franca", 3, 0xc7d23030u},
    {"Sikkert hus", 3, 0xc7d23030u},
    {"Begraafplaats", 3, 0xca9116e9u},
    {"墓場", 3, 0xca9116e9u},
    {"묘지", 3, 0xca9116e9u},
    {"Hautausmaa", 3, 0xca9116e9u},
    {"Kirkegården", 3, 0xca9116e9u},
    {"Kyrkogården", 3, 0xca9116e9u},
    {"墓園", 3, 0xca9116e9u},
    {"Терра инкогнита", 3, 0xca9116e9u},
    {"Cmentarz", 3, 0xca9116e9u},
    {"Cementerio", 3, 0xca9116e9u},
    {"Graveyard", 3, 0xca9116e9u},
    {"Cemitério", 3, 0xca9116e9u},
    {"Νεκροταφείο", 3, 0xca9116e9u},
    {"Hřbitov", 3, 0xca9116e9u},
    {"Mezarlık", 3, 0xca9116e9u},
    {"Cimetière", 3, 0xca9116e9u},
    {"Cimitero", 3, 0xca9116e9u},
    {"Friedhof", 3, 0xca9116e9u},
    {"Kirkegård", 3, 0xca9116e9u},
    {"Ontsnapping uit de middelbare school", 3, 0xce500b5eu},
    {"高校", 3, 0xce500b5eu},
    {"고등학교 탈출", 3, 0xce500b5eu},
    {"Pako koulusta", 3, 0xce500b5eu},
    {"Flukt fra skolen", 3, 0xce500b5eu},
    {"Flykten från skolan", 3, 0xce500b5eu},
    {"逃離高中", 3, 0xce500b5eu},
    {"Средняя школа", 3, 0xce500b5eu},
    {"Ucieczka z budynku szkoły", 3, 0xce500b5eu},
    {"Escape de la escuela", 3, 0xce500b5eu},
    {"High School Escape", 3, 0xce500b5eu},
    {"Fuga da escola secundária", 3, 0xce500b5eu},
    {"Απόδραση από το Γυμνάσιο", 3, 0xce500b5eu},
    {"Útěk ze střední školy", 3, 0xce500b5eu},
    {"Liseden Kaçış", 3, 0xce500b5eu},
    {"Fuite du lycée", 3, 0xce500b5eu},
    {"Fuga dal liceo", 3, 0xce500b5eu},
    {"Highschool-Flucht", 3, 0xce500b5eu},
    {"Huida del instituto", 3, 0xce500b5eu},
    {"Flugt fra highschool", 3, 0xce500b5eu},
    {"Fuga do Liceu", 3, 0xce500b5eu},
    {"Alleen en verlaten", 4, 0xd0175b5bu},
    {"待ち伏せ", 4, 0xd0175b5bu},
    {"우회로", 4, 0xd0175b5bu},
    {"Yksin ja hylättynä", 4, 0xd0175b5bu},
    {"Ensom og forlatt", 4, 0xd0175b5bu},
    {"Ensam och övergiven", 4, 0xd0175b5bu},
    {"孤獨和被遺棄", 4, 0xd0175b5bu},
    {"Обходной путь", 4, 0xd0175b5bu},
    {"Samotna i porzucona", 4, 0xd0175b5bu},
    {"Solos y abandonados", 4, 0xd0175b5bu},
    {"Alone and Forsaken", 4, 0xd0175b5bu},
    {"Sozinho e abandonado", 4, 0xd0175b5bu},
    {"Μόνος κι έρημος", 4, 0xd0175b5bu},
    {"Sami a opuštění", 4, 0xd0175b5bu},
    {"Yalnız ve Terk edilmiş", 4, 0xd0175b5bu},
    {"Solitude et oubli", 4, 0xd0175b5bu},
    {"Solitudine e abbandono", 4, 0xd0175b5bu},
    {"Allein und verlassen", 4, 0xd0175b5bu},
    {"Solo y olvidado", 4, 0xd0175b5bu},
    {"Alene og forladt", 4, 0xd0175b5bu},
    {"Sozinho e esquecido", 4, 0xd0175b5bu},
    {"Hotellobby", 4, 0xd4d646ecu},
    {"ホテル", 4, 0xd4d646ecu},
    {"호텔 로비", 4, 0xd4d646ecu},
    {"Hotellin aula", 4, 0xd4d646ecu},
    {"Hotellobbyn", 4, 0xd4d646ecu},
    {"飯店大廳", 4, 0xd4d646ecu},
    {"Отель", 4, 0xd4d646ecu},
    {"Hotelowy hol", 4, 0xd4d646ecu},
    {"Lobby del hotel", 4, 0xd4d646ecu},
    {"Hotel Lobby", 4, 0xd4d646ecu},
    {"Lobby do hotel", 4, 0xd4d646ecu},
    {"Λόμπι ξενοδοχείου", 4, 0xd4d646ecu},
    {"Hotelová hala", 4, 0xd4d646ecu},
    {"Otel Lobisi", 4, 0xd4d646ecu},
    {"Accueil de l'hôtel", 4, 0xd4d646ecu},
    {"Lobby dell'hotel", 4, 0xd4d646ecu},
    {"Vestíbulo del hotel", 4, 0xd4d646ecu},
    {"Receção do Hotel", 4, 0xd4d646ecu},
    {"Financieel district", 4, 0xd9956035u},
    {"金融街", 4, 0xd9956035u},
    {"금융가", 4, 0xd9956035u},
    {"Talouskeskus", 4, 0xd9956035u},
    {"Finansdistriktet", 4, 0xd9956035u},
    {"Finansdistrikt", 4, 0xd9956035u},
    {"金融區", 4, 0xd9956035u},
    {"Финансовый квартал", 4, 0xd9956035u},
    {"Dzielnica biznesowa", 4, 0xd9956035u},
    {"Distrito financiero", 4, 0xd9956035u},
    {"Financial District", 4, 0xd9956035u},
    {"Distrito financeiro", 4, 0xd9956035u},
    {"Οικονομικό Κέντρο", 4, 0xd9956035u},
    {"Finanční čtvrť", 4, 0xd9956035u},
    {"Mali Semt", 4, 0xd9956035u},
    {"Quartier des affaires", 4, 0xd9956035u},
    {"Distretto finanziario", 4, 0xd9956035u},
    {"Finanzviertel", 4, 0xd9956035u},
    {"Bairro Financeiro", 4, 0xd9956035u},
    {"Ontsnap uit de stad", 4, 0xdd547d82u},
    {"街からの脱出", 4, 0xdd547d82u},
    {"도시 탈출", 4, 0xdd547d82u},
    {"Pako kaupungista", 4, 0xdd547d82u},
    {"Unnslipp byen", 4, 0xdd547d82u},
    {"Fly från staden", 4, 0xdd547d82u},
    {"逃離市區", 4, 0xdd547d82u},
    {"Лагерь Охотников", 4, 0xdd547d82u},
    {"Ucieczka z miasta", 4, 0xdd547d82u},
    {"Escape de la ciudad", 4, 0xdd547d82u},
    {"Escape the City", 4, 0xdd547d82u},
    {"Fuga da cidade", 4, 0xdd547d82u},
    {"Απόδρασε από την Πόλη", 4, 0xdd547d82u},
    {"Útěk z města", 4, 0xdd547d82u},
    {"Şehirden Kaç", 4, 0xdd547d82u},
    {"Quitter la ville", 4, 0xdd547d82u},
    {"Fuggi dalla città", 4, 0xdd547d82u},
    {"Die Stadt verlassen", 4, 0xdd547d82u},
    {"Huye de la ciudad", 4, 0xdd547d82u},
    {"Slip ud af byen", 4, 0xdd547d82u},
    {"Foge da Cidade", 4, 0xdd547d82u},
    {"Riool", 5, 0xe51bc03fu},
    {"地下水路", 5, 0xe51bc03fu},
    {"하수도", 5, 0xe51bc03fu},
    {"Viemäri", 5, 0xe51bc03fu},
    {"Kloakken", 5, 0xe51bc03fu},
    {"Kloakerna", 5, 0xe51bc03fu},
    {"下水道", 5, 0xe51bc03fu},
    {"Канализация", 5, 0xe51bc03fu},
    {"Kanały", 5, 0xe51bc03fu},
    {"Alcantarillas", 5, 0xe51bc03fu},
    {"Sewers", 5, 0xe51bc03fu},
    {"Esgoto", 5, 0xe51bc03fu},
    {"Υπόνομοι", 5, 0xe51bc03fu},
    {"Stoky", 5, 0xe51bc03fu},
    {"Kanalizasyon", 5, 0xe51bc03fu},
    {"Égouts", 5, 0xe51bc03fu},
    {"Fogne", 5, 0xe51bc03fu},
    {"Kanalisation", 5, 0xe51bc03fu},
    {"Kloakker", 5, 0xe51bc03fu},
    {"Esgotos", 5, 0xe51bc03fu},
    {"Buitenwijk", 5, 0xe1dadd88u},
    {"住宅街", 5, 0xe1dadd88u},
    {"교외", 5, 0xe1dadd88u},
    {"Esikaupunki", 5, 0xe1dadd88u},
    {"Forstedene", 5, 0xe1dadd88u},
    {"Förorten", 5, 0xe1dadd88u},
    {"郊區", 5, 0xe1dadd88u},
    {"Пригород", 5, 0xe1dadd88u},
    {"Przedmieścia", 5, 0xe1dadd88u},
    {"Suburbios", 5, 0xe1dadd88u},
    {"Suburbs", 5, 0xe1dadd88u},
    {"Subúrbio", 5, 0xe1dadd88u},
    {"Προάστια", 5, 0xe1dadd88u},
    {"Předměstí", 5, 0xe1dadd88u},
    {"Banliyö", 5, 0xe1dadd88u},
    {"Banlieue", 5, 0xe1dadd88u},
    {"Sobborghi", 5, 0xe1dadd88u},
    {"Vorstadt", 5, 0xe1dadd88u},
    {"Forstaden", 5, 0xe1dadd88u},
    {"Subúrbios", 5, 0xe1dadd88u},
    {"Hydro-elektrische dam", 6, 0xb1f87254u},
    {"水力発電所", 6, 0xb1f87254u},
    {"수력발전 댐", 6, 0xb1f87254u},
    {"Vesivoimalaitos", 6, 0xb1f87254u},
    {"Vannkraftverk", 6, 0xb1f87254u},
    {"Hydroelektrisk damm", 6, 0xb1f87254u},
    {"水力發電水壩", 6, 0xb1f87254u},
    {"Гидроэлектростанция", 6, 0xb1f87254u},
    {"Hydroelektrownia", 6, 0xb1f87254u},
    {"Represa hidroeléctrica", 6, 0xb1f87254u},
    {"Hydroelectric Dam", 6, 0xb1f87254u},
    {"Usina hidrelétrica", 6, 0xb1f87254u},
    {"Υδροηλεκτρικό Φράγμα", 6, 0xb1f87254u},
    {"Vodní elektrárna", 6, 0xb1f87254u},
    {"Hidroelektrik Santrali", 6, 0xb1f87254u},
    {"Barrage hydroélectrique", 6, 0xb1f87254u},
    {"Diga idroelettrica", 6, 0xb1f87254u},
    {"Hydroelektrischer Damm", 6, 0xb1f87254u},
    {"Presa hidroeléctrica", 6, 0xb1f87254u},
    {"Hydroelektrisk dæmning", 6, 0xb1f87254u},
    {"Barragem Hidroelétrica", 6, 0xb1f87254u},
    {"Ranch", 6, 0xb5396fe3u},
    {"牧場の家", 6, 0xb5396fe3u},
    {"목장", 6, 0xb5396fe3u},
    {"Farmi", 6, 0xb5396fe3u},
    {"Gårdshus", 6, 0xb5396fe3u},
    {"Ranchen", 6, 0xb5396fe3u},
    {"牧場房子", 6, 0xb5396fe3u},
    {"Ранчо", 6, 0xb5396fe3u},
    {"Ranczo", 6, 0xb5396fe3u},
    {"Casa de rancho", 6, 0xb5396fe3u},
    {"Ranch House", 6, 0xb5396fe3u},
    {"Casa de campo", 6, 0xb5396fe3u},
    {"Ράντσο", 6, 0xb5396fe3u},
    {"Ranč", 6, 0xb5396fe3u},
    {"Çiftlik Evi", 6, 0xb5396fe3u},
    {"Il ranch", 6, 0xb5396fe3u},
    {"Farmhaus", 6, 0xb5396fe3u},
    {"Casa del rancho", 6, 0xb5396fe3u},
    {"Ranch-husbygning", 6, 0xb5396fe3u},
    {"Casa do Rancho", 6, 0xb5396fe3u},
    {"Hup Big Horns", 7, 0xb87a493au},
    {"キャンパス", 7, 0xb87a493au},
    {"동부 콜로라도 대학", 7, 0xb87a493au},
    {"Tsemppiä Big Horns", 7, 0xb87a493au},
    {"Heia Big Horns", 7, 0xb87a493au},
    {"Universitet", 7, 0xb87a493au},
    {"上吧，大角羊隊", 7, 0xb87a493au},
    {"Университет", 7, 0xb87a493au},
    {"Do boju, Big Horns!", 7, 0xb87a493au},
    {"Vamos Big Horns", 7, 0xb87a493au},
    {"Go Big Horns", 7, 0xb87a493au},
    {"Vamos, carneiros selvagens", 7, 0xb87a493au},
    {"Πάμε, Big Horns", 7, 0xb87a493au},
    {"Do toho, Big Horns!", 7, 0xb87a493au},
    {"Bastır Büyük Boynuzlar", 7, 0xb87a493au},
    {"Allez les Big Horns", 7, 0xb87a493au},
    {"Forza Big Horns", 7, 0xb87a493au},
    {"Auf geht's, Big Horns", 7, 0xb87a493au},
    {"Adelante, Big Horns", 7, 0xb87a493au},
    {"Heya Bighorns", 7, 0xb87a493au},
    {"Força, Big Horns", 7, 0xb87a493au},
    {"Wetenschapsgebouw", 7, 0xbcbb548du},
    {"研究所", 7, 0xbcbb548du},
    {"과학관", 7, 0xbcbb548du},
    {"Tiederakennus", 7, 0xbcbb548du},
    {"Forskningsbygget", 7, 0xbcbb548du},
    {"Forskningsanläggningen", 7, 0xbcbb548du},
    {"科學大樓", 7, 0xbcbb548du},
    {"Лаборатория", 7, 0xbcbb548du},
    {"Budynek naukowy", 7, 0xbcbb548du},
    {"Edificio de ciencias", 7, 0xbcbb548du},
    {"Science Building", 7, 0xbcbb548du},
    {"Prédio de ciência", 7, 0xbcbb548du},
    {"Κτίριο θετικών επιστημών", 7, 0xbcbb548du},
    {"Výzkumná budova", 7, 0xbcbb548du},
    {"Bilim Laboratuvarı", 7, 0xbcbb548du},
    {"Bâtiment de sciences", 7, 0xbcbb548du},
    {"Edificio scientifico", 7, 0xbcbb548du},
    {"Forschungstrakt", 7, 0xbcbb548du},
    {"Den naturvidenskabelige bygning", 7, 0xbcbb548du},
    {"Edifício Científico", 7, 0xbcbb548du},
    {"De jacht", 8, 0xa2fc0488u},
    {"狩り", 8, 0xa2fc0488u},
    {"사냥", 8, 0xa2fc0488u},
    {"Metsällä", 8, 0xa2fc0488u},
    {"Jakten", 8, 0xa2fc0488u},
    {"狩獵", 8, 0xa2fc0488u},
    {"Лес", 8, 0xa2fc0488u},
    {"Polowanie", 8, 0xa2fc0488u},
    {"La cacería", 8, 0xa2fc0488u},
    {"The Hunt", 8, 0xa2fc0488u},
    {"A caçada", 8, 0xa2fc0488u},
    {"Το Κυνήγι", 8, 0xa2fc0488u},
    {"Lov", 8, 0xa2fc0488u},
    {"Av", 8, 0xa2fc0488u},
    {"La chasse", 8, 0xa2fc0488u},
    {"La caccia", 8, 0xa2fc0488u},
    {"Die Jagd", 8, 0xa2fc0488u},
    {"Jagten", 8, 0xa2fc0488u},
    {"A Caçada", 8, 0xa2fc0488u},
    {"Vakantiehuisje", 8, 0xa63d193fu},
    {"吹雪の街", 8, 0xa63d193fu},
    {"휴양지 오두막", 8, 0xa63d193fu},
    {"Lomakylä", 8, 0xa63d193fu},
    {"Hytteområde", 8, 0xa63d193fu},
    {"Stugbyn", 8, 0xa63d193fu},
    {"木屋度假村", 8, 0xa63d193fu},
    {"Городок", 8, 0xa63d193fu},
    {"Kurort z domkami", 8, 0xa63d193fu},
    {"Complejo turístico de cabañas", 8, 0xa63d193fu},
    {"Cabin Resort", 8, 0xa63d193fu},
    {"Resort com cabanas", 8, 0xa63d193fu},
    {"Καλύβα παραθερισμού", 8, 0xa63d193fu},
    {"Chatová osada", 8, 0xa63d193fu},
    {"Kulübe Kampı", 8, 0xa63d193fu},
    {"Village de vacances", 8, 0xa63d193fu},
    {"Resort baita", 8, 0xa63d193fu},
    {"Ferienhütten", 8, 0xa63d193fu},
    {"Complejo de cabañas", 8, 0xa63d193fu},
    {"Feriehytte", 8, 0xa63d193fu},
    {"Cabana da Estância", 8, 0xa63d193fu},
    {"Afrit", 9, 0xab7e3fe6u},
    {"ハイウェイ出口", 9, 0xab7e3fe6u},
    {"고속도로 출구", 9, 0xab7e3fe6u},
    {"Moottoritien liittymä", 9, 0xab7e3fe6u},
    {"Motorveiavkjørsel", 9, 0xab7e3fe6u},
    {"Motorvägsavfarten", 9, 0xab7e3fe6u},
    {"公路出口", 9, 0xab7e3fe6u},
    {"Шоссе", 9, 0xab7e3fe6u},
    {"Autostrada", 9, 0xab7e3fe6u},
    {"Salida por la autopista", 9, 0xab7e3fe6u},
    {"Highway Exit", 9, 0xab7e3fe6u},
    {"Saída pela rodovia", 9, 0xab7e3fe6u},
    {"Έξοδος Αυτοκινητόδρομου", 9, 0xab7e3fe6u},
    {"Dálniční sjezd", 9, 0xab7e3fe6u},
    {"Otoban Çıkışı", 9, 0xab7e3fe6u},
    {"Sortie d'autoroute", 9, 0xab7e3fe6u},
    {"Uscita autostradale", 9, 0xab7e3fe6u},
    {"Autobahnausfahrt", 9, 0xab7e3fe6u},
    {"Salida de la autopista", 9, 0xab7e3fe6u},
    {"Motorvejsafkørsel", 9, 0xab7e3fe6u},
    {"Saída da Autoestrada", 9, 0xab7e3fe6u},
    {"Ondergrondse tunnel", 9, 0xafbf2251u},
    {"地下トンネル", 9, 0xafbf2251u},
    {"지하 터널", 9, 0xafbf2251u},
    {"Maanalainen tunneli", 9, 0xafbf2251u},
    {"Undergrunnstunnel", 9, 0xafbf2251u},
    {"Underjordisk tunnel", 9, 0xafbf2251u},
    {"地下隧道", 9, 0xafbf2251u},
    {"Подземный туннель", 9, 0xafbf2251u},
    {"Podziemny tunel", 9, 0xafbf2251u},
    {"Túnel subterráneo", 9, 0xafbf2251u},
    {"Underground Tunnel", 9, 0xafbf2251u},
    {"Túnel subterrâneo", 9, 0xafbf2251u},
    {"Υπόγειο τούνελ", 9, 0xafbf2251u},
    {"Podzemní tunel", 9, 0xafbf2251u},
    {"Yeraltı Tüneli", 9, 0xafbf2251u},
    {"Tunnel sous-terrain", 9, 0xafbf2251u},
    {"Tunnel sotterraneo", 9, 0xafbf2251u},
    {"Unterirdischer Tunnel", 9, 0xafbf2251u},
    {"Túnel Subterrâneo", 9, 0xafbf2251u},
    {"Het ziekenhuis", 10, 0x97f09fecu},
    {"奪還", 10, 0x97f09fecu},
    {"병원", 10, 0x97f09fecu},
    {"Sairaala", 10, 0x97f09fecu},
    {"Sykehuset", 10, 0x97f09fecu},
    {"Sjukhuset", 10, 0x97f09fecu},
    {"醫院", 10, 0x97f09fecu},
    {"Больница", 10, 0x97f09fecu},
    {"Szpital", 10, 0x97f09fecu},
    {"El hospital", 10, 0x97f09fecu},
    {"The Hospital", 10, 0x97f09fecu},
    {"O hospital", 10, 0x97f09fecu},
    {"Το Νοσοκομείο", 10, 0x97f09fecu},
    {"Nemocnice", 10, 0x97f09fecu},
    {"Hastane", 10, 0x97f09fecu},
    {"L'hôpital", 10, 0x97f09fecu},
    {"L'ospedale", 10, 0x97f09fecu},
    {"Das Krankenhaus", 10, 0x97f09fecu},
    {"Hospitalet", 10, 0x97f09fecu},
    {"O Hospital", 10, 0x97f09fecu},
    {"Epiloog", 11, 0x9331825bu},
    {"エピローグ", 11, 0x9331825bu},
    {"에필로그", 11, 0x9331825bu},
    {"Epilogi", 11, 0x9331825bu},
    {"Epilog", 11, 0x9331825bu},
    {"尾聲", 11, 0x9331825bu},
    {"Эпилог", 11, 0x9331825bu},
    {"Epílogo", 11, 0x9331825bu},
    {"Epilogue", 11, 0x9331825bu},
    {"Επίλογος", 11, 0x9331825bu},
    {"Kapanış", 11, 0x9331825bu},
    {"Épilogue", 11, 0x9331825bu},
    {"Epilogo", 11, 0x9331825bu},
    /* Left Behind's chapters (dc1/dlc1-tasks.bin's table 0x69f74b66): no sub-chapters, each its own
     * area of that table, LB_AREA_FIRST + its row. */
    {"Back in a Flash", LB_AREA_FIRST + 0, 0x21f81deau},
    {"Mallrats", LB_AREA_FIRST + 1, 0x2cbb3b33u},
    {"So Close", LB_AREA_FIRST + 2, 0x287a2684u},
    {"Fun and Games", LB_AREA_FIRST + 3, 0xf44ab298u},
    {"The Enemy of My Enemy", LB_AREA_FIRST + 4, 0x363d7681u},
    {"Escape from Liberty Gardens", LB_AREA_FIRST + 5, 0xc385cce2u},
    {"Je vais faire super vite", LB_AREA_FIRST + 0, 0x21f81deau},
    {"Shopping nocturne", LB_AREA_FIRST + 1, 0x2cbb3b33u},
    {"Si près", LB_AREA_FIRST + 2, 0x287a2684u},
    {"Plaisir et jeux", LB_AREA_FIRST + 3, 0xf44ab298u},
    {"L'ennemi de mon ennemi", LB_AREA_FIRST + 4, 0x363d7681u},
    {"Fuir Liberty Gardens", LB_AREA_FIRST + 5, 0xc385cce2u},
    {"Vado e torno", LB_AREA_FIRST + 0, 0x21f81deau},
    {"Quelle del centro commerciale", LB_AREA_FIRST + 1, 0x2cbb3b33u},
    {"Per un soffio", LB_AREA_FIRST + 2, 0x287a2684u},
    {"Giochi e passatempi", LB_AREA_FIRST + 3, 0xf44ab298u},
    {"Il nemico del mio nemico", LB_AREA_FIRST + 4, 0x363d7681u},
    {"Fuga dai Liberty Gardens", LB_AREA_FIRST + 5, 0xc385cce2u},
    {"Gleich zurück", LB_AREA_FIRST + 0, 0x21f81deau},
    {"Einkaufszentrum-Kids", LB_AREA_FIRST + 1, 0x2cbb3b33u},
    {"So nah", LB_AREA_FIRST + 2, 0x287a2684u},
    {"Spaß und Spiele", LB_AREA_FIRST + 3, 0xf44ab298u},
    {"Der Feind meines Feindes", LB_AREA_FIRST + 4, 0x363d7681u},
    {"Flucht aus den Liberty Gardens", LB_AREA_FIRST + 5, 0xc385cce2u},
    {"En nada", LB_AREA_FIRST + 0, 0x21f81deau},
    {"Niños de centro comercial", LB_AREA_FIRST + 1, 0x2cbb3b33u},
    {"Por poco", LB_AREA_FIRST + 2, 0x287a2684u},
    {"Diversión y juegos", LB_AREA_FIRST + 3, 0xf44ab298u},
    {"El enemigo de mi enemigo", LB_AREA_FIRST + 4, 0x363d7681u},
    {"Escapa de Liberty Gardens", LB_AREA_FIRST + 5, 0xc385cce2u},
    {"Tilbage som et lyn", LB_AREA_FIRST + 0, 0x21f81deau},
    {"Centerhængere", LB_AREA_FIRST + 1, 0x2cbb3b33u},
    {"Så tæt på", LB_AREA_FIRST + 2, 0x287a2684u},
    {"Sjov og ballade", LB_AREA_FIRST + 3, 0xf44ab298u},
    {"Min fjendes fjende", LB_AREA_FIRST + 4, 0x363d7681u},
    {"Undslip Liberty Gardens", LB_AREA_FIRST + 5, 0xc385cce2u},
    {"Volto Num Instante", LB_AREA_FIRST + 0, 0x21f81deau},
    {"Ratos de Centro Comercial", LB_AREA_FIRST + 1, 0x2cbb3b33u},
    {"Tão Perto", LB_AREA_FIRST + 2, 0x287a2684u},
    {"Jogos e Diversão", LB_AREA_FIRST + 3, 0xf44ab298u},
    {"O Inimigo do Meu Inimigo", LB_AREA_FIRST + 4, 0x363d7681u},
    {"Foge de Liberty Gardens", LB_AREA_FIRST + 5, 0xc385cce2u},
    {"Zo weer terug", LB_AREA_FIRST + 0, 0x21f81deau},
    {"Shoppers", LB_AREA_FIRST + 1, 0x2cbb3b33u},
    {"Zo dichtbij", LB_AREA_FIRST + 2, 0x287a2684u},
    {"Spelletjes", LB_AREA_FIRST + 3, 0xf44ab298u},
    {"De vijand van mijn vijand", LB_AREA_FIRST + 4, 0x363d7681u},
    {"Ontsnap uit Liberty Gardens", LB_AREA_FIRST + 5, 0xc385cce2u},
    {"すぐ戻るから", LB_AREA_FIRST + 0, 0x21f81deau},
    {"モール荒し", LB_AREA_FIRST + 1, 0x2cbb3b33u},
    {"近くて遠い場所", LB_AREA_FIRST + 2, 0x287a2684u},
    {"お遊戯の時間", LB_AREA_FIRST + 3, 0xf44ab298u},
    {"敵の敵", LB_AREA_FIRST + 4, 0x363d7681u},
    {"リバティガーデンからの脱出", LB_AREA_FIRST + 5, 0xc385cce2u},
    {"후딱 갔다 올게요", LB_AREA_FIRST + 0, 0x21f81deau},
    {"쇼핑중독자", LB_AREA_FIRST + 1, 0x2cbb3b33u},
    {"얼마 안 남았어", LB_AREA_FIRST + 2, 0x287a2684u},
    {"장난과 놀이", LB_AREA_FIRST + 3, 0xf44ab298u},
    {"내 적들의 적", LB_AREA_FIRST + 4, 0x363d7681u},
    {"리버티 가든 탈출", LB_AREA_FIRST + 5, 0xc385cce2u},
    {"Paluu hetkessä", LB_AREA_FIRST + 0, 0x21f81deau},
    {"Ostarin jengi", LB_AREA_FIRST + 1, 0x2cbb3b33u},
    {"Niin lähellä", LB_AREA_FIRST + 2, 0x287a2684u},
    {"Leikkiä ja pelejä", LB_AREA_FIRST + 3, 0xf44ab298u},
    {"Viholliseni vihollinen", LB_AREA_FIRST + 4, 0x363d7681u},
    {"Pako Liberty Gardensista", LB_AREA_FIRST + 5, 0xc385cce2u},
    {"Straks tilbake", LB_AREA_FIRST + 0, 0x21f81deau},
    {"Sentervankere", LB_AREA_FIRST + 1, 0x2cbb3b33u},
    {"Så nære", LB_AREA_FIRST + 2, 0x287a2684u},
    {"Bare moro", LB_AREA_FIRST + 3, 0xf44ab298u},
    {"Min fiendes fiende", LB_AREA_FIRST + 4, 0x363d7681u},
    {"Kom deg unna Liberty Gardens", LB_AREA_FIRST + 5, 0xc385cce2u},
    {"Tillbaka på ett kick", LB_AREA_FIRST + 0, 0x21f81deau},
    {"Köpcenterhängare", LB_AREA_FIRST + 1, 0x2cbb3b33u},
    {"Så nära", LB_AREA_FIRST + 2, 0x287a2684u},
    {"Lek och skoj", LB_AREA_FIRST + 3, 0xf44ab298u},
    {"Fly från Liberty Gardens", LB_AREA_FIRST + 5, 0xc385cce2u},
    {"馬上回來", LB_AREA_FIRST + 0, 0x21f81deau},
    {"商場老鼠", LB_AREA_FIRST + 1, 0x2cbb3b33u},
    {"差一點點", LB_AREA_FIRST + 2, 0x287a2684u},
    {"笑話和遊戲", LB_AREA_FIRST + 3, 0xf44ab298u},
    {"我敵人的敵人", LB_AREA_FIRST + 4, 0x363d7681u},
    {"逃離自由花園", LB_AREA_FIRST + 5, 0xc385cce2u},
    {"\"Скоро вернусь\"", LB_AREA_FIRST + 0, 0x21f81deau},
    {"Приключения в торговом центре", LB_AREA_FIRST + 1, 0x2cbb3b33u},
    {"Так близко", LB_AREA_FIRST + 2, 0x287a2684u},
    {"Шутки и проказы", LB_AREA_FIRST + 3, 0xf44ab298u},
    {"Враг моего врага", LB_AREA_FIRST + 4, 0x363d7681u},
    {"Побег из \"Либерти Гарденс\"", LB_AREA_FIRST + 5, 0xc385cce2u},
    {"Zaraz wracam", LB_AREA_FIRST + 0, 0x21f81deau},
    {"Szczury z centrum handlowego", LB_AREA_FIRST + 1, 0x2cbb3b33u},
    {"Tak blisko", LB_AREA_FIRST + 2, 0x287a2684u},
    {"Gry i zabawy", LB_AREA_FIRST + 3, 0xf44ab298u},
    {"Wróg mojego wroga", LB_AREA_FIRST + 4, 0x363d7681u},
    {"Ucieczka z Liberty Gardens", LB_AREA_FIRST + 5, 0xc385cce2u},
    {"De regreso en un abrir y cerrar de ojos", LB_AREA_FIRST + 0, 0x21f81deau},
    {"Ratas del centro comercial", LB_AREA_FIRST + 1, 0x2cbb3b33u},
    {"Tan cerca", LB_AREA_FIRST + 2, 0x287a2684u},
    {"De volta num segundo", LB_AREA_FIRST + 0, 0x21f81deau},
    {"Ratos no shopping", LB_AREA_FIRST + 1, 0x2cbb3b33u},
    {"Por pouco", LB_AREA_FIRST + 2, 0x287a2684u},
    {"Diversão e jogos", LB_AREA_FIRST + 3, 0xf44ab298u},
    {"O inimigo do meu inimigo", LB_AREA_FIRST + 4, 0x363d7681u},
    {"Fuga do Liberty Gardens", LB_AREA_FIRST + 5, 0xc385cce2u},
    {"Επιστρέφω αμέσως", LB_AREA_FIRST + 0, 0x21f81deau},
    {"Παιδιά σε Εμπορικό Κέντρο", LB_AREA_FIRST + 1, 0x2cbb3b33u},
    {"Παρά τρίχα", LB_AREA_FIRST + 2, 0x287a2684u},
    {"Παιχνίδια και διασκέδαση", LB_AREA_FIRST + 3, 0xf44ab298u},
    {"Ο εχθρός του εχθρού μου", LB_AREA_FIRST + 4, 0x363d7681u},
    {"Απόδραση από το Liberty Gardens", LB_AREA_FIRST + 5, 0xc385cce2u},
    {"Za chvilku zpátky", LB_AREA_FIRST + 0, 0x21f81deau},
    {"Flákači", LB_AREA_FIRST + 1, 0x2cbb3b33u},
    {"O fous", LB_AREA_FIRST + 2, 0x287a2684u},
    {"Hry a zábava", LB_AREA_FIRST + 3, 0xf44ab298u},
    {"Nepřítel mého nepřítele", LB_AREA_FIRST + 4, 0x363d7681u},
    {"Unikněte z Liberty Gardens", LB_AREA_FIRST + 5, 0xc385cce2u},
    {"Hemen Dönerim", LB_AREA_FIRST + 0, 0x21f81deau},
    {"Aylak Fareler", LB_AREA_FIRST + 1, 0x2cbb3b33u},
    {"Kıl Payı", LB_AREA_FIRST + 2, 0x287a2684u},
    {"Eğlence", LB_AREA_FIRST + 3, 0xf44ab298u},
    {"Düşmanımın Düşmanı", LB_AREA_FIRST + 4, 0x363d7681u},
    {"Özgürlük Bahçeleri'nden Kaç", LB_AREA_FIRST + 5, 0xc385cce2u},
    /* Not sub-chapters: a save after the credits says COMPLETE (Jackson's picture, the last area),
     * the first one of a new game New Game Data (the Prologue's). */
    {"COMPLETE", 11, 0x4a0d5edcu},
    {"New Game Data", 0, 0xe9610985u},
    {"TERMINÉ", 11, 0x4a0d5edcu},
    {"Nouvelles données de jeu", 0, 0xe9610985u},
    {"COMPLETATO", 11, 0x4a0d5edcu},
    {"Nuovi dati di gioco", 0, 0xe9610985u},
    {"ABGESCHLOSSEN", 11, 0x4a0d5edcu},
    {"Neue Spieldaten", 0, 0xe9610985u},
    {"COMPLETADO", 11, 0x4a0d5edcu},
    {"Nuevos datos de partida", 0, 0xe9610985u},
    {"GENNEMFØRT", 11, 0x4a0d5edcu},
    {"Nye spildata", 0, 0xe9610985u},
    {"CONCLUÍDO", 11, 0x4a0d5edcu},
    {"Novos dados de jogo", 0, 0xe9610985u},
    {"VOLTOOID", 11, 0x4a0d5edcu},
    {"Nieuwe speldata", 0, 0xe9610985u},
    {"完了", 11, 0x4a0d5edcu},
    {"はじめから", 0, 0xe9610985u},
    {"완료", 11, 0x4a0d5edcu},
    {"새 게임 데이터", 0, 0xe9610985u},
    {"VALMIS", 11, 0x4a0d5edcu},
    {"Uusia pelitietoja", 0, 0xe9610985u},
    {"FULLFØRT", 11, 0x4a0d5edcu},
    {"Nye spilldata", 0, 0xe9610985u},
    {"KLAR", 11, 0x4a0d5edcu},
    {"Nya speldata", 0, 0xe9610985u},
    {"完成", 11, 0x4a0d5edcu},
    {"新的遊戲資料", 0, 0xe9610985u},
    {"ГОТОВО", 11, 0x4a0d5edcu},
    {"Новые игровые данные", 0, 0xe9610985u},
    {"ZAKOŃCZONO", 11, 0x4a0d5edcu},
    {"Nowe dane gry", 0, 0xe9610985u},
    {"COMPLETO", 11, 0x4a0d5edcu},
    {"Datos de juego nuevo", 0, 0xe9610985u},
    {"ΟΛΟΚΛΗΡΩΘΗΚΕ", 11, 0x4a0d5edcu},
    {"Δεδομένα νέου παιχνιδιού", 0, 0xe9610985u},
    {"DOKONČENO", 11, 0x4a0d5edcu},
    {"Nová herní data", 0, 0xe9610985u},
    {"TAMAMLANDI", 11, 0x4a0d5edcu},
    {"Yeni Oyun Verisi", 0, 0xe9610985u},
};
/* Their place line is that text alone, without an area. */
#define TEXT_COMPLETE 0x4a0d5edcu
#define TEXT_NEW_GAME_DATA 0xe9610985u

static int area_of(int row) {
    if (areas[row] != AREA_UNKNOWN) return areas[row];
    int area = -1;
    subchapter_text[row] = 0;
    for (size_t i = 0; chapters[row][0] && i < sizeof(subchapters) / sizeof(subchapters[0]); ++i)
        if (!strcmp(subchapters[i].name, chapters[row])) {
            area = subchapters[i].area;
            subchapter_text[row] = subchapters[i].text;
            break;
        }
    /* Beyond the Wall has a second text id (0x6b7c145c, "Beyond the Wall"): the same sub-chapter. */
    const uint32_t text = subchapter_text[row] == 0x6b7c145cu ? 0xeb593133u : subchapter_text[row];
    subchapter_index[row] = -1;
    for (int g = 0; text && g < SUBCHAPTER_COUNT; ++g)
        if (subchapter_order[g] == text) subchapter_index[row] = g;
    areas[row] = area;
    return area;
}

/* The areas' names (dc1/tasks.bin's area table, the game's texts), in the Chapters list's order. */
static const uint32_t area_texts[12] = {0x92a29483u, 0x9fe1b25au, 0x9b20afedu, 0x8567ffe8u, 0x81a6e25fu, 0x8ce5c486u,
                                        0x8824d931u, 0xb06b648cu, 0xb4aa793bu, 0x5a39ee60u, 0x5ef8f3d7u, 0x53bbd50eu};

/* "Area - Sub-chapter" of a row in the game's current language (the save's own sub-chapter name
 * when the text is not there), or "" when the save's chapter is not known. */
static void place_of(int row, char *out, size_t size) {
    out[0] = 0;
    const int area = area_of(row);
    if (area < 0) {
        snprintf(out, size, "%s", chapters[row]);
        return;
    }
    const char *area_name = area < LB_AREA_FIRST ? runtime_menu_pc_game_text(area_texts[area]) : NULL;
    const char *sub_name = subchapter_text[row] ? runtime_menu_pc_game_text(subchapter_text[row]) : NULL;
    if (!sub_name || !sub_name[0]) sub_name = chapters[row];
    const int alone = subchapter_text[row] == TEXT_COMPLETE || subchapter_text[row] == TEXT_NEW_GAME_DATA ||
                      area >= LB_AREA_FIRST;
    if (!alone && area_name && area_name[0] && strcmp(area_name, sub_name)) snprintf(out, size, "%s - %s", area_name, sub_name);
    else snprintf(out, size, "%s", sub_name);
}

/* image(out, self, row, kind) (slot 14, returns out): the row's picture, as the area list
 * provider of the game's Chapters page gives it (its own slot 14, for the save's area); none
 * (that provider's answer for another kind) when the area is not known. */
typedef ABI void *(*ImageSlot)(void *out, void *self, int index, uint32_t kind);
enum { IMAGE_SLOT = 14, MENU_AREA_LIST = 0x1480 };
#define PICTURE_KIND 0x2138a9b2u
static ABI void *row_image(void *out, void *self, int index, uint32_t kind) {
    (void)self;
    const unsigned char *menu = atomic_load(&open_menu);
    unsigned char *areas_list = menu ? (unsigned char *)(uintptr_t)gow3_menu_u64(menu + MENU_AREA_LIST) : NULL;
    if (!areas_list) {
        memset(out, 0, 0x2c);
        return out;
    }
    const ImageSlot image = (ImageSlot)(uintptr_t)gow3_menu_u64(
        (const unsigned char *)(uintptr_t)gow3_menu_u64(areas_list) + 8 * IMAGE_SLOT);
    const int area = index >= 0 && index < atomic_load(&row_count) ? area_of(index) : -1;
    const int row = area >= LB_AREA_FIRST ? area - LB_AREA_FIRST : area;
    void *result = image(out, areas_list, row >= 0 ? row : 0, row >= 0 ? kind : 0);
    /* The picture kind answered with no picture (first word 0, else the art's frame). */
    if (area >= 0 && kind == PICTURE_KIND) {
        const uint32_t frame = gow3_menu_u32(out);
        atomic_store(&no_game_picture[index], frame == 0);
        /* The sub-chapter's own picture: the game's image ids follow the art's cells, column by
         * column (areas 0x57 + area: the third column; the cover art 0x37, then sub-chapter g at
         * 0x38 + g, as the sub-chapter list provider answers 0x38 + its area's first + row). */
        const int sub = subchapter_index[index];
        if (frame != 0 && sub >= 0) {
            const uint32_t id = SUBCHAPTER_IMAGE_FIRST + (uint32_t)sub;
            memcpy(out, &id, 4);
        }
    }
    return result;
}
static ABI int row_min(void *self) {
    (void)self;
    return 0;
}
static ABI int row_max(void *self) {
    (void)self;
    const int count = atomic_load(&row_count);
    return count > 0 ? count - 1 : 0;
}

void runtime_menu_saves_bind(unsigned char *binding) {
    if (!atomic_load(&vtable_ready)) {
        memcpy(vtable, (const void *)(uintptr_t)gow3_menu_u64(binding), sizeof(vtable));
        vtable[GOW3_BINDING_LABEL_SLOT] = (uint64_t)(uintptr_t)row_label;
        vtable[GOW3_BINDING_MIN_SLOT] = (uint64_t)(uintptr_t)row_min;
        vtable[GOW3_BINDING_MAX_SLOT] = (uint64_t)(uintptr_t)row_max;
        vtable[IMAGE_SLOT] = (uint64_t)(uintptr_t)row_image;
        atomic_store(&vtable_ready, 1);
    }
    gow3_menu_put64(binding, (uint64_t)(uintptr_t)vtable);
    gow3_menu_put64(binding + GOW3_BINDING_VALUE, (uint64_t)(uintptr_t)&selected);
    gow3_menu_put32(binding + GOW3_BINDING_MIN, 0);
    gow3_menu_put32(binding + GOW3_BINDING_MAX, (uint32_t)row_max(NULL));
}

const char *runtime_menu_saves_text(Gow3TextLookup lookup, void *table, uint32_t sid) {
    if (sid != GOW3_MENU_SAVES_TITLE) return NULL;
    const int type = atomic_load(&list_type);
    const char *text = lookup(table, type == 1 ? TEXT_SAVE_GAME : TEXT_LOAD_GAME);
    return text ? text : "";
}
