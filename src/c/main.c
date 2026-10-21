#include <pebble.h>

// Uncomment to seed sample history/reminders + a DOWN-button hook that opens the
// DUMPED success screen — lets the emulator render every screen without a phone.
// MUST stay commented for release builds.
// #define DEBUG_SEED 1

// ============================================================================
// CONSTANTS
// ============================================================================

// A note can be several dictation chunks chained together (continuous mode,
// up to the max note length setting), so it holds more than one chunk.
// Sizes scale with app RAM: 128K on emery/gabbro, 64K elsewhere. Pauses cost
// nothing here — only transcribed words fill the buffer.
#if defined(PBL_PLATFORM_EMERY) || defined(PBL_PLATFORM_GABBRO)
#define NOTE_BUF_SIZE     4000   // ~5 minutes of actual speech
#define DICTATION_BUF_SIZE 2048  // one chunk; room for a longer per-chunk OS limit
#define HISTORY_FULL_CHUNKS 20   // 20 × 200 = 4000 = NOTE_BUF_SIZE
#else
#define NOTE_BUF_SIZE     2000
#define DICTATION_BUF_SIZE 1024
#define HISTORY_FULL_CHUNKS  3   // 3 × 200 = 600; 8 slots of more won't fit in 64K
#endif
#define AI_RESPONSE_BUF_SIZE 1100
#define MAX_NOTE_MIN_DEFAULT  1   // stop chaining chunks after this many minutes
#define CONTINUE_DELAY_MS  2000  // "keep talking" pause between chunks
#define SILENT_RESTART_MS   300  // restart delay after a chunk that heard nothing
// Longest take the (modified) PebbleOS records before ending it itself; must
// match CONFIG_VOICE_DICTATION_TIMEOUT_MS. A take that ran that long was cut
// mid-speech, so the next one starts right away instead of after KEEP TALKING.
#define TAKE_CAP_MS       60000
// A failed take at least this long had speech in it that is now lost (the OS
// reports a failed take as "no speech" either way, so length is the tell).
#define LOST_TAKE_MS      15000
#define LOST_RESTART_MS    5000  // after the SOS buzz, before listening again
#define STATUS_BUF_SIZE     48
#define CONV_BUF_SIZE     1200
#define MAX_HISTORY          8
#define HISTORY_SHORT_LEN   48
#define HISTORY_FULL_LEN   (HISTORY_FULL_CHUNK * HISTORY_FULL_CHUNKS)  // note or AI thread

// Persistent storage keys
//
// Key map — check here before claiming a new key:
//   0      history format version      1      history count
//   2      silence timeout (seconds)   4      reminder count
//   5, 6   Quick Launch flags          7, 8   dictation flags
//   10..17 history meta records        18     max note length (minutes)
//   30..45 reminder entries
//   100+   history full-text chunks (100 + slot * HISTORY_FULL_CHUNKS + chunk)
//
// Pebble caps each persist value at 256 bytes, so a history entry can't be one
// blob (short+full+dest+ts ≈ 653 B — the tail fields silently fail to write).
// We split it: a small meta record (short+dest+ts, ~53 B) per slot, plus the
// full transcript chunked across HISTORY_FULL_CHUNKS string keys.
#define PERSIST_HISTORY_VERSION_KEY  0
#define PERSIST_HISTORY_VERSION      6   // v6: full text in HISTORY_FULL_CHUNKS (per platform) keys
#define HISTORY_EXT_ID_LEN          40
#define PERSIST_HISTORY_COUNT        1
#define PERSIST_HISTORY_BASE        10   // meta records at 10..17
#define HISTORY_FULL_CHUNK         200   // bytes per full-text chunk (< 256 persist cap)
#define PERSIST_HIST_FULL_BASE     100   // chunk c of slot s at 100 + s*CHUNKS + c

// Quick Launch behavior flags (set from the phone settings page, persisted
// on-watch so they're available immediately at quick-launch time).
#define PERSIST_QUICK_SKIP_CONFIRM   5
#define PERSIST_QUICK_AUTO_EXIT      6
// Skip the review screen for every note, not just Quick Launch ones.
#define PERSIST_SKIP_CONFIRM         7
// Chain dictation chunks into one note (PebbleOS stops each after ~15s).
#define PERSIST_CONTINUOUS           8
// Continuous mode: end the note after this many seconds without speech
// (0 = never; only Select/Back or the max note length end it).
#define PERSIST_SILENCE_TIMEOUT      2
// Continuous mode: stop chaining chunks this many minutes after the note began.
#define PERSIST_MAX_NOTE_MIN        18
// Mic start/stop vibration presets (0 = off; see s_vibe_start_ms).
#define PERSIST_VIBE_START           3
#define PERSIST_VIBE_STOP            9

// Reminders (local storage, always available)
#define MAX_REMINDERS       16
#define REMINDER_TEXT_LEN  200
#define PERSIST_REM_COUNT    4
#define PERSIST_REM_BASE    30   // entries at 30..45

// Destination indices (must match pkjs)
#define DEST_TASKS   0
#define DEST_NOTION  1
#define DEST_AI      2
#define DEST_WEBHOOK 3
#define DEST_LOCAL   4   // on-watch reminders list (no phone needed)
#define DEST_TODOIST    5
#define DEST_NEXTCLOUD  6
#define DEST_NEXTCLOUD_TASKS 7
#define DEST_DISCORD   8
#define DEST_JOPLIN    9
#define DEST_TIMELINE  10

// Destination bitmask bits (must match pkjs DEST_MASK)
#define DEST_BIT_TASKS     (1 << DEST_TASKS)
#define DEST_BIT_NOTION    (1 << DEST_NOTION)
#define DEST_BIT_AI        (1 << DEST_AI)
#define DEST_BIT_WEBHOOK   (1 << DEST_WEBHOOK)
#define DEST_BIT_TODOIST   (1 << DEST_TODOIST)
#define DEST_BIT_NEXTCLOUD (1 << DEST_NEXTCLOUD)
#define DEST_BIT_NEXTCLOUD_TASKS (1 << DEST_NEXTCLOUD_TASKS)
#define DEST_BIT_DISCORD   (1 << DEST_DISCORD)
#define DEST_BIT_JOPLIN    (1 << DEST_JOPLIN)
#define DEST_BIT_TIMELINE  (1 << DEST_TIMELINE)

// Ink theme — strict black & white
#define C_SCREEN   GColorBlack
#define C_BAR      GColorWhite
#define C_ON_SCREEN GColorWhite
#define C_ON_BAR   GColorBlack

// Layout chrome (Time 2 handoff spec)
// STATUS_H / MENU_ROW_H grow with the OS Text Size pref (see status_h/menu_row_h).
#define STATUS_H              (status_h())
#define STATUS_TITLE_X         4
#define STATUS_CLOCK_W        48
#define STATUS_CLOCK_RIGHT_PAD 3   // inset from screen right (no action bar)
#define STATUS_CLOCK_X(right_x) \
    ((right_x) - STATUS_CLOCK_RIGHT_PAD - STATUS_CLOCK_W)
#define STATUS_CLOCK_BAR_PAD   2
#define STATUS_HDR_TITLE_W(content_w) ((content_w) - STATUS_TITLE_X - 4)
#define ACTION_BAR_W   33
#define MENU_ROW_H     (menu_row_h())
#define ICON_SIZE      18
#define MENU_TILE_SIZE 28
#define MAX_MERGED     (MAX_HISTORY + MAX_REMINDERS)
#define MERGED_META_LEN 40

// ============================================================================
// DATA TYPES
// ============================================================================

// Small meta record (≤256 B) — full transcript stored separately in chunks.
typedef struct __attribute__((packed)) {
    char     short_text[HISTORY_SHORT_LEN];
    uint8_t  dest;
    uint32_t timestamp;
    char     external_id[HISTORY_EXT_ID_LEN];
} HistoryMeta;

typedef struct __attribute__((packed)) {
    char     text[REMINDER_TEXT_LEN];
    uint32_t timestamp;
} ReminderEntry;

// ============================================================================
// GLOBAL STATE
// ============================================================================

// --- Windows ---
static Window *s_home_window;
static Window *s_confirm_window;
static Window *s_response_window;
static Window *s_history_window;
static Window *s_detail_window;
static Window *s_rem_detail_window;
static Window *s_success_window;

// --- Home window layers ---
static Layer       *s_canvas_layer;
static GBitmap     *s_brain_bmp;   // hero brain-mic icon (resource)
static TextLayer   *s_home_title_layer;
static TextLayer   *s_home_clock_layer;   // unused on round (no clock in status bar)
#ifndef PBL_ROUND
static char         s_clock_buf[8];
#endif
static TextLayer   *s_hero_layer;
static TextLayer   *s_meta_layer;

// --- Success ("DUMPED") window ---
static Layer *s_success_canvas_layer;
static int    s_success_dest = DEST_AI;
static AppTimer *s_success_timer = NULL;
// The window doubles as the in-flight screen: pushed as soon as a cloud note
// is sent ("SENDING"), then flipped to "DUMPED" in place when the phone
// confirms. The full transcript scrolls below the destination chip.
static bool         s_success_pending = false;
static bool         s_success_scrolled = false;   // user scrolled: no auto-dismiss
static bool         s_success_exit_on_close = false;  // Quick Launch: closing it ends the app
static ScrollLayer *s_success_scroll_layer;
static TextLayer   *s_success_reply_layer;
static TextLayer   *s_success_note_layer;
static char         s_success_note_text[NOTE_BUF_SIZE + 8];

// --- Send-failed fallback prompt ("Save as local note?") ---
static Window *s_fallback_window = NULL;
static Layer  *s_fallback_canvas = NULL;
static char    s_fallback_err[STATUS_BUF_SIZE];

// --- Confirm window ---
static Layer     *s_confirm_canvas_layer;
static Layer     *s_confirm_divider_layer;
static TextLayer *s_confirm_title_layer;
static TextLayer *s_confirm_content_layer;
static TextLayer *s_confirm_target_layer;
static TextLayer *s_confirm_due_layer;
static bool       s_confirm_is_followup;
static bool       s_confirm_open      = false;
static char       s_confirm_target_buf[32];
static char       s_confirm_due_buf[32];

// --- Response window layers ---
static TextLayer  *s_resp_header_layer;
static ScrollLayer *s_resp_scroll_layer;
static TextLayer  *s_resp_content_layer;

// --- History window (merged smart actions + local notes) ---
typedef enum { MERGED_SMART = 0, MERGED_LOCAL = 1 } MergedKind;

typedef struct {
    MergedKind kind;
    int        src_idx;
    uint8_t    dest;
    uint32_t   timestamp;
    char       title[HISTORY_SHORT_LEN];
    char       meta[MERGED_META_LEN];
} MergedEntry;

// Custom scrolling list (MenuLayer's highlight compositing proved unreliable
// with full-cell custom drawing, so we render rows ourselves).
static Layer     *s_hist_list_layer;
static int        s_hist_sel    = 0;   // selected row
static int        s_hist_scroll = 0;   // scroll offset in px
static int        s_hist_row_w  = 144;
static MergedEntry s_merged[MAX_MERGED];
static int         s_merged_count = 0;

// --- Detail window layers ---
static Layer       *s_detail_canvas_layer;
static TextLayer  *s_detail_header_layer;
static ScrollLayer *s_detail_scroll_layer;
static TextLayer  *s_detail_content_layer;
static bool        s_detail_confirm = false;
static char        s_detail_header_buf[HISTORY_SHORT_LEN];

// --- Dictation ---
static DictationSession *s_dictation_session;

// --- Buffers ---
static char s_note_buf[NOTE_BUF_SIZE];
static char s_ai_response_buf[AI_RESPONSE_BUF_SIZE];
static char s_status_buf[STATUS_BUF_SIZE];
static char s_webhook_msg_buf[120];   // two-way webhook reply shown on success screen
static char s_conversation_buf[CONV_BUF_SIZE];
static int  s_conv_loading_at = -1;   // offset of "..." in s_conversation_buf

// --- State flags ---
static bool    s_waiting_response   = false;
static bool    s_response_open      = false;
static bool    s_is_followup        = false;
static bool    s_in_ai_thread        = false;
static int     s_dest_mask           = DEST_BIT_AI;   // default until phone responds
static bool    s_quick_launch        = false;         // launched via long-press Quick Launch
static bool    s_quick_note          = false;         // THIS note was started by that gesture
static bool    s_cfg_quick_skip_confirm = false;      // route without the review screen
static bool    s_cfg_skip_confirm       = false;      // ...for every note, not just quick ones
static bool    s_cfg_continuous         = false;      // keep listening across chunks
static int     s_cfg_silence_timeout_s  = 0;          // 0 = silence never ends the note
static int     s_cfg_max_note_min       = MAX_NOTE_MIN_DEFAULT;  // continuous-mode cap
#define VIBE_PRESETS        4
#define VIBE_PRESET_DEFAULT 2
static int     s_cfg_vibe_start         = VIBE_PRESET_DEFAULT;
static int     s_cfg_vibe_stop          = VIBE_PRESET_DEFAULT;

// --- Continuous dictation ---
// PebbleOS ends a dictation after ~15s or at the first pause. In continuous
// mode each finished chunk is appended to s_note_buf and, after a short
// "KEEP TALKING" screen (Select = save, Back = cancel), the next chunk starts
// automatically, until the max note length has passed, the silence timeout (if any)
// runs out, or the user stops it (Select while listening also saves; see
// the Select hook below).
static int       s_chunk_count = 0;          // chunks appended to the current note
static uint32_t  s_dict_start_ms = 0;        // when the note's first chunk started
static uint32_t  s_last_speech_ms = 0;       // when a chunk last came back with words
static int       s_chunk_aborts = 0;         // back-to-back chunks the system aborted

// --- Recording log ---
// How long each take of the current note was, sent with the note (NOTE_RECORDING)
// so a webhook can log it. A take runs from the mic turning on to its result,
// so it includes the wait for the transcript. Silent chained takes are only
// counted; takes that failed are listed with why.
#define REC_LOG_SIZE 200
static char      s_rec_log[REC_LOG_SIZE];    // "48s, 1m 12s (not recognized)"
static char      s_rec_summary[REC_LOG_SIZE + 64];
static uint32_t  s_rec_total_ms = 0;
static int       s_rec_takes = 0;
static int       s_rec_silent_n = 0;
static uint32_t  s_rec_silent_ms = 0;
static uint32_t  s_seg_start_ms = 0;         // mic turned on (0 = off)
static uint32_t  s_seg_done_ms = 0;          // last take's length, not yet attributed
static uint32_t  s_seg_end_at = 0;           // when the mic last turned off
static int       s_rec_interruptions = 0;
static Window   *s_continue_window = NULL;
static TextLayer *s_continue_hero_layer;
static TextLayer *s_continue_meta_layer;
static TextLayer *s_continue_text_layer;
static AppTimer *s_continue_timer = NULL;
// What the KEEP TALKING screen is showing: the normal pause between takes,
// a take that was lost (say it again), or a stop because something like a
// notification popped up over the app (wait for the user).
typedef enum { CW_KEEP, CW_LOST, CW_STOPPED } ContinueMode;
static ContinueMode s_continue_mode = CW_KEEP;
static bool      s_note_interrupted = false; // the app lost focus mid-note
static bool    s_cfg_quick_auto_exit    = true;       // exit once the note is saved (default on)
static int16_t s_resp_scroll_offset  = 0;
static int16_t s_detail_scroll_offset = 0;

// --- History ---
static int      s_history_count = 0;
static char     s_history_short[MAX_HISTORY][HISTORY_SHORT_LEN];
// Heap-allocated in init(): on emery/gabbro it's 32K, and an app's static
// footprint (.text + .data + .bss) must stay under 64K on every platform.
static char   (*s_history_full)[HISTORY_FULL_LEN];
static char     s_history_ext_id[MAX_HISTORY][HISTORY_EXT_ID_LEN];
static uint8_t  s_history_dest [MAX_HISTORY];
static uint32_t s_history_ts   [MAX_HISTORY];
static int      s_detail_idx    = 0;

// --- Reminders (local storage) ---
static Layer     *s_rem_detail_canvas_layer;
static TextLayer *s_rem_detail_header;
static ScrollLayer *s_rem_detail_scroll;
static TextLayer *s_rem_detail_content;
static int      s_rem_count      = 0;
static char     s_rem_text[MAX_REMINDERS][REMINDER_TEXT_LEN];
static uint32_t s_rem_ts[MAX_REMINDERS];
static int      s_rem_detail_idx = 0;
static bool     s_rem_confirm    = false;
static int16_t  s_rem_scroll_offset = 0;
static char     s_rem_header_buf[32];

// --- Accessibility: cached OS "Text Size" preference (sampled once in init) ---
static PreferredContentSize s_csize = PreferredContentSizeMedium;

// ============================================================================
// FORWARD DECLARATIONS
// ============================================================================

static void home_window_push(void);
static void confirm_window_push(void);
static void confirm_relayout_meta(GRect b, int content_w);
static void response_window_push(void);
static void history_window_push(void);
static void detail_window_push(int idx);
static void rem_detail_window_push(int display_idx);
static void merged_rebuild(void);
static void success_window_push(int dest);
static void success_window_push_pending(void);
static void success_pending_dismiss(void);
static void fallback_window_push(const char *err);
static void mic_vibe_preview(void);

// ============================================================================
// HELPERS
// ============================================================================

// ----------------------------------------------------------------------------
// Content-size-aware fonts
//
// Pebble exposes the user's system-wide "Text Size" (Settings > Notifications >
// Text Size) via preferred_content_size(). The native UI scales its fonts across
// four tiers; we mirror that table (firmware shell/system_theme.c) so our chrome
// tracks the OS setting. s_csize is sampled once in init() — the pref only changes
// from the system Settings app, which kills us first, so a fresh launch re-reads it.
// ----------------------------------------------------------------------------
typedef enum {
    AF_STATUS = 0,   // status-bar text (clock, screen title) — capped to bar height
    AF_HEADER,       // bold label / list-row title
    AF_TITLE,        // prominent title (hero, DUMPED, empty state)
    AF_BODY,         // long-form reading content (scrollable)
    AF_FOOTER,       // meta / footnote / due / quote
    AF_COUNT
} AppFont;

// key = system font key; h = nominal line height in px (used to size frames).
typedef struct { const char *key; int16_t h; } AppFontSpec;

static const AppFontSpec s_app_fonts[NumPreferredContentSizes][AF_COUNT] = {
    [PreferredContentSizeSmall] = {
        [AF_STATUS] = { FONT_KEY_GOTHIC_14_BOLD, 16 },
        [AF_HEADER] = { FONT_KEY_GOTHIC_18_BOLD, 21 },
        [AF_TITLE]  = { FONT_KEY_GOTHIC_18_BOLD, 21 },
        [AF_BODY]   = { FONT_KEY_GOTHIC_18,      21 },
        [AF_FOOTER] = { FONT_KEY_GOTHIC_14,      16 },
    },
    [PreferredContentSizeMedium] = {
        [AF_STATUS] = { FONT_KEY_GOTHIC_14_BOLD, 16 },
        [AF_HEADER] = { FONT_KEY_GOTHIC_18_BOLD, 21 },
        [AF_TITLE]  = { FONT_KEY_GOTHIC_24_BOLD, 28 },
        [AF_BODY]   = { FONT_KEY_GOTHIC_24_BOLD, 28 },
        [AF_FOOTER] = { FONT_KEY_GOTHIC_18,      21 },
    },
    [PreferredContentSizeLarge] = {
        [AF_STATUS] = { FONT_KEY_GOTHIC_18_BOLD, 21 },
        [AF_HEADER] = { FONT_KEY_GOTHIC_24_BOLD, 28 },
        [AF_TITLE]  = { FONT_KEY_GOTHIC_28_BOLD, 33 },
        [AF_BODY]   = { FONT_KEY_GOTHIC_28,      33 },
        [AF_FOOTER] = { FONT_KEY_GOTHIC_18,      21 },
    },
    [PreferredContentSizeExtraLarge] = {
        // No GOTHIC_36 system font key in the app SDK (firmware-only), so XL
        // Title/Body cap at 28 — still a clear step up from the Large tier's body.
        [AF_STATUS] = { FONT_KEY_GOTHIC_18_BOLD, 21 },
        [AF_HEADER] = { FONT_KEY_GOTHIC_28_BOLD, 33 },
        [AF_TITLE]  = { FONT_KEY_GOTHIC_28_BOLD, 33 },
        [AF_BODY]   = { FONT_KEY_GOTHIC_28,      33 },
        [AF_FOOTER] = { FONT_KEY_GOTHIC_24,      28 },
    },
};

static const char *app_font_key(AppFont f) { return s_app_fonts[s_csize][f].key; }
static GFont app_font(AppFont f)           { return fonts_get_system_font(app_font_key(f)); }
static int  app_font_h(AppFont f)          { return s_app_fonts[s_csize][f].h; }

// Status bar grows so the (scaled) clock + title still fit: 18px at S/M, 23 at L/XL.
static int status_h(void)   { return app_font_h(AF_STATUS) + 2; }
// History row grows to hold a scaled title (HEADER) + meta line (FOOTER).
static int menu_row_h(void) { return app_font_h(AF_HEADER) + app_font_h(AF_FOOTER) + 14; }

// Full product name for a destination — used in every user-facing label.
static const char *dest_full_name(int dest) {
    switch (dest) {
        case DEST_TASKS:     return "Google Tasks";
        case DEST_NOTION:    return "Notion";
        case DEST_AI:        return "AI";
        case DEST_WEBHOOK:   return "Webhook";
        case DEST_LOCAL:     return "Local Note";
        case DEST_TODOIST:   return "Todoist";
        case DEST_NEXTCLOUD: return "Nextcloud Notes";
        case DEST_NEXTCLOUD_TASKS: return "Nextcloud Tasks";
        case DEST_DISCORD:   return "Discord";
        case DEST_JOPLIN:    return "Joplin";
        case DEST_TIMELINE:  return "Timeline";
        default:             return "?";
    }
}

static int content_area_w(GRect bounds) {
    int w = bounds.size.w - ACTION_BAR_W;
    return (w < 20) ? bounds.size.w : w;
}

static int action_bar_x(GRect bounds) {
    return bounds.size.w - ACTION_BAR_W;
}

static int btn_y_at(GRect bounds, int pct) {
    return bounds.size.h * pct / 100;
}

// Shared vertical positions for action-bar icons, used on every screen so the
// UP and SELECT glyphs line up identically. SELECT is the exact middle (the
// physical middle button); UP sits above it (a bit lower on round to clear the
// top curve and match the physical button).
#ifdef PBL_ROUND
#  define BTN_UP_PCT 38
#else
#  define BTN_UP_PCT 20
#endif
static int btn_up_y(GRect b)     { return b.size.h * BTN_UP_PCT / 100; }
static int btn_select_y(GRect b) { return b.size.h / 2; }

// "Destination · <when>": time only if today, date + time if an earlier day.
static void format_relative_meta(uint32_t ts, int dest, char *buf, size_t len) {
    const char *name = dest_full_name(dest);

    time_t tt = (time_t)ts;
    struct tm entry = *localtime(&tt);          // copy — localtime returns a shared buffer
    time_t nowt = time(NULL);
    struct tm now = *localtime(&nowt);
    bool today = (entry.tm_year == now.tm_year && entry.tm_yday == now.tm_yday);

    char when[28];
    bool h24 = clock_is_24h_style();
    if (today) {
        strftime(when, sizeof(when), h24 ? "%H:%M" : "%l:%M %p", &entry);
    } else {
        strftime(when, sizeof(when), h24 ? "%d %b %H:%M" : "%d %b %l:%M %p", &entry);
    }
    // Trim any leading space %l can produce for single-digit hours.
    char *wp = when; while (*wp == ' ') wp++;
    snprintf(buf, len, "%s \xC2\xB7 %s", name, wp);
}

static bool is_phone_connected(void) {
    return connection_service_peek_pebble_app_connection();
}

static uint32_t now_ms(void) {
    time_t s; uint16_t ms;
    time_ms(&s, &ms);
    return (uint32_t)s * 1000 + ms;
}

static void rec_format_ms(char *buf, size_t n, uint32_t ms) {
    unsigned secs = (unsigned)((ms + 500) / 1000);
    if (secs < 60) snprintf(buf, n, "%us", secs);
    else           snprintf(buf, n, "%um %us", secs / 60, secs % 60);
}

static void rec_log_reset(void) {
    s_rec_log[0] = '\0';
    s_rec_summary[0] = '\0';
    s_rec_total_ms = 0;
    s_rec_takes = 0;
    s_rec_silent_n = 0;
    s_rec_silent_ms = 0;
    s_seg_start_ms = 0;
    s_seg_done_ms = 0;
    s_rec_interruptions = 0;
}

// One take: its length and, if it failed, why (NULL = it worked).
static void rec_log_add(uint32_t ms, const char *why) {
    char t[16];
    rec_format_ms(t, sizeof(t), ms);
    size_t used = strlen(s_rec_log);
    snprintf(s_rec_log + used, REC_LOG_SIZE - used, "%s%s%s%s%s",
             used ? ", " : "", t, why ? " (" : "", why ? why : "", why ? ")" : "");
    s_rec_total_ms += ms;
    s_rec_takes++;
}

static void rec_log_silent(uint32_t ms) {
    s_rec_silent_n++;
    s_rec_silent_ms += ms;
    s_rec_total_ms += ms;
}

// "48s, 1m 12s (not recognized); 3 silent restarts (21s); total 2m 21s"
static void rec_log_finish(void) {
    char t[16];
    snprintf(s_rec_summary, sizeof(s_rec_summary), "%s", s_rec_log);
    size_t used = strlen(s_rec_summary);
    if (s_rec_silent_n > 0) {
        rec_format_ms(t, sizeof(t), s_rec_silent_ms);
        snprintf(s_rec_summary + used, sizeof(s_rec_summary) - used, "%s%d silent restart%s (%s)",
                 used ? "; " : "", s_rec_silent_n, s_rec_silent_n == 1 ? "" : "s", t);
        used = strlen(s_rec_summary);
    }
    if (s_rec_takes + s_rec_silent_n > 1) {
        rec_format_ms(t, sizeof(t), s_rec_total_ms);
        snprintf(s_rec_summary + used, sizeof(s_rec_summary) - used, "; total %s", t);
        used = strlen(s_rec_summary);
    }
    if (s_rec_interruptions > 0) {
        snprintf(s_rec_summary + used, sizeof(s_rec_summary) - used,
                 "; interrupted by a notification");
    }
}

static void refresh_home_idle_ui(void) {
    if (!s_hero_layer || !s_meta_layer) return;
    if (strcmp(s_status_buf, "Ready") != 0) return;
    text_layer_set_text(s_hero_layer, "READY");
    text_layer_set_text(s_meta_layer,
        is_phone_connected() ? "SELECT saves\nBACK cancels" : "NO PHONE · LOCAL ONLY");
}

static void set_status(const char *msg) {
    strncpy(s_status_buf, msg, STATUS_BUF_SIZE - 1);
    s_status_buf[STATUS_BUF_SIZE - 1] = '\0';
    if (!s_hero_layer) return;
    if (strcmp(msg, "Ready") == 0) {
        refresh_home_idle_ui();
    } else {
        text_layer_set_text(s_hero_layer, msg);
        if (s_meta_layer) text_layer_set_text(s_meta_layer, "");
    }
    if (s_canvas_layer) layer_mark_dirty(s_canvas_layer);
}

// ============================================================================
// PERSISTENT HISTORY
// ============================================================================

// Scratch buffers for persist read/write — kept in BSS (not stack) to avoid
// stack overflow. HistoryEntry is 653 bytes; allocating two on the stack
// simultaneously (history_save_item → history_load_from_persist) would consume
// ~1.3 KB of the ~2.5 KB app stack and trigger a hard fault.
static HistoryMeta   s_hist_meta_buf;
static ReminderEntry s_rem_entry_buf;
static char          s_hist_chunk_buf[HISTORY_FULL_CHUNK + 1];

// Write a transcript across HISTORY_FULL_CHUNKS string keys for the given slot.
// Stops after the first short (or empty) chunk, which is where reading stops,
// so a short note doesn't rewrite all of the slot's keys.
static void history_write_full(int slot, const char *full) {
    int len = (int)strlen(full);
    for (int c = 0; c < HISTORY_FULL_CHUNKS && c * HISTORY_FULL_CHUNK <= len; c++) {
        uint32_t key = (uint32_t)(PERSIST_HIST_FULL_BASE + slot * HISTORY_FULL_CHUNKS + c);
        strncpy(s_hist_chunk_buf, full + c * HISTORY_FULL_CHUNK, HISTORY_FULL_CHUNK);
        s_hist_chunk_buf[HISTORY_FULL_CHUNK] = '\0';
        persist_write_string(key, s_hist_chunk_buf);
    }
}

// Reassemble a transcript from its chunk keys into dst.
static void history_read_full(int slot, char *dst, int dstlen) {
    dst[0] = '\0';
    int used = 0;
    for (int c = 0; c < HISTORY_FULL_CHUNKS; c++) {
        uint32_t key = (uint32_t)(PERSIST_HIST_FULL_BASE + slot * HISTORY_FULL_CHUNKS + c);
        if (!persist_exists(key)) break;
        int n = persist_read_string(key, s_hist_chunk_buf, sizeof(s_hist_chunk_buf));
        if (n <= 0) break;
        int clen = (int)strlen(s_hist_chunk_buf);
        if (used + clen >= dstlen) clen = dstlen - 1 - used;
        if (clen <= 0) break;
        memcpy(dst + used, s_hist_chunk_buf, clen);
        used += clen;
        dst[used] = '\0';
        if (clen < HISTORY_FULL_CHUNK) break;   // last chunk
    }
}

static void history_load_from_persist(void) {
    // Wipe history if schema version has changed
    if (!persist_exists(PERSIST_HISTORY_VERSION_KEY) ||
        persist_read_int(PERSIST_HISTORY_VERSION_KEY) != PERSIST_HISTORY_VERSION) {
        persist_write_int(PERSIST_HISTORY_VERSION_KEY, PERSIST_HISTORY_VERSION);
        persist_write_int(PERSIST_HISTORY_COUNT, 0);
        s_history_count = 0;
        return;
    }

    int stored = persist_exists(PERSIST_HISTORY_COUNT)
                 ? persist_read_int(PERSIST_HISTORY_COUNT) : 0;
    int n = (stored > MAX_HISTORY) ? MAX_HISTORY : stored;
    s_history_count = 0;

    // Read most-recent first (slot = (stored-1-i) % MAX_HISTORY)
    for (int i = 0; i < n; i++) {
        int slot = ((stored - 1 - i) % MAX_HISTORY + MAX_HISTORY) % MAX_HISTORY;
        uint32_t key = (uint32_t)(PERSIST_HISTORY_BASE + slot);
        if (!persist_exists(key)) continue;
        if (persist_read_data(key, &s_hist_meta_buf, sizeof(s_hist_meta_buf)) < 0) continue;
        strncpy(s_history_short[s_history_count], s_hist_meta_buf.short_text, HISTORY_SHORT_LEN - 1);
        history_read_full(slot, s_history_full[s_history_count], HISTORY_FULL_LEN);
        s_history_dest[s_history_count] = s_hist_meta_buf.dest;
        s_history_ts  [s_history_count] = s_hist_meta_buf.timestamp;
        strncpy(s_history_ext_id[s_history_count], s_hist_meta_buf.external_id,
                HISTORY_EXT_ID_LEN - 1);
        s_history_ext_id[s_history_count][HISTORY_EXT_ID_LEN - 1] = '\0';
        s_history_count++;
    }
}

// question → short_text (list label); response → full_text (detail view).
// Pass NULL for response to use question as both (non-AI destinations).
static void history_save_item(const char *question, const char *response, int dest,
                              const char *external_id) {
    int stored = persist_exists(PERSIST_HISTORY_COUNT)
                 ? persist_read_int(PERSIST_HISTORY_COUNT) : 0;
    int slot = stored % MAX_HISTORY;

    memset(&s_hist_meta_buf, 0, sizeof(s_hist_meta_buf));
    strncpy(s_hist_meta_buf.short_text, question, HISTORY_SHORT_LEN - 1);
    s_hist_meta_buf.dest      = (uint8_t)dest;
    s_hist_meta_buf.timestamp = (uint32_t)time(NULL);
    if (external_id && external_id[0]) {
        strncpy(s_hist_meta_buf.external_id, external_id, HISTORY_EXT_ID_LEN - 1);
    }

    persist_write_data((uint32_t)(PERSIST_HISTORY_BASE + slot), &s_hist_meta_buf, sizeof(s_hist_meta_buf));
    history_write_full(slot, response ? response : question);
    persist_write_int(PERSIST_HISTORY_COUNT, stored + 1);

    history_load_from_persist();
}

// Update the full_text of the most recently saved entry (for follow-up responses).
static void history_update_latest(const char *response) {
    int stored = persist_exists(PERSIST_HISTORY_COUNT)
                 ? persist_read_int(PERSIST_HISTORY_COUNT) : 0;
    if (stored == 0) return;
    int slot = ((stored - 1) % MAX_HISTORY + MAX_HISTORY) % MAX_HISTORY;
    history_write_full(slot, response);
    // Also update in-memory display (most-recent is at index 0)
    if (s_history_count > 0) {
        strncpy(s_history_full[0], response, HISTORY_FULL_LEN - 1);
        s_history_full[0][HISTORY_FULL_LEN - 1] = '\0';
    }
}

static void history_rewrite_persist(void) {
    int n = s_history_count;
    persist_write_int(PERSIST_HISTORY_COUNT, n);
    for (int i = 0; i < n; i++) {
        int slot = ((n - 1 - i) % MAX_HISTORY + MAX_HISTORY) % MAX_HISTORY;
        strncpy(s_hist_meta_buf.short_text, s_history_short[i], HISTORY_SHORT_LEN - 1);
        s_hist_meta_buf.short_text[HISTORY_SHORT_LEN - 1] = '\0';
        s_hist_meta_buf.dest = s_history_dest[i];
        s_hist_meta_buf.timestamp = s_history_ts[i];
        strncpy(s_hist_meta_buf.external_id, s_history_ext_id[i], HISTORY_EXT_ID_LEN - 1);
        persist_write_data((uint32_t)(PERSIST_HISTORY_BASE + slot),
                           &s_hist_meta_buf, sizeof(s_hist_meta_buf));
        history_write_full(slot, s_history_full[i]);
    }
}

// display_idx: 0 = newest (same order as s_history_* arrays).
static void appmsg_complete_task(const char *external_id);

static void history_delete(int display_idx) {
    if (display_idx < 0 || display_idx >= s_history_count) return;
    if (s_history_dest[display_idx] == DEST_TASKS &&
        s_history_ext_id[display_idx][0]) {
        appmsg_complete_task(s_history_ext_id[display_idx]);
    }
    for (int i = display_idx; i < s_history_count - 1; i++) {
        strncpy(s_history_short[i], s_history_short[i + 1], HISTORY_SHORT_LEN);
        memmove(s_history_full[i], s_history_full[i + 1], HISTORY_FULL_LEN);
        strncpy(s_history_ext_id[i], s_history_ext_id[i + 1], HISTORY_EXT_ID_LEN);
        s_history_dest[i] = s_history_dest[i + 1];
        s_history_ts[i] = s_history_ts[i + 1];
    }
    s_history_count--;
    history_rewrite_persist();
}

// ============================================================================
// PERSISTENT REMINDERS
// ============================================================================

static void reminders_save_to_persist(void) {
    persist_write_int(PERSIST_REM_COUNT, s_rem_count);
    for (int i = 0; i < s_rem_count; i++) {
        memset(&s_rem_entry_buf, 0, sizeof(s_rem_entry_buf));
        strncpy(s_rem_entry_buf.text, s_rem_text[i], REMINDER_TEXT_LEN - 1);
        s_rem_entry_buf.timestamp = s_rem_ts[i];
        persist_write_data((uint32_t)(PERSIST_REM_BASE + i), &s_rem_entry_buf, sizeof(s_rem_entry_buf));
    }
}

static void reminders_load_from_persist(void) {
    int n = persist_exists(PERSIST_REM_COUNT)
            ? persist_read_int(PERSIST_REM_COUNT) : 0;
    if (n > MAX_REMINDERS) n = MAX_REMINDERS;
    s_rem_count = 0;
    for (int i = 0; i < n; i++) {
        uint32_t key = (uint32_t)(PERSIST_REM_BASE + i);
        if (!persist_exists(key)) continue;
        if (persist_read_data(key, &s_rem_entry_buf, sizeof(s_rem_entry_buf)) < 0) continue;
        strncpy(s_rem_text[s_rem_count], s_rem_entry_buf.text, REMINDER_TEXT_LEN - 1);
        s_rem_ts[s_rem_count] = s_rem_entry_buf.timestamp;
        s_rem_count++;
    }
}

static void reminders_add(const char *text) {
    if (s_rem_count >= MAX_REMINDERS) {
        // Drop oldest entry (index 0), compact
        for (int i = 0; i < MAX_REMINDERS - 1; i++) {
            strncpy(s_rem_text[i], s_rem_text[i + 1], REMINDER_TEXT_LEN);
            s_rem_ts[i] = s_rem_ts[i + 1];
        }
        s_rem_count = MAX_REMINDERS - 1;
    }
    strncpy(s_rem_text[s_rem_count], text, REMINDER_TEXT_LEN - 1);
    s_rem_text[s_rem_count][REMINDER_TEXT_LEN - 1] = '\0';
    s_rem_ts[s_rem_count] = (uint32_t)time(NULL);
    s_rem_count++;
    reminders_save_to_persist();
}

// display_idx: 0 = newest (displayed first), maps to storage index s_rem_count-1-display_idx
static void reminders_delete(int display_idx) {
    int si = s_rem_count - 1 - display_idx;
    if (si < 0 || si >= s_rem_count) return;
    for (int i = si; i < s_rem_count - 1; i++) {
        strncpy(s_rem_text[i], s_rem_text[i + 1], REMINDER_TEXT_LEN);
        s_rem_ts[i] = s_rem_ts[i + 1];
    }
    s_rem_count--;
    reminders_save_to_persist();
}

// ============================================================================
// CONVERSATION BUFFER
// ============================================================================

static void conv_update_display(void);  // forward decl

static void conv_start(const char *question) {
    snprintf(s_conversation_buf, CONV_BUF_SIZE, "YOU: %.120s\n\n", question);
    s_conv_loading_at = (int)strlen(s_conversation_buf);
    snprintf(s_conversation_buf + s_conv_loading_at,
             CONV_BUF_SIZE - s_conv_loading_at, "AI: [thinking]");
}

static void conv_append_question(const char *question) {
    size_t used = strlen(s_conversation_buf);
    size_t avail = CONV_BUF_SIZE - used;
    if (avail < 20) return;
    snprintf(s_conversation_buf + used, avail, "\n\nYOU: %.120s\n\n", question);
    s_conv_loading_at = (int)strlen(s_conversation_buf);
    snprintf(s_conversation_buf + s_conv_loading_at,
             CONV_BUF_SIZE - s_conv_loading_at, "AI: [thinking]");
}

static void conv_set_response(const char *response) {
    if (s_conv_loading_at < 0 || s_conv_loading_at >= CONV_BUF_SIZE) return;
    snprintf(s_conversation_buf + s_conv_loading_at,
             CONV_BUF_SIZE - s_conv_loading_at, "AI: %s", response);
    s_conv_loading_at = -1;
}

static void conv_update_display(void) {
    if (!s_resp_content_layer || !s_resp_scroll_layer) return;
    text_layer_set_text(s_resp_content_layer, s_conversation_buf);
    GSize text_size = text_layer_get_content_size(s_resp_content_layer);
    GRect fr = layer_get_frame(scroll_layer_get_layer(s_resp_scroll_layer));
    int content_w = fr.size.w;
    scroll_layer_set_content_size(s_resp_scroll_layer,
        GSize(content_w, text_size.h + 24));
    // Scroll to bottom so latest content is visible
    int16_t max_scroll = text_size.h + 24 - fr.size.h;
    if (max_scroll < 0) max_scroll = 0;
    s_resp_scroll_offset = max_scroll;
    scroll_layer_set_content_offset(s_resp_scroll_layer,
        GPoint(0, -max_scroll), true);
}

// ============================================================================
// APPMESSAGE — OUTBOX
// ============================================================================

static void appmsg_send_note(const char *text, bool is_followup) {
    DictionaryIterator *iter;
    if (app_message_outbox_begin(&iter) != APP_MSG_OK) {
        set_status("Send error");
        return;
    }
    dict_write_cstring(iter, MESSAGE_KEY_NOTE_TEXT, text);
    dict_write_int8(iter, MESSAGE_KEY_NOTE_TYPE, 0);
    dict_write_int8(iter, MESSAGE_KEY_NOTE_IS_FOLLOWUP, is_followup ? 1 : 0);
    dict_write_int8(iter, MESSAGE_KEY_CLOCK_24H, clock_is_24h_style() ? 1 : 0);
    if (!is_followup && s_rec_summary[0]) {
        dict_write_cstring(iter, MESSAGE_KEY_NOTE_RECORDING, s_rec_summary);
        APP_LOG(APP_LOG_LEVEL_INFO, "Recording: %s", s_rec_summary);
    }
    if (app_message_outbox_send() == APP_MSG_OK) {
        set_status("Routing...");
        s_waiting_response = true;
    } else {
        set_status("Send failed");
    }
}

static void appmsg_clear_context(void) {
    DictionaryIterator *iter;
    if (app_message_outbox_begin(&iter) != APP_MSG_OK) return;
    dict_write_int8(iter, MESSAGE_KEY_CLEAR_CONTEXT, 1);
    app_message_outbox_send();
}

static void appmsg_classify_note(const char *text) {
    DictionaryIterator *iter;
    if (app_message_outbox_begin(&iter) != APP_MSG_OK) return;
    dict_write_cstring(iter, MESSAGE_KEY_NOTE_TEXT, text);
    dict_write_int8(iter, MESSAGE_KEY_NOTE_IS_CLASSIFY_ONLY, 1);
    dict_write_int8(iter, MESSAGE_KEY_CLOCK_24H, clock_is_24h_style() ? 1 : 0);
    app_message_outbox_send();
}

static void appmsg_complete_task(const char *external_id) {
    DictionaryIterator *iter;
    if (app_message_outbox_begin(&iter) != APP_MSG_OK) return;
    dict_write_int8(iter, MESSAGE_KEY_COMPLETE_TASK, 1);
    dict_write_cstring(iter, MESSAGE_KEY_EXTERNAL_ID, external_id);
    app_message_outbox_send();
}

// Tell the phone the user picked "Retry later" on the send-failed fallback, so
// it queues the note that just failed for a later retry.
static void appmsg_queue_retry(void) {
    DictionaryIterator *iter;
    if (app_message_outbox_begin(&iter) != APP_MSG_OK) return;
    dict_write_int8(iter, MESSAGE_KEY_QUEUE_RETRY, 1);
    app_message_outbox_send();
}

// Quick Launch + auto-exit: once the note is safely stored, buzz for eyes-free
// feedback and arm the success screen to end the app when it closes (after
// SUCCESS_DISMISS_MS, or on Select/Back if the user scrolled to read). Failure
// paths never call this, so errors still surface on-screen.
static void quick_exit_arm(void) {
    if (!(s_quick_note && s_cfg_quick_auto_exit)) return;
    vibes_short_pulse();
    s_success_exit_on_close = true;
}

// The exit reason is what sends the user back to the watchface: without it
// PebbleOS returns to the launcher, which is not "out of my way" at all.
static void quick_exit_now(void) {
    exit_reason_set(APP_EXIT_ACTION_PERFORMED_SUCCESSFULLY);
    window_stack_pop_all(true);
}

static void route_note(bool is_followup) {
    if (!is_phone_connected()) {
        if (is_followup) {
            set_status("Connect phone for AI");
            return;
        }
        reminders_add(s_note_buf);
        set_status("Saved locally ✓");
        quick_exit_arm();
        success_window_push(DEST_LOCAL);
        return;
    }
    if (s_dest_mask == 0 && !is_followup) {
        reminders_add(s_note_buf);
        set_status("Saved to reminders");
        quick_exit_arm();
        success_window_push(DEST_LOCAL);
    } else {
        if (is_followup) {
            conv_append_question(s_note_buf);
            conv_update_display();
        } else {
            s_in_ai_thread = false;
            conv_start(s_note_buf);
        }
        appmsg_send_note(s_note_buf, is_followup);
        // Show the full transcript while the phone works on it; the same window
        // turns into the DUMPED screen once the send is confirmed.
        if (!is_followup) success_window_push_pending();
    }
}

// ============================================================================
// APPMESSAGE — INBOX
// ============================================================================

static int vibe_preset_clamp(int v) {
    return (v >= 0 && v < VIBE_PRESETS) ? v : VIBE_PRESET_DEFAULT;
}

static int max_note_min_clamp(int v) {
    return (v >= 1 && v <= 30) ? v : MAX_NOTE_MIN_DEFAULT;
}

static void inbox_received_callback(DictionaryIterator *iter, void *context) {
    // DEST_MASK — sent by phone on ready
    Tuple *mask_t = dict_find(iter, MESSAGE_KEY_DEST_MASK);
    if (mask_t) {
        s_dest_mask = (int)mask_t->value->int32;
        APP_LOG(APP_LOG_LEVEL_INFO, "dest_mask=%d", s_dest_mask);
    }

    // Quick Launch behavior flags — persisted so the next quick launch has them
    // before the phone connection is even up.
    Tuple *qsc_t = dict_find(iter, MESSAGE_KEY_QUICK_SKIP_CONFIRM);
    if (qsc_t) {
        s_cfg_quick_skip_confirm = qsc_t->value->int32 != 0;
        persist_write_bool(PERSIST_QUICK_SKIP_CONFIRM, s_cfg_quick_skip_confirm);
    }
    Tuple *qae_t = dict_find(iter, MESSAGE_KEY_QUICK_AUTO_EXIT);
    if (qae_t) {
        s_cfg_quick_auto_exit = qae_t->value->int32 != 0;
        persist_write_bool(PERSIST_QUICK_AUTO_EXIT, s_cfg_quick_auto_exit);
    }
    Tuple *sc_t = dict_find(iter, MESSAGE_KEY_SKIP_CONFIRM);
    if (sc_t) {
        s_cfg_skip_confirm = sc_t->value->int32 != 0;
        persist_write_bool(PERSIST_SKIP_CONFIRM, s_cfg_skip_confirm);
    }
    Tuple *cd_t = dict_find(iter, MESSAGE_KEY_CONTINUOUS_DICTATION);
    if (cd_t) {
        s_cfg_continuous = cd_t->value->int32 != 0;
        persist_write_bool(PERSIST_CONTINUOUS, s_cfg_continuous);
    }
    Tuple *st_t = dict_find(iter, MESSAGE_KEY_SILENCE_TIMEOUT);
    if (st_t) {
        s_cfg_silence_timeout_s = (int)st_t->value->int32;
        persist_write_int(PERSIST_SILENCE_TIMEOUT, s_cfg_silence_timeout_s);
    }
    Tuple *mn_t = dict_find(iter, MESSAGE_KEY_MAX_NOTE_MIN);
    if (mn_t) {
        s_cfg_max_note_min = max_note_min_clamp((int)mn_t->value->int32);
        persist_write_int(PERSIST_MAX_NOTE_MIN, s_cfg_max_note_min);
    }
    Tuple *vs_t = dict_find(iter, MESSAGE_KEY_VIBE_START);
    if (vs_t) {
        s_cfg_vibe_start = vibe_preset_clamp((int)vs_t->value->int32);
        persist_write_int(PERSIST_VIBE_START, s_cfg_vibe_start);
    }
    Tuple *vp_t = dict_find(iter, MESSAGE_KEY_VIBE_STOP);
    if (vp_t) {
        s_cfg_vibe_stop = vibe_preset_clamp((int)vp_t->value->int32);
        persist_write_int(PERSIST_VIBE_STOP, s_cfg_vibe_stop);
    }
    // Sent when the settings page saved new mic vibes, so they can be felt.
    if (dict_find(iter, MESSAGE_KEY_VIBE_PREVIEW)) mic_vibe_preview();

    // ROUTING_DONE — JS has decided destination
    Tuple *routing_t = dict_find(iter, MESSAGE_KEY_ROUTING_DONE);
    if (routing_t) {
        int dest = (int)routing_t->value->int32;
        if (s_confirm_open && s_confirm_target_layer) {
            // Classify-only response — update target label on confirm screen
            snprintf(s_confirm_target_buf, sizeof(s_confirm_target_buf),
                     "\xe2\x86\x92 %s", dest_full_name(dest));
            text_layer_set_text(s_confirm_target_layer, s_confirm_target_buf);
            // Extracted due date/time, shown under the destination
            Tuple *due_t = dict_find(iter, MESSAGE_KEY_DUE_LABEL);
            if (due_t && due_t->value->cstring[0] && s_confirm_due_layer) {
                snprintf(s_confirm_due_buf, sizeof(s_confirm_due_buf),
                         "due %s", due_t->value->cstring);
                text_layer_set_text(s_confirm_due_layer, s_confirm_due_buf);
            } else {
                s_confirm_due_buf[0] = '\0';
                text_layer_set_text(s_confirm_due_layer, s_confirm_due_buf);
            }
            if (s_confirm_window) {
                Layer *root = window_get_root_layer(s_confirm_window);
                confirm_relayout_meta(layer_get_bounds(root), content_area_w(layer_get_bounds(root)) - 8);
            }
        } else {
            if (dest == DEST_AI) {
                // The answer gets its own response window; drop the SENDING
                // screen so it isn't left behind underneath.
                success_pending_dismiss();
                set_status("Waiting for answer...");
            } else {
                set_status("Taking action...");
                if (s_success_pending) {
                    s_success_dest = dest;
                    if (s_success_canvas_layer) layer_mark_dirty(s_success_canvas_layer);
                }
            }
        }
    }

    // CONFIRM — note was stored (non-AI destination)
    Tuple *confirm_t = dict_find(iter, MESSAGE_KEY_CONFIRM);
    if (confirm_t) {
        int ok = (int)confirm_t->value->int32;
        if (ok == 1) {
            Tuple *dest_t = dict_find(iter, MESSAGE_KEY_DEST_USED);
            int dest = dest_t ? (int)dest_t->value->int32 : 0;
            s_webhook_msg_buf[0] = '\0';
            if (dest == DEST_LOCAL) {
                reminders_add(s_note_buf);
                set_status("Saved locally ✓");
            } else {
                Tuple *ext_t = dict_find(iter, MESSAGE_KEY_EXTERNAL_ID);
                const char *ext_id = (ext_t && ext_t->value->cstring[0])
                                     ? ext_t->value->cstring : NULL;
                history_save_item(s_note_buf, NULL, dest, ext_id);
                // Two-way webhook: stash the endpoint's reply to show on the success screen.
                Tuple *webhook_msg_t = dict_find(iter, MESSAGE_KEY_WEBHOOK_MSG);
                if (webhook_msg_t && webhook_msg_t->value->cstring[0]) {
                    strncpy(s_webhook_msg_buf, webhook_msg_t->value->cstring, sizeof(s_webhook_msg_buf) - 1);
                    s_webhook_msg_buf[sizeof(s_webhook_msg_buf) - 1] = '\0';
                }
                char msg[STATUS_BUF_SIZE];
                snprintf(msg, sizeof(msg), "Sent → %s ✓", dest_full_name(dest));
                set_status(msg);
            }
            quick_exit_arm();
            success_window_push(dest);
        } else {
            Tuple *err_t = dict_find(iter, MESSAGE_KEY_ERROR_MSG);
            const char *emsg = (err_t && err_t->value->cstring[0])
                             ? err_t->value->cstring : "Error — check phone";
            success_pending_dismiss();
            // A first-turn send failed — offer to keep the note locally so it
            // isn't lost. Followups (AI thread) just report the error.
            if (!s_is_followup && s_note_buf[0]) {
                fallback_window_push(emsg);
            } else {
                set_status(emsg);
            }
        }
        s_waiting_response = false;
    }

    // AI_RESPONSE — show on response window
    Tuple *ai_t = dict_find(iter, MESSAGE_KEY_AI_RESPONSE);
    if (ai_t) {
        strncpy(s_ai_response_buf, ai_t->value->cstring, AI_RESPONSE_BUF_SIZE - 1);
        s_ai_response_buf[AI_RESPONSE_BUF_SIZE - 1] = '\0';
    }

    Tuple *done_t = dict_find(iter, MESSAGE_KEY_AI_RESPONSE_DONE);
    if (done_t && done_t->value->int32 == 1) {
        s_waiting_response = false;
        set_status("AI responded");
        conv_set_response(s_ai_response_buf);
        if (s_in_ai_thread) {
            history_update_latest(s_conversation_buf);
        } else {
            Tuple *dest_t = dict_find(iter, MESSAGE_KEY_DEST_USED);
            int dest = dest_t ? (int)dest_t->value->int32 : DEST_AI;
            history_save_item(s_note_buf, s_conversation_buf, dest, NULL);
            s_in_ai_thread = true;
        }
        if (s_response_open) {
            conv_update_display();
        } else {
            response_window_push();
        }
    }
}

static void inbox_dropped_callback(AppMessageResult reason, void *context) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "Inbox dropped: %d", (int)reason);
}

static void outbox_failed_callback(DictionaryIterator *iter,
                                   AppMessageResult reason, void *context) {
    success_pending_dismiss();
    set_status("Send failed");
    s_waiting_response = false;
}

// ============================================================================
// DICTATION CALLBACK
// ============================================================================

// The note is complete (one chunk, or the last of a continuous chain): review
// it, or route it straight away when the review screen is skipped.
static void continue_window_close(void) {
    s_chunk_count = 0;
    s_note_interrupted = false;
    if (s_continue_timer) { app_timer_cancel(s_continue_timer); s_continue_timer = NULL; }
    if (s_continue_window && window_stack_contains_window(s_continue_window)) {
        window_stack_remove(s_continue_window, false);
    }
}

static void dictation_finish(void) {
    rec_log_finish();
    bool is_followup = s_is_followup;
    s_is_followup = false;   // consume flag
    continue_window_close();
    // Every take was lost (or silent) and the user pressed Select anyway.
    if (!s_note_buf[0]) {
        set_status("Nothing was captured");
        return;
    }
    // Hands-off capture: route straight away, no review screen — for every
    // note, or only Quick Launch ones. Follow-ups (AI thread) keep the
    // review step — they're already interactive.
    if (!is_followup && (s_cfg_skip_confirm || (s_quick_note && s_cfg_quick_skip_confirm))) {
        route_note(false);
        return;
    }
    s_confirm_is_followup = is_followup;
    confirm_window_push();
}

// Back, while recording or on the KEEP TALKING screen: throw the whole note away.
static void dictation_cancel(void) {
    s_is_followup = false;   // consume flag
    continue_window_close();
    s_note_buf[0] = '\0';
    set_status("Ready");
}

// Another chunk only while continuous mode is on, there's time left before
// the max note length, and the buffer has room for more. AI follow-ups stay
// single-chunk. Pauses count toward the time, so it's generous by design.
static bool dictation_wants_more(void) {
    if (!s_cfg_continuous || s_is_followup) return false;
    if (now_ms() - s_dict_start_ms > (uint32_t)s_cfg_max_note_min * 60000 - 3000) return false;
    return strlen(s_note_buf) + 120 < NOTE_BUF_SIZE;
}

// --- Telling Select apart from a pause ---
// PebbleOS reports a chunk ended by Select exactly like one ended by a pause
// or the 15s cap. But its mic window lives on this app's window stack, so we
// wrap that window's click config provider to also subscribe a raw Select
// handler (raw handlers fire alongside the OS's own click handler). The OS
// re-sets the provider on every state change, so a short poll re-wraps it.
//
// The same poll tells when the mic is live. The window starts with a
// Back-only provider and switches to a Back+Select one exactly while it is
// recording (the OS mic is already on by then), then back again when
// recording ends. The OS gives no vibe of its own, so we buzz on both edges.
// A stop the phone triggers (a pause) only shows up once the transcript
// arrives, so that buzz lags the mic by the transcription time; Select and
// the 15s cap are immediate.
#define VOICE_HOOK_POLL_MS 50
static Window             *s_voice_window = NULL;
static ClickConfigProvider s_voice_orig_click = NULL;
static ClickConfigProvider s_voice_idle_click = NULL;   // the Back-only provider
static AppTimer           *s_voice_hook_timer = NULL;
static bool                s_voice_select_pressed = false;
static bool                s_voice_listening = false;

// Three pulses: the mic is on, start talking. One buzz: it's off. The
// settings page picks a preset for each; index 0 is off, and the order must
// match VIBE_START_PRESETS / VIBE_STOP_PRESETS in pkjs. Apps can't raise the
// motor's strength (that's the watch's System vibe intensity setting), only
// the length.
static const uint32_t s_vibe_start_ms[VIBE_PRESETS][2] = {   // { pulse, gap }
    { 0, 0 }, { 100, 60 }, { 175, 75 }, { 250, 150 },
};
static const uint32_t s_vibe_stop_ms[VIBE_PRESETS] = { 0, 400, 700, 1000 };

// Plays the start and/or stop preset (0 = skip), with gap_ms of quiet
// between them when both play.
static void mic_vibe_play(int start, int stop, uint32_t gap_ms) {
    uint32_t segs[7];   // alternating on/off, starting with on
    int n = 0;
    for (int i = 0; start > 0 && i < 3; i++) {
        if (i) segs[n++] = s_vibe_start_ms[start][1];
        segs[n++] = s_vibe_start_ms[start][0];
    }
    if (stop > 0) {
        if (n) segs[n++] = gap_ms;
        segs[n++] = s_vibe_stop_ms[stop];
    }
    if (n) vibes_enqueue_custom_pattern((VibePattern) { .durations = segs, .num_segments = n });
}

static void mic_vibe_preview(void) {
    mic_vibe_play(s_cfg_vibe_start, s_cfg_vibe_stop, 1000);
}

// SOS (... --- ...): what was just said is lost, say it again. ~4s.
static void lost_vibe_play(void) {
    static const uint32_t segs[] = {
        150, 150, 150, 150, 150, 450,    // S
        450, 150, 450, 150, 450, 450,    // O
        150, 150, 150, 150, 150,         // S
    };
    vibes_cancel();
    vibes_enqueue_custom_pattern((VibePattern) {
        .durations = segs, .num_segments = ARRAY_LENGTH(segs) });
}

static void voice_listening_set(bool listening) {
    if (listening == s_voice_listening) return;
    s_voice_listening = listening;
    if (listening) {
        // A take that ended without reaching dictation_callback was one the
        // system retried itself ("Missed that. Try again.").
        if (s_seg_done_ms) { rec_log_add(s_seg_done_ms, "missed, retried"); s_seg_done_ms = 0; }
        s_seg_start_ms = now_ms();
    } else if (s_seg_start_ms) {
        s_seg_end_at = now_ms();
        s_seg_done_ms = s_seg_end_at - s_seg_start_ms;
        s_seg_start_ms = 0;
    }
    APP_LOG(APP_LOG_LEVEL_DEBUG, "Mic %s", listening ? "listening" : "stopped");
    if (listening) mic_vibe_play(s_cfg_vibe_start, 0, 0);
    else           mic_vibe_play(0, s_cfg_vibe_stop, 0);
}

static void voice_select_down(ClickRecognizerRef rec, void *ctx) {
    s_voice_select_pressed = true;
}

static void voice_click_wrap(void *ctx) {
    if (s_voice_orig_click) s_voice_orig_click(ctx);
    window_raw_click_subscribe(BUTTON_ID_SELECT, voice_select_down, NULL, NULL);
}

static void voice_hook_poll(void *ctx) {
    s_voice_hook_timer = NULL;
    if (!s_voice_window || !window_stack_contains_window(s_voice_window)) return;
    ClickConfigProvider p = window_get_click_config_provider(s_voice_window);
    if (p != voice_click_wrap) {
        if (p) {
            if (!s_voice_idle_click) s_voice_idle_click = p;
            else voice_listening_set(p != s_voice_idle_click);
        }
        s_voice_orig_click = p;
        window_set_click_config_provider_with_context(s_voice_window, voice_click_wrap,
            window_get_click_config_context(s_voice_window));
    }
    s_voice_hook_timer = app_timer_register(VOICE_HOOK_POLL_MS, voice_hook_poll, NULL);
}

static void voice_hook_stop(void) {
    if (s_voice_hook_timer) { app_timer_cancel(s_voice_hook_timer); s_voice_hook_timer = NULL; }
    // The window can close before the poll sees it stop recording.
    voice_listening_set(false);
    s_voice_window = NULL;
    s_voice_idle_click = NULL;
}

// Start a dictation chunk with the Select hook on its mic window.
static void dictation_chunk_start(void) {
    voice_hook_stop();
    s_voice_select_pressed = false;
    Window *below = window_stack_get_top_window();
    if (dictation_session_start(s_dictation_session) != DictationSessionStatusSuccess) return;
    Window *top = window_stack_get_top_window();
    if (top && top != below) {
        s_voice_window = top;
        voice_hook_poll(NULL);
    }
}

static void continue_timer_cb(void *ctx) {
    s_continue_timer = NULL;
    s_continue_mode = CW_KEEP;
    dictation_chunk_start();
}

static void continue_save_click(ClickRecognizerRef rec, void *ctx) {
    dictation_finish();
}

static void continue_cancel_click(ClickRecognizerRef rec, void *ctx) {
    dictation_cancel();
}

static void continue_window_refresh(uint32_t delay_ms);

// UP after a stop: pick the note back up.
static void continue_resume_click(ClickRecognizerRef rec, void *ctx) {
    if (s_continue_mode != CW_STOPPED) return;
    s_note_interrupted = false;
    s_continue_mode = CW_KEEP;
    continue_window_refresh(CONTINUE_DELAY_MS);
}

static void continue_click_config(void *ctx) {
    window_single_click_subscribe(BUTTON_ID_SELECT, continue_save_click);
    window_single_click_subscribe(BUTTON_ID_BACK,   continue_cancel_click);
    window_single_click_subscribe(BUTTON_ID_UP,     continue_resume_click);
}

// Show the tail of what's been captured so far, so the user can pick up the
// thread, and re-arm the countdown to the next chunk. The tail starts at a
// word and is trimmed from the front until it fits, so the last words show.
static void continue_window_refresh(uint32_t delay_ms) {
    if (!s_continue_text_layer) return;
    static const char *const hero[] = { "KEEP TALKING", "SAY THAT AGAIN", "STOPPED" };
    static const char *const meta[] = {
        "SELECT saves\nBACK cancels",
        "Last part was lost\nSELECT saves the rest",
        "A notification came in\nSELECT saves · UP resumes",
    };
    text_layer_set_text(s_continue_hero_layer, hero[s_continue_mode]);
    text_layer_set_text(s_continue_meta_layer, meta[s_continue_mode]);
    Layer *tl = text_layer_get_layer(s_continue_text_layer);
    GRect frame = layer_get_frame(tl);
    size_t len = strlen(s_note_buf);
    const char *tail = len > 90 ? s_note_buf + len - 90 : s_note_buf;
    layer_set_frame(tl, GRect(frame.origin.x, frame.origin.y, frame.size.w, 2000));
    for (;;) {
        if (tail != s_note_buf) {
            while (*tail && *tail != ' ') tail++;   // start on a word boundary
            while (*tail == ' ') tail++;
        }
        text_layer_set_text(s_continue_text_layer, tail);
        if (!*tail || text_layer_get_content_size(s_continue_text_layer).h <= frame.size.h) break;
        tail++;
    }
    layer_set_frame(tl, frame);
    if (s_continue_timer) { app_timer_cancel(s_continue_timer); s_continue_timer = NULL; }
    // Stopped: nothing listens again until the user says so.
    if (s_continue_mode != CW_STOPPED) {
        s_continue_timer = app_timer_register(delay_ms, continue_timer_cb, NULL);
    }
}

static TextLayer *continue_text_layer(GRect frame, GFont font, GColor color) {
    TextLayer *t = text_layer_create(frame);
    text_layer_set_background_color(t, GColorClear);
    text_layer_set_text_color(t, color);
    text_layer_set_font(t, font);
    text_layer_set_text_alignment(t, GTextAlignmentCenter);
    text_layer_set_overflow_mode(t, GTextOverflowModeWordWrap);
    return t;
}

static void continue_window_load(Window *window) {
    Layer *root = window_get_root_layer(window);
    GRect b = layer_get_bounds(root);
    window_set_background_color(window, C_SCREEN);
    int y = b.size.h * 18 / 100;
    int hero_h = app_font_h(AF_TITLE) + 6;
    s_continue_hero_layer = continue_text_layer(GRect(0, y, b.size.w, hero_h),
                                                app_font(AF_TITLE), C_ON_SCREEN);
    text_layer_set_text(s_continue_hero_layer, "KEEP TALKING");
    layer_add_child(root, text_layer_get_layer(s_continue_hero_layer));
    y += hero_h;
    int meta_h = app_font_h(AF_STATUS) * 2 + 4;
    s_continue_meta_layer = continue_text_layer(GRect(0, y, b.size.w, meta_h),
                                                app_font(AF_STATUS), GColorLightGray);
    text_layer_set_text(s_continue_meta_layer, "SELECT saves\nBACK cancels");
    layer_add_child(root, text_layer_get_layer(s_continue_meta_layer));
    y += meta_h + 8;
    s_continue_text_layer = continue_text_layer(GRect(8, y, b.size.w - 16, b.size.h - y),
                                                app_font(AF_BODY), GColorLightGray);
    layer_add_child(root, text_layer_get_layer(s_continue_text_layer));
    window_set_click_config_provider(window, continue_click_config);
}

static void continue_window_unload(Window *window) {
    text_layer_destroy(s_continue_text_layer); s_continue_text_layer = NULL;
    text_layer_destroy(s_continue_meta_layer); s_continue_meta_layer = NULL;
    text_layer_destroy(s_continue_hero_layer); s_continue_hero_layer = NULL;
}

// Between chunks: the window stays in the stack under the dictation UI, so
// each later chunk just refreshes it.
static void continue_window_show(uint32_t delay_ms) {
    if (!s_continue_window) {
        s_continue_window = window_create();
        window_set_window_handlers(s_continue_window, (WindowHandlers) {
            .load   = continue_window_load,
            .unload = continue_window_unload,
        });
    }
    if (!window_stack_contains_window(s_continue_window)) {
        window_stack_push(s_continue_window, false);
    }
    if (s_note_interrupted) s_continue_mode = CW_STOPPED;
    continue_window_refresh(delay_ms);
}

// A note is in progress while a take is recording or KEEP TALKING is up.
static bool note_in_progress(void) {
    return s_voice_window ||
           (s_continue_window && window_stack_contains_window(s_continue_window));
}

// Something (a notification, a system alert) popped up over the app. It
// breaks the take that's recording, and anything said from here on goes
// nowhere: buzz SOS and stop the chain until the user comes back to it.
static void app_focus_changed(bool in_focus) {
    if (in_focus || !note_in_progress() || s_note_interrupted) return;
    APP_LOG(APP_LOG_LEVEL_WARNING, "Note interrupted (app lost focus)");
    s_note_interrupted = true;
    s_rec_interruptions++;
    lost_vibe_play();
    // Between takes: stop here. Mid-take: dictation_callback shows STOPPED.
    if (!s_voice_window) {
        s_continue_mode = CW_STOPPED;
        continue_window_refresh(0);
    }
}

static void dictation_callback(DictationSession *session,
                                DictationSessionStatus status,
                                char *transcription, void *context) {
    bool chained = s_chunk_count > 0;
    // Select while listening (or processing) means "that's the whole note".
    bool user_stopped = s_voice_select_pressed;
    voice_hook_stop();
    uint32_t take_ms = s_seg_done_ms;   // this take's length (0 if the mic never came on)
    s_seg_done_ms = 0;

    if (status == DictationSessionStatusSuccess) {
        if (!chained) {
            strncpy(s_note_buf, transcription, NOTE_BUF_SIZE - 1);
            s_note_buf[NOTE_BUF_SIZE - 1] = '\0';
        } else {
            size_t used = strlen(s_note_buf);
            snprintf(s_note_buf + used, NOTE_BUF_SIZE - used, " %s", transcription);
        }
        s_chunk_count++;
        s_chunk_aborts = 0;
        s_last_speech_ms = now_ms();
        rec_log_add(take_ms, NULL);
        APP_LOG(APP_LOG_LEVEL_INFO, "Dictation chunk %d: %s", s_chunk_count, transcription);
        if (!user_stopped && dictation_wants_more()) {
            // Cut off by the take limit mid-speech: keep listening right away.
            continue_window_show(take_ms + 2000 >= TAKE_CAP_MS ? SILENT_RESTART_MS
                                                               : CONTINUE_DELAY_MS);
            return;
        }
        dictation_finish();
        return;
    }

    // Back during recording cancels the whole note, chained or not.
    if (status == DictationSessionStatusFailureTranscriptionRejected ||
        status == DictationSessionStatusFailureTranscriptionRejectedWithError) {
        dictation_cancel();
        return;
    }

    // A long take that failed had speech in it, and those words are gone.
    // Buzz SOS so the user knows to say it again; keep what came before.
    if (take_ms >= LOST_TAKE_MS) {
        // How long the phone took to answer, to tell real failures (which
        // reportedly take a few seconds) from silent takes.
        char why[32];
        uint32_t wait_ms = now_ms() - s_seg_end_at;
        snprintf(why, sizeof(why), "lost, failed after %u.%us",
                 (unsigned)(wait_ms / 1000), (unsigned)(wait_ms % 1000 / 100));
        rec_log_add(take_ms, why);
        if (!s_note_interrupted) lost_vibe_play();   // it already buzzed
        APP_LOG(APP_LOG_LEVEL_WARNING, "Lost a %lu ms take (status %d)",
                (unsigned long)take_ms, (int)status);
        if (s_cfg_continuous && !s_is_followup && !user_stopped && dictation_wants_more()) {
            s_continue_mode = CW_LOST;
            continue_window_show(LOST_RESTART_MS);
            return;
        }
        if (chained) { dictation_finish(); return; }
        s_is_followup = false;   // consume flag
        set_status("Lost that. Try again");
        return;
    }

    if (chained) {
        // A chained chunk that heard nothing (or nothing it could make out)
        // is a pause: listen again, unless the silence timeout has run out.
        // The system also aborts a session that fails to start (with error
        // dialogs off); retry that a couple of times before giving up.
        if (status == DictationSessionStatusFailureSystemAborted) s_chunk_aborts++;
        bool silent = status == DictationSessionStatusFailureNoSpeechDetected ||
                      status == DictationSessionStatusFailureRecognizerError ||
                      (status == DictationSessionStatusFailureSystemAborted && s_chunk_aborts <= 2);
        if (take_ms) {
            if (status == DictationSessionStatusFailureNoSpeechDetected) rec_log_silent(take_ms);
            else rec_log_add(take_ms,
                status == DictationSessionStatusFailureRecognizerError ? "not recognized" :
                status == DictationSessionStatusFailureConnectivityError ? "no connection" :
                status == DictationSessionStatusFailureSystemAborted ? "aborted" : "error");
        }
        bool timed_out = s_cfg_silence_timeout_s > 0 &&
            now_ms() - s_last_speech_ms >= (uint32_t)s_cfg_silence_timeout_s * 1000;
        if (silent && !timed_out && !user_stopped && dictation_wants_more()) {
            continue_window_show(SILENT_RESTART_MS);
            return;
        }
        // Anything else (an error, time is up) keeps what was captured.
        dictation_finish();
        return;
    }

    s_is_followup = false;   // consume flag
    if (s_note_interrupted) {
        s_note_interrupted = false;
        set_status("Stopped: a notification came in");
        return;
    }
    switch (status) {
        case DictationSessionStatusFailureNoSpeechDetected:
            set_status("No speech detected"); break;
        case DictationSessionStatusFailureConnectivityError:
            set_status("No connection"); break;
        case DictationSessionStatusFailureRecognizerError:
            set_status("Transcription error"); break;
        case DictationSessionStatusFailureDisabled:
            set_status("Dictation disabled"); break;
        default:
            set_status("Dictation failed"); break;
    }
}

// ============================================================================
// DRAW — CHROME & ICONS
// ============================================================================

// Straight vertical inverted strip hugging the right edge. On round the
// circular display naturally clips the top/bottom, but the inner (left) edge
// stays vertical — same treatment as rectangular.
static void draw_action_bar(GContext *ctx, GRect bounds) {
    graphics_context_set_fill_color(ctx, C_BAR);
    graphics_fill_rect(ctx, GRect(action_bar_x(bounds), 0, ACTION_BAR_W, bounds.size.h),
                       0, GCornerNone);
}

// Icons are black on the white strip on every platform.
#define ACTION_ICON_COLOR C_ON_BAR

static int action_bar_icon_x(GRect bounds) {
    return action_bar_x(bounds) + ACTION_BAR_W / 2;
}

// Current time, top-right of the status bar. Round drops the clock (centered
// title only), so these are rectangular-only.
#ifndef PBL_ROUND
static void refresh_clock_buf(void) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    if (clock_is_24h_style()) {
        snprintf(s_clock_buf, sizeof(s_clock_buf), "%02d:%02d", t->tm_hour, t->tm_min);
    } else {
        int h = t->tm_hour % 12; if (h == 0) h = 12;
        snprintf(s_clock_buf, sizeof(s_clock_buf), "%d:%02d", h, t->tm_min);
    }
}

// Draw the clock on the dark header (full-width screens without an action bar).
static void draw_status_clock(GContext *ctx, int right_x) {
    refresh_clock_buf();
    graphics_context_set_text_color(ctx, C_ON_SCREEN);
    // Clock stays compact (14_BOLD) at all tiers — it lives in a fixed-width strip.
    graphics_draw_text(ctx, s_clock_buf, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
        GRect(STATUS_CLOCK_X(right_x), 1, STATUS_CLOCK_W, STATUS_H),
        GTextOverflowModeFill, GTextAlignmentRight, NULL);
}

static GRect status_clock_bar_rect(GRect bounds) {
    int x = action_bar_x(bounds) + STATUS_CLOCK_BAR_PAD;
    return GRect(x, 1, bounds.size.w - x - STATUS_CLOCK_BAR_PAD, STATUS_H);
}

// Clock tucked into the action bar — black on white, flush right.
static void draw_status_clock_on_bar(GContext *ctx, GRect bounds) {
    refresh_clock_buf();
    graphics_context_set_text_color(ctx, C_ON_BAR);
    graphics_draw_text(ctx, s_clock_buf, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
        status_clock_bar_rect(bounds),
        GTextOverflowModeFill, GTextAlignmentRight, NULL);
}

static void add_home_clock_layer(Layer *root, GRect bounds) {
    refresh_clock_buf();
    s_home_clock_layer = text_layer_create(status_clock_bar_rect(bounds));
    text_layer_set_background_color(s_home_clock_layer, GColorClear);
    text_layer_set_text_color(s_home_clock_layer, C_ON_BAR);
    text_layer_set_font(s_home_clock_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD));
    text_layer_set_text_alignment(s_home_clock_layer, GTextAlignmentRight);
    text_layer_set_text(s_home_clock_layer, s_clock_buf);
    layer_add_child(root, text_layer_get_layer(s_home_clock_layer));
}
#endif

// 1px hairline beneath the status bar. White so it vanishes over the action bar.
static void draw_status_divider(GContext *ctx, GRect bounds) {
    graphics_context_set_stroke_color(ctx, C_BAR);
    graphics_context_set_stroke_width(ctx, 1);
    int w = bounds.size.w;
    graphics_draw_line(ctx, GPoint(4, STATUS_H), GPoint(w - 4, STATUS_H));
}

static void status_divider_layer_update(Layer *layer, GContext *ctx) {
    GRect b = layer_get_bounds(layer);
    graphics_context_set_stroke_color(ctx, C_BAR);
    graphics_context_set_stroke_width(ctx, 1);
    graphics_draw_line(ctx, GPoint(4, 0), GPoint(b.size.w - 4, 0));
}

static Layer *add_status_divider_layer(Layer *root, int width) {
    Layer *layer = layer_create(GRect(0, STATUS_H - 1, width, 1));
    layer_set_update_proc(layer, status_divider_layer_update);
    layer_add_child(root, layer);
    return layer;
}

// Record / dump button — a single filled disk centered at c.
static void draw_icon_record(GContext *ctx, GPoint c, GColor color) {
    graphics_context_set_fill_color(ctx, color);
    graphics_fill_circle(ctx, c, 8);
}

// History glyph: three lines, third ~60% width (paragraph, not a burger menu).
static void draw_icon_hamburger(GContext *ctx, GPoint c, GColor color) {
    graphics_context_set_stroke_color(ctx, color);
    graphics_context_set_stroke_width(ctx, 2);
    graphics_draw_line(ctx, GPoint(c.x - 7, c.y - 4), GPoint(c.x + 7, c.y - 4));
    graphics_draw_line(ctx, GPoint(c.x - 7, c.y),     GPoint(c.x + 7, c.y));
    graphics_draw_line(ctx, GPoint(c.x - 7, c.y + 4), GPoint(c.x + 1, c.y + 4));
}

// Review-screen redo: clockwise arc; arrowhead at the right (start) end,
// pointing up-left (almost horizontal) — the redo/back direction.
static void draw_icon_redo_review(GContext *ctx, GPoint c, GColor color) {
    GPoint rc = GPoint(c.x + 1, c.y);
    graphics_context_set_stroke_color(ctx, color);
    graphics_context_set_stroke_width(ctx, 3);
    int r = 7;
    GRect box = GRect(rc.x - r, rc.y - r, 2 * r, 2 * r + 1);
    int32_t a_start = DEG_TO_TRIGANGLE(40);
    int32_t a_end   = DEG_TO_TRIGANGLE(315);
    graphics_draw_arc(ctx, box, GOvalScaleModeFitCircle, a_start, a_end);

    graphics_context_set_stroke_width(ctx, 2);
    // Pebble: 0° = 12 o'clock, angles increase clockwise.
    // Arc runs clockwise from a_start (upper-right) to a_end (upper-left).
    int ca = cos_lookup(a_start), sa = sin_lookup(a_start);
    int px = rc.x + sa * r / TRIG_MAX_RATIO - 2;
    int py = rc.y - ca * r / TRIG_MAX_RATIO - 2;
    // Clockwise tangent; arrow points opposite (up-left at this end).
    int Tx = ca, Ty = sa;
    int Nx = -sa, Ny = ca;
    int L = 3;
    GPoint tip = GPoint(px, py);
    GPoint b1 = GPoint(px + (Tx + Nx) * L / TRIG_MAX_RATIO,
                       py + (Ty + Ny) * L / TRIG_MAX_RATIO);
    GPoint b2 = GPoint(px + (Tx - Nx) * L / TRIG_MAX_RATIO,
                       py + (Ty - Ny) * L / TRIG_MAX_RATIO);
    graphics_draw_line(ctx, b1, tip);
    graphics_draw_line(ctx, b2, tip);
}

// Send: a right-pointing arrow.
static void draw_icon_send(GContext *ctx, GPoint c, GColor color) {
    graphics_context_set_stroke_color(ctx, color);
    graphics_context_set_stroke_width(ctx, 2);
    graphics_draw_line(ctx, GPoint(c.x - 7, c.y), GPoint(c.x + 7, c.y));
    graphics_draw_line(ctx, GPoint(c.x + 7, c.y), GPoint(c.x + 1, c.y - 5));
    graphics_draw_line(ctx, GPoint(c.x + 7, c.y), GPoint(c.x + 1, c.y + 5));
}

static void draw_icon_reply(GContext *ctx, GPoint c, GColor color) {
    draw_icon_send(ctx, c, color);
}

// Trash can — solid filled delete affordance on history detail.
static void draw_icon_trash(GContext *ctx, GPoint c, GColor color) {
    graphics_context_set_fill_color(ctx, color);
    graphics_fill_rect(ctx, GRect(c.x - 3, c.y - 9, 6, 2), 0, GCornerNone);
    graphics_fill_rect(ctx, GRect(c.x - 7, c.y - 7, 14, 3), 0, GCornerNone);
    graphics_fill_rect(ctx, GRect(c.x - 5, c.y - 4, 10, 11), 0, GCornerNone);
}

// Checkmark — confirm / accept affordance.
static void draw_icon_checkmark(GContext *ctx, GPoint c, GColor color) {
    graphics_context_set_stroke_color(ctx, color);
    graphics_context_set_stroke_width(ctx, 2);
    graphics_draw_line(ctx, GPoint(c.x - 6, c.y), GPoint(c.x - 2, c.y + 5));
    graphics_draw_line(ctx, GPoint(c.x - 2, c.y + 5), GPoint(c.x + 7, c.y - 5));
}

// --- Per-destination vector glyphs (drawn in fg color, centered at c) ---

// Sparkle / 4-point star — AI route
static void draw_glyph_sparkle(GContext *ctx, GPoint c, GColor fg) {
    c.y -= 1;   // optically sits a touch low otherwise
    graphics_context_set_stroke_color(ctx, fg);
    graphics_context_set_stroke_width(ctx, 2);
    graphics_draw_line(ctx, GPoint(c.x, c.y - 5), GPoint(c.x, c.y + 5));
    graphics_draw_line(ctx, GPoint(c.x - 5, c.y), GPoint(c.x + 5, c.y));
    graphics_context_set_stroke_width(ctx, 1);
    graphics_draw_line(ctx, GPoint(c.x - 3, c.y - 3), GPoint(c.x + 3, c.y + 3));
    graphics_draw_line(ctx, GPoint(c.x - 3, c.y + 3), GPoint(c.x + 3, c.y - 3));
}

// Check mark — Tasks
static void draw_glyph_check(GContext *ctx, GPoint c, GColor fg) {
    graphics_context_set_stroke_color(ctx, fg);
    graphics_context_set_stroke_width(ctx, 2);
    graphics_draw_line(ctx, GPoint(c.x - 5, c.y + 1), GPoint(c.x - 1, c.y + 5));
    graphics_draw_line(ctx, GPoint(c.x - 1, c.y + 5), GPoint(c.x + 6, c.y - 4));
}

// Three stacked lines — local Note
static void draw_glyph_lines(GContext *ctx, GPoint c, GColor fg) {
    c.y -= 1;   // optically sits a touch low otherwise
    graphics_context_set_stroke_color(ctx, fg);
    graphics_context_set_stroke_width(ctx, 1);
    graphics_draw_line(ctx, GPoint(c.x - 5, c.y - 4), GPoint(c.x + 5, c.y - 4));
    graphics_draw_line(ctx, GPoint(c.x - 5, c.y),     GPoint(c.x + 5, c.y));
    graphics_draw_line(ctx, GPoint(c.x - 5, c.y + 4), GPoint(c.x + 1, c.y + 4));
}

// Draw a single letter monogram centered at c, in fg color. The rect top is
// raised (Pebble text has top leading) so the glyph is optically centered.
static void draw_glyph_letter(GContext *ctx, char letter, GPoint c, GColor fg) {
    char ch[2] = { letter, '\0' };
    GFont font = fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD);
    graphics_context_set_text_color(ctx, fg);
    graphics_draw_text(ctx, ch, font,
        GRect(c.x - 9, c.y - 9, 18, 18),
        GTextOverflowModeFill, GTextAlignmentCenter, NULL);
}

// Dispatch: draw the destination's glyph centered at c.
static void draw_dest_glyph(GContext *ctx, int dest, GPoint c, GColor fg) {
    switch (dest) {
        case DEST_AI:        draw_glyph_sparkle(ctx, c, fg); break;
        case DEST_TASKS:     draw_glyph_check(ctx, c, fg);   break;
        case DEST_LOCAL:     draw_glyph_lines(ctx, c, fg);   break;
        case DEST_NOTION:    draw_glyph_letter(ctx, 'N', c, fg); break;
        case DEST_TODOIST:   draw_glyph_letter(ctx, 'T', c, fg); break;
        case DEST_WEBHOOK:   draw_glyph_letter(ctx, 'H', c, fg); break;
        case DEST_NEXTCLOUD: draw_glyph_letter(ctx, 'C', c, fg); break;
        case DEST_NEXTCLOUD_TASKS: draw_glyph_letter(ctx, 'C', c, fg); break;
        case DEST_DISCORD:   draw_glyph_letter(ctx, 'D', c, fg); break;
        case DEST_JOPLIN:    draw_glyph_letter(ctx, 'J', c, fg); break;
        case DEST_TIMELINE:  draw_glyph_letter(ctx, 'R', c, fg); break;
        default:             draw_glyph_letter(ctx, '?', c, fg); break;
    }
}

// Bordered rounded-square tile (transparent interior) with the dest glyph,
// both stroked in fg. Matches the handoff's leading-icon / chip style.
static void draw_dest_tile(GContext *ctx, int dest, GPoint center, int size, GColor fg) {
    GRect box = GRect(center.x - size / 2, center.y - size / 2, size, size);
    graphics_context_set_stroke_color(ctx, fg);
    graphics_context_set_stroke_width(ctx, 1);
    graphics_draw_round_rect(ctx, box, 3);
    draw_dest_glyph(ctx, dest, center, fg);
}

// ============================================================================
// CONFIRM WINDOW  (review transcription + target before routing)
// ============================================================================

#define CONFIRM_HINT_W ACTION_BAR_W
#define CONFIRM_BOTTOM_PAD 4
#define CONFIRM_TARGET_H   (app_font_h(AF_HEADER))
#define CONFIRM_DUE_H      (app_font_h(AF_FOOTER))
#define CONFIRM_META_GAP   2

static void confirm_relayout_meta(GRect b, int content_w) {
    if (!s_confirm_target_layer || !s_confirm_content_layer) return;

    bool has_due = s_confirm_due_buf[0] != '\0';
    int due_y = b.size.h - CONFIRM_BOTTOM_PAD - CONFIRM_DUE_H;
    int target_y = has_due
        ? (due_y - CONFIRM_META_GAP - CONFIRM_TARGET_H)
        : (b.size.h - CONFIRM_BOTTOM_PAD - CONFIRM_TARGET_H);
    int content_bottom = target_y - 4;

    layer_set_frame(text_layer_get_layer(s_confirm_target_layer),
                    GRect(4, target_y, content_w, CONFIRM_TARGET_H));
    layer_set_frame(text_layer_get_layer(s_confirm_due_layer),
                    GRect(4, due_y, content_w, CONFIRM_DUE_H));
    layer_set_hidden(text_layer_get_layer(s_confirm_due_layer), !has_due);
    layer_set_frame(text_layer_get_layer(s_confirm_content_layer),
                    GRect(4, STATUS_H + 4, content_w, content_bottom - (STATUS_H + 4)));
}

static void confirm_canvas_update(Layer *layer, GContext *ctx) {
    GRect bounds = layer_get_bounds(layer);
    graphics_context_set_fill_color(ctx, C_SCREEN);
    graphics_fill_rect(ctx, bounds, 0, GCornerNone);
    draw_action_bar(ctx, bounds);
#ifndef PBL_ROUND
    draw_status_clock_on_bar(ctx, bounds);
#endif
    int ax = action_bar_icon_x(bounds);
    draw_icon_redo_review(ctx, GPoint(ax, btn_up_y(bounds)), ACTION_ICON_COLOR);
    draw_icon_checkmark(ctx, GPoint(ax, btn_select_y(bounds)), ACTION_ICON_COLOR);
}

// Start dictation. `quick` marks the note the Quick Launch gesture started
// itself: only that note skips the review screen and exits once saved, so a
// second note dictated by hand in the same session behaves normally.
static void start_dictation(bool quick) {
    s_quick_note = quick;
    s_chunk_count = 0;
    s_chunk_aborts = 0;
    s_dict_start_ms = now_ms();
    s_last_speech_ms = s_dict_start_ms;
    rec_log_reset();
    s_continue_mode = CW_KEEP;
    s_note_interrupted = false;
    s_note_buf[0] = '\0';   // a note whose takes all fail must not resend the last one
    dictation_chunk_start();
}

static void confirm_route_cb(void *ctx) {
    route_note(s_confirm_is_followup);
}

static void confirm_redo_cb(void *ctx) {
    s_is_followup = s_confirm_is_followup;
    start_dictation(s_quick_note);   // redoing a quick-launched note keeps its behaviour
}

static void confirm_select_click(ClickRecognizerRef rec, void *ctx) {
    window_stack_pop(true);
    app_timer_register(50, confirm_route_cb, NULL);
}

static void confirm_up_click(ClickRecognizerRef rec, void *ctx) {
    window_stack_pop(true);
    app_timer_register(300, confirm_redo_cb, NULL);
}

static void confirm_click_config(void *ctx) {
    window_single_click_subscribe(BUTTON_ID_SELECT, confirm_select_click);
    window_single_click_subscribe(BUTTON_ID_UP,     confirm_up_click);
}

static void confirm_window_load(Window *window) {
    Layer *root = window_get_root_layer(window);
    GRect b = layer_get_bounds(root);

    window_set_background_color(window, C_SCREEN);

    int content_w = content_area_w(b) - 8;

    s_confirm_canvas_layer = layer_create(b);
    layer_set_update_proc(s_confirm_canvas_layer, confirm_canvas_update);
    layer_add_child(root, s_confirm_canvas_layer);

    s_confirm_title_layer = text_layer_create(
        GRect(STATUS_TITLE_X, 1, STATUS_HDR_TITLE_W(content_area_w(b)), STATUS_H));
    text_layer_set_background_color(s_confirm_title_layer, GColorClear);
    text_layer_set_text_color(s_confirm_title_layer, C_ON_SCREEN);
    text_layer_set_font(s_confirm_title_layer, app_font(AF_STATUS));
    text_layer_set_text(s_confirm_title_layer, "REVIEW");
    layer_add_child(root, text_layer_get_layer(s_confirm_title_layer));

    s_confirm_divider_layer = add_status_divider_layer(root, b.size.w);

    s_confirm_content_layer = text_layer_create(
        GRect(4, STATUS_H + 4, content_w, b.size.h - STATUS_H - 8));
    text_layer_set_background_color(s_confirm_content_layer, GColorClear);
    text_layer_set_text_color(s_confirm_content_layer, C_ON_SCREEN);
    text_layer_set_font(s_confirm_content_layer, app_font(AF_BODY));
    text_layer_set_overflow_mode(s_confirm_content_layer, GTextOverflowModeWordWrap);
    text_layer_set_text(s_confirm_content_layer, s_note_buf);
    layer_add_child(root, text_layer_get_layer(s_confirm_content_layer));

    // Destination ("→ Google Tasks") + any extracted due date/time below it.
    snprintf(s_confirm_target_buf, sizeof(s_confirm_target_buf), "\xe2\x86\x92 ?");
    s_confirm_target_layer = text_layer_create(
        GRect(4, b.size.h - CONFIRM_BOTTOM_PAD - CONFIRM_TARGET_H, content_w, CONFIRM_TARGET_H));
    text_layer_set_background_color(s_confirm_target_layer, GColorClear);
    text_layer_set_text_color(s_confirm_target_layer, C_ON_SCREEN);
    text_layer_set_font(s_confirm_target_layer, app_font(AF_HEADER));
    text_layer_set_text(s_confirm_target_layer, s_confirm_target_buf);
    layer_add_child(root, text_layer_get_layer(s_confirm_target_layer));

    s_confirm_due_buf[0] = '\0';
    s_confirm_due_layer = text_layer_create(
        GRect(4, b.size.h - CONFIRM_BOTTOM_PAD - CONFIRM_DUE_H, content_w, CONFIRM_DUE_H));
    text_layer_set_background_color(s_confirm_due_layer, GColorClear);
    text_layer_set_text_color(s_confirm_due_layer, GColorLightGray);
    text_layer_set_font(s_confirm_due_layer, app_font(AF_FOOTER));
    text_layer_set_text(s_confirm_due_layer, s_confirm_due_buf);
    layer_add_child(root, text_layer_get_layer(s_confirm_due_layer));

    if (!is_phone_connected()) {
        snprintf(s_confirm_target_buf, sizeof(s_confirm_target_buf),
                 "\xe2\x86\x92 %s", dest_full_name(DEST_LOCAL));
        text_layer_set_text(s_confirm_target_layer, s_confirm_target_buf);
        s_confirm_due_buf[0] = '\0';
        text_layer_set_text(s_confirm_due_layer, s_confirm_due_buf);
    }

#ifdef DEBUG_SEED
    snprintf(s_confirm_target_buf, sizeof(s_confirm_target_buf), "\xe2\x86\x92 Google Tasks");
    text_layer_set_text(s_confirm_target_layer, s_confirm_target_buf);
    snprintf(s_confirm_due_buf, sizeof(s_confirm_due_buf), "due Tomorrow 09:00");
    text_layer_set_text(s_confirm_due_layer, s_confirm_due_buf);
#endif

    confirm_relayout_meta(b, content_w);

    window_set_click_config_provider(window, confirm_click_config);
    s_confirm_open = true;
}

static void confirm_window_unload(Window *window) {
    layer_destroy(s_confirm_canvas_layer);     s_confirm_canvas_layer  = NULL;
    layer_destroy(s_confirm_divider_layer);    s_confirm_divider_layer = NULL;
    text_layer_destroy(s_confirm_title_layer); s_confirm_title_layer = NULL;
    text_layer_destroy(s_confirm_content_layer); s_confirm_content_layer = NULL;
    text_layer_destroy(s_confirm_target_layer);  s_confirm_target_layer  = NULL;
    text_layer_destroy(s_confirm_due_layer);     s_confirm_due_layer     = NULL;
    s_confirm_open = false;
}

// ============================================================================
// SUCCESS ("DUMPED") WINDOW — post-dump confirmation, auto-dismiss unless scrolled
// ============================================================================

#define SUCCESS_DISMISS_MS 4000

// Header block: medallion on the left, title ("SENT" / "SENDING") to its
// right with the destination chip under the title. The transcript starts
// right below, so most of the screen is the note itself.
#define SUCCESS_TILE 20
typedef struct {
    int    r;          // medallion radius
    GPoint med;        // medallion center
    int    text_x;     // left edge of the title / chip column
    int    title_y, title_h;
    GFont  title_font;
    int    chip_cy;    // destination chip center line
    int    bottom;     // first free y below the block
} SuccessHeader;

static SuccessHeader success_header(int w) {
    SuccessHeader hd;
    const char *title = s_success_pending ? "SENDING" : "SENT";
    hd.r = w <= 144 ? 16 : 22;
    int gap = 8;
    // The biggest title that fits beside the medallion.
    hd.title_font = app_font(AF_TITLE);
    hd.title_h = app_font_h(AF_TITLE);
    int col_w = w - 2 * hd.r - gap - 16;
    if (graphics_text_layout_get_content_size(title, hd.title_font, GRect(0, 0, w, hd.title_h + 6),
            GTextOverflowModeFill, GTextAlignmentLeft).w > col_w) {
        hd.title_font = app_font(AF_HEADER);
        hd.title_h = app_font_h(AF_HEADER);
    }
    int col_h = hd.title_h + 4 + SUCCESS_TILE;
    int block_h = col_h > 2 * hd.r + 4 ? col_h : 2 * hd.r + 4;
#ifdef PBL_ROUND
    // Centered as a group; dropped a little so the corners clear the bezel.
    int top = STATUS_H + 18;
    int x0 = (w - (2 * hd.r + gap + col_w * 3 / 4)) / 2;
#else
    int top = STATUS_H + 8;
    int x0 = 8;
#endif
    hd.med = GPoint(x0 + hd.r, top + block_h / 2);
    hd.text_x = x0 + 2 * hd.r + gap;
    int col_top = top + (block_h - col_h) / 2;
    // Nudge up: Pebble text renders with top leading.
    hd.title_y = col_top - hd.title_h / 6;
    hd.chip_cy = col_top + hd.title_h + 4 + SUCCESS_TILE / 2;
    hd.bottom = top + block_h + 6;
    return hd;
}

// Larger check mark for the medallion (scaled by radius r around center c).
static void draw_check_big(GContext *ctx, GPoint c, int r, GColor fg) {
    graphics_context_set_stroke_color(ctx, fg);
    graphics_context_set_stroke_width(ctx, 3);
    int x0 = c.x - r * 45 / 100, y0 = c.y + r * 5 / 100;
    int x1 = c.x - r * 8 / 100,  y1 = c.y + r * 42 / 100;
    int x2 = c.x + r * 48 / 100, y2 = c.y - r * 40 / 100;
    graphics_draw_line(ctx, GPoint(x0, y0), GPoint(x1, y1));
    graphics_draw_line(ctx, GPoint(x1, y1), GPoint(x2, y2));
}

static void success_canvas_update(Layer *layer, GContext *ctx) {
    GRect bounds = layer_get_bounds(layer);
    int w = bounds.size.w;

    const char *header = s_success_pending ? "SENDING" : "SAVED";

    graphics_context_set_fill_color(ctx, C_SCREEN);
    graphics_fill_rect(ctx, bounds, 0, GCornerNone);

    // Status bar: round centers the title alone; rect shows title + clock.
    graphics_context_set_text_color(ctx, GColorLightGray);
#ifdef PBL_ROUND
    graphics_draw_text(ctx, header, app_font(AF_STATUS),
        GRect(0, 1, w, STATUS_H), GTextOverflowModeFill, GTextAlignmentCenter, NULL);
#else
    graphics_draw_text(ctx, header, app_font(AF_STATUS),
        GRect(STATUS_TITLE_X, 1, STATUS_HDR_TITLE_W(w), STATUS_H),
        GTextOverflowModeFill, GTextAlignmentLeft, NULL);
    graphics_context_set_text_color(ctx, C_ON_SCREEN);
    refresh_clock_buf();
    graphics_draw_text(ctx, s_clock_buf, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
        GRect(STATUS_CLOCK_X(w), 1, STATUS_CLOCK_W, STATUS_H),
        GTextOverflowModeFill, GTextAlignmentRight, NULL);
    draw_status_divider(ctx, bounds);
#endif

    SuccessHeader hd = success_header(w);

    // Medallion: ring + check (ring + three dots while still sending)
    graphics_context_set_stroke_color(ctx, C_ON_SCREEN);
    graphics_context_set_stroke_width(ctx, 2);
    graphics_draw_circle(ctx, hd.med, hd.r);
    if (s_success_pending) {
        graphics_context_set_fill_color(ctx, C_ON_SCREEN);
        for (int i = -1; i <= 1; i++) {
            graphics_fill_circle(ctx, GPoint(hd.med.x + i * hd.r * 45 / 100, hd.med.y), 3);
        }
    } else {
        draw_check_big(ctx, GPoint(hd.med.x, hd.med.y + 2), hd.r, C_ON_SCREEN);
    }

    // Title "SENT" / "SENDING", left-aligned beside the medallion
    int col_w = w - hd.text_x - 4;
    graphics_context_set_text_color(ctx, C_ON_SCREEN);
    graphics_draw_text(ctx, s_success_pending ? "SENDING" : "SENT", hd.title_font,
        GRect(hd.text_x, hd.title_y, col_w, hd.title_h + 6),
        GTextOverflowModeFill, GTextAlignmentLeft, NULL);

    // Destination chip under the title: bordered tile + name. Not drawn until
    // the phone has said where the note is going.
    if (s_success_dest < 0) return;
    const char *name = dest_full_name(s_success_dest);
    GFont chip_font = app_font(AF_STATUS);
    int chip_h = app_font_h(AF_STATUS);
    int gap = 6;
    draw_dest_tile(ctx, s_success_dest, GPoint(hd.text_x + SUCCESS_TILE / 2, hd.chip_cy),
                   SUCCESS_TILE, C_ON_SCREEN);
    graphics_context_set_text_color(ctx, C_ON_SCREEN);
    // Nudge up: a rect-centered line sits visually low against the tile.
    graphics_draw_text(ctx, name, chip_font,
        GRect(hd.text_x + SUCCESS_TILE + gap, hd.chip_cy - chip_h / 2 - chip_h / 10,
              col_w - SUCCESS_TILE - gap, chip_h + 4),
        GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
    // The webhook reply and the full transcript are text layers below this,
    // in the same scroll layer (success_relayout).
}

static void success_close(void) {
    if (s_success_exit_on_close) { quick_exit_now(); return; }
    window_stack_pop(true);
    set_status("Ready");   // restore the READY idle screen on home
}

static void success_dismiss_cb(void *ctx) {
    s_success_timer = NULL;
    if (s_success_window && window_stack_get_top_window() == s_success_window) {
        success_close();
    }
}

static void success_dismiss_click(ClickRecognizerRef rec, void *ctx) {
    if (s_success_timer) { app_timer_cancel(s_success_timer); s_success_timer = NULL; }
    success_close();
}

static void success_click_config(void *ctx) {
    window_single_click_subscribe(BUTTON_ID_SELECT, success_dismiss_click);
    window_single_click_subscribe(BUTTON_ID_BACK,   success_dismiss_click);
}

// Scrolling means the user is reading: stop the auto-dismiss for good, even
// across the SENDING → DUMPED flip.
static void success_scrolled(ScrollLayer *scroll_layer, void *ctx) {
    s_success_scrolled = true;
    if (s_success_timer) { app_timer_cancel(s_success_timer); s_success_timer = NULL; }
}

// Lay out the reply + transcript under the chip and size the scroll content.
// Auto-dismiss once the send is confirmed, unless the user has scrolled.
static void success_relayout(void) {
    if (!s_success_scroll_layer) return;
    GRect b = layer_get_frame(scroll_layer_get_layer(s_success_scroll_layer));
    int w = b.size.w, h = b.size.h;
    int y = success_header(w).bottom;
    int tw = w - 16;

    // Two-way webhook reply (if any) first — the endpoint talked back.
    bool has_reply = (!s_success_pending && s_success_dest == DEST_WEBHOOK && s_webhook_msg_buf[0]);
    text_layer_set_text(s_success_reply_layer, has_reply ? s_webhook_msg_buf : "");
    int reply_h = 0;
    if (has_reply) {
        layer_set_frame(text_layer_get_layer(s_success_reply_layer), GRect(8, y, tw, 2000));
        reply_h = text_layer_get_content_size(s_success_reply_layer).h + 4;
    }
    layer_set_frame(text_layer_get_layer(s_success_reply_layer), GRect(8, y, tw, reply_h));
    y += reply_h;

    snprintf(s_success_note_text, sizeof(s_success_note_text), "\"%s\"", s_note_buf);
    text_layer_set_text(s_success_note_layer, s_success_note_text);
    layer_set_frame(text_layer_get_layer(s_success_note_layer), GRect(8, y, tw, 2000));
    int note_h = text_layer_get_content_size(s_success_note_layer).h + 4;
    layer_set_frame(text_layer_get_layer(s_success_note_layer), GRect(8, y, tw, note_h));

    int content_h = y + note_h + 8;
    if (content_h < h) content_h = h;
    scroll_layer_set_content_size(s_success_scroll_layer, GSize(w, content_h));
    if (s_success_canvas_layer) layer_mark_dirty(s_success_canvas_layer);

    if (s_success_timer) { app_timer_cancel(s_success_timer); s_success_timer = NULL; }
    if (!s_success_pending && !s_success_scrolled) {
        s_success_timer = app_timer_register(SUCCESS_DISMISS_MS, success_dismiss_cb, NULL);
    }
}

static TextLayer *success_text_layer_create(GColor color) {
    TextLayer *t = text_layer_create(GRect(0, 0, 0, 0));
    text_layer_set_background_color(t, GColorClear);
    text_layer_set_text_color(t, color);
    text_layer_set_font(t, app_font(AF_BODY));
    text_layer_set_text_alignment(t, GTextAlignmentCenter);
    text_layer_set_overflow_mode(t, GTextOverflowModeWordWrap);
    return t;
}

static void success_window_load(Window *window) {
    Layer *root = window_get_root_layer(window);
    GRect b = layer_get_bounds(root);
    window_set_background_color(window, C_SCREEN);

    s_success_scroll_layer = scroll_layer_create(b);
    scroll_layer_set_shadow_hidden(s_success_scroll_layer, true);
    scroll_layer_set_callbacks(s_success_scroll_layer, (ScrollLayerCallbacks) {
        .click_config_provider = success_click_config,
        .content_offset_changed_handler = success_scrolled,
    });
    scroll_layer_set_click_config_onto_window(s_success_scroll_layer, window);
    layer_add_child(root, scroll_layer_get_layer(s_success_scroll_layer));

    // The canvas keeps its original one-screen geometry; text scrolls below it.
    s_success_canvas_layer = layer_create(b);
    layer_set_update_proc(s_success_canvas_layer, success_canvas_update);
    scroll_layer_add_child(s_success_scroll_layer, s_success_canvas_layer);

    s_success_reply_layer = success_text_layer_create(C_ON_SCREEN);
    scroll_layer_add_child(s_success_scroll_layer, text_layer_get_layer(s_success_reply_layer));
    s_success_note_layer = success_text_layer_create(GColorLightGray);
    scroll_layer_add_child(s_success_scroll_layer, text_layer_get_layer(s_success_note_layer));

    success_relayout();
}

static void success_window_unload(Window *window) {
    if (s_success_timer) { app_timer_cancel(s_success_timer); s_success_timer = NULL; }
    // Dismissed while still sending: the result, when it comes, gets a fresh window.
    s_success_pending = false;
    s_success_exit_on_close = false;
    text_layer_destroy(s_success_note_layer);   s_success_note_layer  = NULL;
    text_layer_destroy(s_success_reply_layer);  s_success_reply_layer = NULL;
    layer_destroy(s_success_canvas_layer);      s_success_canvas_layer = NULL;
    scroll_layer_destroy(s_success_scroll_layer); s_success_scroll_layer = NULL;
}

// ============================================================================
// SEND-FAILED FALLBACK — a cloud send failed; offer two ways to keep the note
// so it isn't lost. UP asks the phone to retry it later (queued phone-side);
// SELECT saves it as an on-watch local reminder; BACK discards. The two are
// mutually exclusive, so the note is never both queued and saved locally.
// ============================================================================

static void fallback_canvas_update(Layer *layer, GContext *ctx) {
    GRect b = layer_get_bounds(layer);
    graphics_context_set_fill_color(ctx, C_SCREEN);
    graphics_fill_rect(ctx, b, 0, GCornerNone);
    int content_w = content_area_w(b) - 8;

    // Status bar — title + clock + divider, same chrome as the review screen.
    graphics_context_set_text_color(ctx, C_ON_SCREEN);
    graphics_draw_text(ctx, "FAILED", app_font(AF_STATUS),
        GRect(STATUS_TITLE_X, 1, STATUS_HDR_TITLE_W(content_area_w(b)), STATUS_H),
        GTextOverflowModeFill, GTextAlignmentLeft, NULL);
#ifndef PBL_ROUND
    draw_status_clock_on_bar(ctx, b);
#endif
    draw_status_divider(ctx, b);

    // Two stacked choice hints at the bottom, in button order (UP then SELECT),
    // mirroring the two action-bar icons. BACK discards and needs no label.
    int hint_h = app_font_h(AF_FOOTER);
    int hint2_y = b.size.h - CONFIRM_BOTTOM_PAD - hint_h;
    int hint1_y = hint2_y - hint_h;

    // Error message, prefixed. HEADER size — bigger than a caption but small
    // enough that a typical error fits without being cut.
    char disp[STATUS_BUF_SIZE + 12];
    snprintf(disp, sizeof(disp), "Error: %s", s_fallback_err);
    int content_top = STATUS_H + 4;
    graphics_context_set_text_color(ctx, C_ON_SCREEN);
    graphics_draw_text(ctx, disp, app_font(AF_HEADER),
        GRect(4, content_top, content_w, hint1_y - 6 - content_top),
        GTextOverflowModeWordWrap, GTextAlignmentLeft, NULL);

    graphics_context_set_text_color(ctx, C_ON_SCREEN);
    graphics_draw_text(ctx, "Retry later", app_font(AF_FOOTER),
        GRect(4, hint1_y, content_w, hint_h + 2),
        GTextOverflowModeFill, GTextAlignmentLeft, NULL);
    graphics_draw_text(ctx, "Save local", app_font(AF_FOOTER),
        GRect(4, hint2_y, content_w, hint_h + 2),
        GTextOverflowModeFill, GTextAlignmentLeft, NULL);

    // Action bar — redo = retry later (UP), local-note glyph = save (SELECT);
    // back dismisses. Redo icon reused from the dictation confirm screen.
    draw_action_bar(ctx, b);
    int ax = action_bar_icon_x(b);
    draw_icon_redo_review(ctx, GPoint(ax, btn_up_y(b)),     ACTION_ICON_COLOR);
    draw_glyph_lines     (ctx, GPoint(ax, btn_select_y(b)), ACTION_ICON_COLOR);
}

static void fallback_retry_click(ClickRecognizerRef rec, void *ctx) {
    window_stack_pop(true);
    appmsg_queue_retry();
    set_status("Queued for retry");
}

static void fallback_save_click(ClickRecognizerRef rec, void *ctx) {
    window_stack_pop(true);
    reminders_add(s_note_buf);
    set_status("Saved locally ✓");
    success_window_push(DEST_LOCAL);
}

static void fallback_dismiss_click(ClickRecognizerRef rec, void *ctx) {
    window_stack_pop(true);
    set_status(s_fallback_err);
}

static void fallback_click_config(void *ctx) {
    window_single_click_subscribe(BUTTON_ID_UP,     fallback_retry_click);
    window_single_click_subscribe(BUTTON_ID_SELECT, fallback_save_click);
    window_single_click_subscribe(BUTTON_ID_BACK,   fallback_dismiss_click);
}

static void fallback_window_load(Window *window) {
    Layer *root = window_get_root_layer(window);
    window_set_background_color(window, C_SCREEN);
    s_fallback_canvas = layer_create(layer_get_bounds(root));
    layer_set_update_proc(s_fallback_canvas, fallback_canvas_update);
    layer_add_child(root, s_fallback_canvas);
    window_set_click_config_provider(window, fallback_click_config);
}

static void fallback_window_unload(Window *window) {
    if (s_fallback_canvas) { layer_destroy(s_fallback_canvas); s_fallback_canvas = NULL; }
}

static void fallback_window_push(const char *err) {
    strncpy(s_fallback_err, (err && err[0]) ? err : "Send failed", sizeof(s_fallback_err) - 1);
    s_fallback_err[sizeof(s_fallback_err) - 1] = '\0';
    if (s_fallback_window) { window_destroy(s_fallback_window); s_fallback_window = NULL; }
    s_fallback_window = window_create();
    window_set_window_handlers(s_fallback_window, (WindowHandlers) {
        .load   = fallback_window_load,
        .unload = fallback_window_unload,
    });
    window_stack_push(s_fallback_window, true);
}

// ============================================================================
// HOME WINDOW
// ============================================================================

static void connection_handler(bool connected) {
    (void)connected;
    refresh_home_idle_ui();
    if (s_canvas_layer) layer_mark_dirty(s_canvas_layer);
}

static void canvas_update_proc(Layer *layer, GContext *ctx) {
    GRect bounds = layer_get_bounds(layer);

    graphics_context_set_fill_color(ctx, C_SCREEN);
    graphics_fill_rect(ctx, bounds, 0, GCornerNone);
    draw_action_bar(ctx, bounds);
    draw_status_divider(ctx, bounds);

    // Round centers across the full width (the clipped strip leaves room and
    // the text below is also full-width centered); rect centers in the content
    // area left of the strip.
#ifdef PBL_ROUND
    int cx = bounds.size.w / 2;
#else
    int cx = content_area_w(bounds) / 2;
#endif
    int cy = bounds.size.h * 44 / 100;
    if (s_brain_bmp) {
        GRect r = gbitmap_get_bounds(s_brain_bmp);
        graphics_context_set_compositing_mode(ctx, GCompOpSet);
        graphics_draw_bitmap_in_rect(ctx, s_brain_bmp,
            GRect(cx - r.size.w / 2, cy - r.size.h / 2, r.size.w, r.size.h));
    }

    int ax = action_bar_icon_x(bounds);
    draw_icon_hamburger(ctx, GPoint(ax, btn_up_y(bounds)),     ACTION_ICON_COLOR);
    draw_icon_record(ctx,    GPoint(ax, btn_select_y(bounds)), ACTION_ICON_COLOR);
}

static void home_select_click(ClickRecognizerRef rec, void *ctx) {
    start_dictation(false);
}

static void home_up_click(ClickRecognizerRef rec, void *ctx) {
    history_window_push();
}

#ifdef DEBUG_SEED
// Screen gallery — DOWN on home cycles through every screen (incl. the error
// prompt) for visual verification. Order is announced by the on-screen content.
#define DEBUG_GALLERY_N 10
static int s_gallery_idx = 0;

static void debug_gallery_show(int i) {
    // Pushes on top of home; the caller returns to home (BACK) between steps.
    strncpy(s_note_buf, "Buy oat milk tomorrow at 9am", NOTE_BUF_SIZE - 1);
    s_note_buf[NOTE_BUF_SIZE - 1] = '\0';
    switch (i) {
        case 0: set_status("Waiting for answer from the assistant to your question"); break;
        case 1: confirm_window_push(); break;                 // review / confirm
        case 2: success_window_push(DEST_NOTION); break;      // success — letter tile
        case 3: success_window_push(DEST_AI); break;          // success — sparkle tile
        case 4: success_window_push(DEST_LOCAL); break;       // success — lines tile
        case 5: fallback_window_push("Nextcloud Tasks: list not found on the server"); break;
        case 6:                                               // AI discussion (scrollable)
            conv_start("How should I structure a Q3 planning deck for the exec team?");
            conv_set_response("A clean structure:\n\n"
                "1. Executive summary — the ask up front.\n"
                "2. Where we are — metrics vs targets.\n"
                "3. What we learned last quarter.\n"
                "4. The plan — 3 bets, each with owner + timeline.\n"
                "5. Resourcing and risks.\n"
                "6. The ask — decision needed today.\n\n"
                "Keep it to 8 slides; detail goes in an appendix.");
            s_in_ai_thread = true;
            response_window_push();
            break;
        case 7: history_window_push(); break;                 // history (populated)
        case 8: detail_window_push(0); break;                 // history detail
        case 9: rem_detail_window_push(0); break;             // reminder detail
    }
}

static void home_debug_success_click(ClickRecognizerRef rec, void *ctx) {
    debug_gallery_show(s_gallery_idx);
    s_gallery_idx = (s_gallery_idx + 1) % DEBUG_GALLERY_N;
}
#endif

static void home_click_config(void *ctx) {
    window_single_click_subscribe(BUTTON_ID_SELECT, home_select_click);
    window_single_click_subscribe(BUTTON_ID_UP,     home_up_click);
#ifdef DEBUG_SEED
    window_single_click_subscribe(BUTTON_ID_DOWN,   home_debug_success_click);
#endif
}

#ifndef PBL_ROUND
static void home_clock_tick(struct tm *tick_time, TimeUnits changed) {
    if (!s_home_clock_layer) return;
    if (clock_is_24h_style()) {
        snprintf(s_clock_buf, sizeof(s_clock_buf), "%02d:%02d",
                 tick_time->tm_hour, tick_time->tm_min);
    } else {
        int h = tick_time->tm_hour % 12;
        if (h == 0) h = 12;
        snprintf(s_clock_buf, sizeof(s_clock_buf), "%d:%02d", h, tick_time->tm_min);
    }
    text_layer_set_text(s_home_clock_layer, s_clock_buf);
}
#endif

static void home_window_load(Window *window) {
    Layer *root = window_get_root_layer(window);
    GRect b = layer_get_bounds(root);
    int content_w = content_area_w(b);
    (void)content_w;   // only used in the rectangular status-bar layout below

    window_set_background_color(window, C_SCREEN);

    s_canvas_layer = layer_create(b);
    layer_set_update_proc(s_canvas_layer, canvas_update_proc);
    layer_add_child(root, s_canvas_layer);

    // Status bar. Round: centered title, no clock. Rect: left title + right clock.
#ifdef PBL_ROUND
    s_home_title_layer = text_layer_create(GRect(0, 1, b.size.w, STATUS_H));
    text_layer_set_text_alignment(s_home_title_layer, GTextAlignmentCenter);
#else
    s_home_title_layer = text_layer_create(GRect(STATUS_TITLE_X, 1, STATUS_HDR_TITLE_W(content_w), STATUS_H));
#endif
    text_layer_set_background_color(s_home_title_layer, GColorClear);
    text_layer_set_text_color(s_home_title_layer, GColorLightGray);
    text_layer_set_font(s_home_title_layer, app_font(AF_STATUS));
    text_layer_set_text(s_home_title_layer, "DUMP");
    layer_add_child(root, text_layer_get_layer(s_home_title_layer));

#ifndef PBL_ROUND
    add_home_clock_layer(root, b);
    tick_timer_service_subscribe(MINUTE_UNIT, home_clock_tick);
    time_t now = time(NULL);
    struct tm *tick_time = localtime(&now);
    home_clock_tick(tick_time, MINUTE_UNIT);
#endif

    int hero_y = b.size.h * 58 / 100;
#ifdef PBL_ROUND
    int text_w = b.size.w;   // center across full width on round
#else
    int text_w = content_w;
#endif
    // Two lines tall so long status messages ("Waiting for answer…") wrap
    // instead of truncating. READY still sits on the first line; meta tucks
    // right under it (status messages clear the meta line, which is then empty).
    int hero_line = app_font_h(AF_TITLE);
    int hero_h = hero_line * 2 + 4;
    s_hero_layer = text_layer_create(GRect(0, hero_y, text_w, hero_h));
    text_layer_set_background_color(s_hero_layer, GColorClear);
    text_layer_set_text_color(s_hero_layer, C_ON_SCREEN);
    text_layer_set_font(s_hero_layer, app_font(AF_TITLE));
    text_layer_set_text_alignment(s_hero_layer, GTextAlignmentCenter);
    text_layer_set_overflow_mode(s_hero_layer, GTextOverflowModeWordWrap);
    text_layer_set_text(s_hero_layer, "READY");
    layer_add_child(root, text_layer_get_layer(s_hero_layer));

    s_meta_layer = text_layer_create(GRect(0, hero_y + hero_line + 2, text_w, app_font_h(AF_FOOTER) * 2 + 4));
    text_layer_set_background_color(s_meta_layer, GColorClear);
    text_layer_set_text_color(s_meta_layer, GColorLightGray);
    text_layer_set_font(s_meta_layer, app_font(AF_FOOTER));
    text_layer_set_text_alignment(s_meta_layer, GTextAlignmentCenter);
    text_layer_set_text(s_meta_layer, "SELECT saves\nBACK cancels");
    layer_add_child(root, text_layer_get_layer(s_meta_layer));

    window_set_click_config_provider(window, home_click_config);

    strncpy(s_status_buf, "Ready", STATUS_BUF_SIZE - 1);
    refresh_home_idle_ui();
}

static void home_window_unload(Window *window) {
    layer_destroy(s_canvas_layer);              s_canvas_layer       = NULL;
    text_layer_destroy(s_home_title_layer);     s_home_title_layer   = NULL;
    if (s_home_clock_layer) {  // not created on round
        text_layer_destroy(s_home_clock_layer); s_home_clock_layer   = NULL;
    }
    text_layer_destroy(s_hero_layer);           s_hero_layer         = NULL;
    text_layer_destroy(s_meta_layer);           s_meta_layer         = NULL;
}

// ============================================================================
// RESPONSE WINDOW  (AI replies)
// ============================================================================

#define RESP_SCROLL_STEP 36

static void resp_up_click(ClickRecognizerRef rec, void *ctx) {
    s_resp_scroll_offset -= RESP_SCROLL_STEP;
    if (s_resp_scroll_offset < 0) s_resp_scroll_offset = 0;
    scroll_layer_set_content_offset(s_resp_scroll_layer,
        GPoint(0, -s_resp_scroll_offset), true);
}

static void resp_down_click(ClickRecognizerRef rec, void *ctx) {
    GSize cs = scroll_layer_get_content_size(s_resp_scroll_layer);
    GRect fr = layer_get_frame(scroll_layer_get_layer(s_resp_scroll_layer));
    int16_t max_scroll = cs.h - fr.size.h;
    if (max_scroll < 0) max_scroll = 0;
    s_resp_scroll_offset += RESP_SCROLL_STEP;
    if (s_resp_scroll_offset > max_scroll) s_resp_scroll_offset = max_scroll;
    scroll_layer_set_content_offset(s_resp_scroll_layer,
        GPoint(0, -s_resp_scroll_offset), true);
}

static void resp_select_click(ClickRecognizerRef rec, void *ctx) {
    if (!is_phone_connected()) {
        set_status("Connect phone for AI");
        return;
    }
    s_is_followup = true;
    start_dictation(false);
}

static void resp_back_click(ClickRecognizerRef rec, void *ctx) {
    s_is_followup  = false;
    s_in_ai_thread = false;
    appmsg_clear_context();
    s_response_open = false;
    window_stack_pop(true);
}

static void resp_click_config(void *ctx) {
    window_single_click_subscribe(BUTTON_ID_UP,     resp_up_click);
    window_single_click_subscribe(BUTTON_ID_DOWN,   resp_down_click);
    window_single_click_subscribe(BUTTON_ID_SELECT, resp_select_click);
    window_single_click_subscribe(BUTTON_ID_BACK,   resp_back_click);
}

static Layer *s_resp_canvas_layer;

static void resp_canvas_update(Layer *layer, GContext *ctx) {
    GRect bounds = layer_get_bounds(layer);
    graphics_context_set_fill_color(ctx, C_SCREEN);
    graphics_fill_rect(ctx, bounds, 0, GCornerNone);
    draw_action_bar(ctx, bounds);
#ifndef PBL_ROUND
    draw_status_clock_on_bar(ctx, bounds);
    draw_status_divider(ctx, bounds);
#endif
    draw_icon_reply(ctx, GPoint(action_bar_icon_x(bounds), btn_select_y(bounds)), ACTION_ICON_COLOR);
}

static void response_window_load(Window *window) {
    Layer *root = window_get_root_layer(window);
    GRect b = layer_get_bounds(root);
    int content_w = content_area_w(b);

    window_set_background_color(window, C_SCREEN);

    s_resp_canvas_layer = layer_create(b);
    layer_set_update_proc(s_resp_canvas_layer, resp_canvas_update);
    layer_add_child(root, s_resp_canvas_layer);

    s_resp_header_layer = text_layer_create(
        GRect(STATUS_TITLE_X, 1, STATUS_HDR_TITLE_W(content_w), STATUS_H));
    text_layer_set_background_color(s_resp_header_layer, GColorClear);
    text_layer_set_text_color(s_resp_header_layer, C_ON_SCREEN);
    text_layer_set_font(s_resp_header_layer, app_font(AF_STATUS));
    text_layer_set_overflow_mode(s_resp_header_layer, GTextOverflowModeTrailingEllipsis);
    text_layer_set_text(s_resp_header_layer, "AI");
    layer_add_child(root, text_layer_get_layer(s_resp_header_layer));

    int scroll_top  = STATUS_H + 4;
    int scroll_h    = b.size.h - scroll_top;
    GRect scroll_frame = GRect(0, scroll_top, content_w, scroll_h);

    s_resp_scroll_offset = 0;
    s_resp_scroll_layer = scroll_layer_create(scroll_frame);
    layer_add_child(root, scroll_layer_get_layer(s_resp_scroll_layer));

    int content_w_inner = content_w - 8;
    s_resp_content_layer = text_layer_create(GRect(4, 4, content_w_inner, 2000));
    text_layer_set_background_color(s_resp_content_layer, GColorClear);
    text_layer_set_text_color(s_resp_content_layer, C_ON_SCREEN);
    text_layer_set_font(s_resp_content_layer, app_font(AF_BODY));
    text_layer_set_overflow_mode(s_resp_content_layer, GTextOverflowModeWordWrap);
    text_layer_set_text(s_resp_content_layer, s_conversation_buf);
    scroll_layer_add_child(s_resp_scroll_layer,
                           text_layer_get_layer(s_resp_content_layer));

    // Set content size to fit text
    GSize text_size = text_layer_get_content_size(s_resp_content_layer);
    scroll_layer_set_content_size(s_resp_scroll_layer,
        GSize(content_w_inner, text_size.h + 24));

    window_set_click_config_provider(window, resp_click_config);
    s_response_open = true;
}

static void response_window_unload(Window *window) {
    layer_destroy(s_resp_canvas_layer);       s_resp_canvas_layer  = NULL;
    text_layer_destroy(s_resp_header_layer);  s_resp_header_layer  = NULL;
    text_layer_destroy(s_resp_content_layer); s_resp_content_layer = NULL;
    scroll_layer_destroy(s_resp_scroll_layer); s_resp_scroll_layer = NULL;
    s_response_open = false;
}

// ============================================================================
// DETAIL WINDOW  (full history transcript)
// ============================================================================

static void detail_up_click(ClickRecognizerRef rec, void *ctx) {
    s_detail_scroll_offset -= RESP_SCROLL_STEP;
    if (s_detail_scroll_offset < 0) s_detail_scroll_offset = 0;
    scroll_layer_set_content_offset(s_detail_scroll_layer,
        GPoint(0, -s_detail_scroll_offset), true);
}

static void detail_down_click(ClickRecognizerRef rec, void *ctx) {
    GSize cs = scroll_layer_get_content_size(s_detail_scroll_layer);
    GRect fr = layer_get_frame(scroll_layer_get_layer(s_detail_scroll_layer));
    int16_t max_scroll = cs.h - fr.size.h;
    if (max_scroll < 0) max_scroll = 0;
    s_detail_scroll_offset += RESP_SCROLL_STEP;
    if (s_detail_scroll_offset > max_scroll) s_detail_scroll_offset = max_scroll;
    scroll_layer_set_content_offset(s_detail_scroll_layer,
        GPoint(0, -s_detail_scroll_offset), true);
}

static void detail_select_click(ClickRecognizerRef rec, void *ctx) {
    if (!s_detail_confirm) {
        s_detail_confirm = true;
        strncpy(s_detail_header_buf, "DELETE?", sizeof(s_detail_header_buf) - 1);
        s_detail_header_buf[sizeof(s_detail_header_buf) - 1] = '\0';
        text_layer_set_text(s_detail_header_layer, s_detail_header_buf);
        layer_mark_dirty(s_detail_canvas_layer);
    } else {
        history_delete(s_detail_idx);
        window_stack_pop(true);
    }
}

static void detail_click_config(void *ctx) {
    window_single_click_subscribe(BUTTON_ID_UP,     detail_up_click);
    window_single_click_subscribe(BUTTON_ID_DOWN,   detail_down_click);
    window_single_click_subscribe(BUTTON_ID_SELECT, detail_select_click);
}

static void detail_canvas_update(Layer *layer, GContext *ctx) {
    GRect bounds = layer_get_bounds(layer);
    graphics_context_set_fill_color(ctx, C_SCREEN);
    graphics_fill_rect(ctx, bounds, 0, GCornerNone);
    draw_action_bar(ctx, bounds);
#ifndef PBL_ROUND
    draw_status_clock_on_bar(ctx, bounds);
    draw_status_divider(ctx, bounds);
#endif
    int ax = action_bar_icon_x(bounds);
    if (s_detail_confirm) {
        draw_icon_checkmark(ctx, GPoint(ax, btn_select_y(bounds)), ACTION_ICON_COLOR);
    } else {
        draw_icon_trash(ctx, GPoint(ax, btn_select_y(bounds)), ACTION_ICON_COLOR);
    }
}

static void detail_window_load(Window *window) {
    Layer *root = window_get_root_layer(window);
    GRect b = layer_get_bounds(root);
    int content_w = content_area_w(b);

    window_set_background_color(window, C_SCREEN);

    s_detail_canvas_layer = layer_create(b);
    layer_set_update_proc(s_detail_canvas_layer, detail_canvas_update);
    layer_add_child(root, s_detail_canvas_layer);

    strncpy(s_detail_header_buf, s_history_short[s_detail_idx], sizeof(s_detail_header_buf) - 1);
    s_detail_header_buf[sizeof(s_detail_header_buf) - 1] = '\0';

#ifdef PBL_ROUND
    int hdr_w = content_w - 8;
#else
    int hdr_w = STATUS_HDR_TITLE_W(content_w);
#endif
    s_detail_header_layer = text_layer_create(GRect(STATUS_TITLE_X, 1, hdr_w, STATUS_H));
    text_layer_set_background_color(s_detail_header_layer, GColorClear);
    text_layer_set_text_color(s_detail_header_layer, C_ON_SCREEN);
    text_layer_set_font(s_detail_header_layer, app_font(AF_STATUS));
    text_layer_set_overflow_mode(s_detail_header_layer, GTextOverflowModeTrailingEllipsis);
    text_layer_set_text(s_detail_header_layer, s_detail_header_buf);
    layer_add_child(root, text_layer_get_layer(s_detail_header_layer));

    s_detail_scroll_offset = 0;
    GRect scroll_frame = GRect(0, STATUS_H + 2, content_w, b.size.h - STATUS_H - 2);
    s_detail_scroll_layer = scroll_layer_create(scroll_frame);
    layer_add_child(root, scroll_layer_get_layer(s_detail_scroll_layer));

    int inner_w = content_w - 8;
    s_detail_content_layer = text_layer_create(GRect(4, 4, inner_w, 2000));
    text_layer_set_background_color(s_detail_content_layer, GColorClear);
    text_layer_set_text_color(s_detail_content_layer, C_ON_SCREEN);
    text_layer_set_font(s_detail_content_layer, app_font(AF_BODY));
    text_layer_set_overflow_mode(s_detail_content_layer, GTextOverflowModeWordWrap);
    text_layer_set_text(s_detail_content_layer, s_history_full[s_detail_idx]);
    scroll_layer_add_child(s_detail_scroll_layer,
                           text_layer_get_layer(s_detail_content_layer));

    GSize ts = text_layer_get_content_size(s_detail_content_layer);
    scroll_layer_set_content_size(s_detail_scroll_layer,
        GSize(inner_w, ts.h + 24));

    window_set_click_config_provider(window, detail_click_config);
}

static void detail_window_unload(Window *window) {
    layer_destroy(s_detail_canvas_layer);        s_detail_canvas_layer  = NULL;
    text_layer_destroy(s_detail_header_layer);   s_detail_header_layer  = NULL;
    text_layer_destroy(s_detail_content_layer);  s_detail_content_layer = NULL;
    scroll_layer_destroy(s_detail_scroll_layer); s_detail_scroll_layer  = NULL;
}

// ============================================================================
// MERGED HISTORY
// ============================================================================

static void merged_rebuild(void) {
    s_merged_count = 0;

    for (int i = 0; i < s_history_count && s_merged_count < MAX_MERGED; i++) {
        MergedEntry *e = &s_merged[s_merged_count++];
        e->kind = MERGED_SMART;
        e->src_idx = i;
        e->dest = s_history_dest[i];
        e->timestamp = s_history_ts[i];
        strncpy(e->title, s_history_short[i], HISTORY_SHORT_LEN - 1);
        e->title[HISTORY_SHORT_LEN - 1] = '\0';
        format_relative_meta(e->timestamp, e->dest, e->meta, sizeof(e->meta));
    }

    for (int i = 0; i < s_rem_count && s_merged_count < MAX_MERGED; i++) {
        int si = s_rem_count - 1 - i;
        MergedEntry *e = &s_merged[s_merged_count++];
        e->kind = MERGED_LOCAL;
        e->src_idx = i;
        e->dest = DEST_LOCAL;
        e->timestamp = s_rem_ts[si];
        strncpy(e->title, s_rem_text[si], HISTORY_SHORT_LEN - 1);
        e->title[HISTORY_SHORT_LEN - 1] = '\0';
        format_relative_meta(e->timestamp, DEST_LOCAL, e->meta, sizeof(e->meta));
    }

    for (int i = 0; i < s_merged_count - 1; i++) {
        for (int j = i + 1; j < s_merged_count; j++) {
            if (s_merged[j].timestamp > s_merged[i].timestamp) {
                MergedEntry tmp = s_merged[i];
                s_merged[i] = s_merged[j];
                s_merged[j] = tmp;
            }
        }
    }
}

static void merged_item_open(int index) {
    if (index < 0 || index >= s_merged_count) return;
    if (s_merged[index].kind == MERGED_SMART) {
        detail_window_push(s_merged[index].src_idx);
    } else {
        rem_detail_window_push(s_merged[index].src_idx);
    }
}

// Total list content height (entries + end-of-list footer).
static int hist_content_height(void) {
    return s_merged_count > 0 ? (s_merged_count + 1) * MENU_ROW_H : 0;
}

static int hist_max_scroll(int list_h) {
    int max = hist_content_height() - list_h;
    return max > 0 ? max : 0;
}

// Scroll offset that keeps the last real entry visible (footer may stay below).
static int hist_last_item_scroll(int list_h) {
    if (s_merged_count <= 0) return 0;
    int top = (s_merged_count - 1) * MENU_ROW_H;
    int max = top + MENU_ROW_H - list_h;
    return max > 0 ? max : 0;
}

static void draw_hist_footer_row(GContext *ctx, int y, int w) {
    graphics_context_set_text_color(ctx, GColorLightGray);
    graphics_draw_text(ctx, "NO MORE ENTRIES", app_font(AF_FOOTER),
        GRect(0, y + 18, w, app_font_h(AF_FOOTER) + 4), GTextOverflowModeFill, GTextAlignmentCenter, NULL);
}

// Draw one merged row at top-left (0,y) within width w.
static void draw_merged_row(GContext *ctx, const MergedEntry *e, int y, int w, bool selected) {
    graphics_context_set_fill_color(ctx, selected ? C_BAR : C_SCREEN);
    graphics_fill_rect(ctx, GRect(0, y, w, MENU_ROW_H), 0, GCornerNone);

    GColor fg      = selected ? C_ON_BAR : C_ON_SCREEN;
    GColor meta_fg = selected ? C_ON_BAR : GColorLightGray;

    int tile_cx = 6 + MENU_TILE_SIZE / 2;
    draw_dest_tile(ctx, e->dest, GPoint(tile_cx, y + MENU_ROW_H / 2), MENU_TILE_SIZE, fg);

    int text_x = tile_cx + MENU_TILE_SIZE / 2 + 8;
    int text_w = w - text_x - 4;
    int th = app_font_h(AF_HEADER);
    int mh = app_font_h(AF_FOOTER);
    int pad = (MENU_ROW_H - th - mh) / 2;
    if (pad < 2) pad = 2;
    // Nudge the title/meta block up (Pebble text has top leading) so it optically
    // centers on the tile, and pull the meta up to tighten the gap under the title.
    graphics_context_set_text_color(ctx, fg);
    graphics_draw_text(ctx, e->title, app_font(AF_HEADER),
        GRect(text_x, y + pad - 1, text_w, th + 2),
        GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
    graphics_context_set_text_color(ctx, meta_fg);
    graphics_draw_text(ctx, e->meta, app_font(AF_FOOTER),
        GRect(text_x, y + pad + th - 5, text_w, mh + 2),
        GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

    // Row divider (skip under a selected/inverted row)
    if (!selected) {
        graphics_context_set_stroke_color(ctx, GColorDarkGray);
        graphics_draw_line(ctx, GPoint(0, y + MENU_ROW_H - 1), GPoint(w, y + MENU_ROW_H - 1));
    }
}

// Top inset before the first row. On round, push rows clear of the top curve
// and center the title; on rectangular it sits right under the status bar.
#ifdef PBL_ROUND
#  define LIST_TOP (STATUS_H + 22)
#else
#  define LIST_TOP STATUS_H
#endif

static void hist_list_update(Layer *layer, GContext *ctx) {
    GRect bounds = layer_get_bounds(layer);
    int w = bounds.size.w;
    int h = bounds.size.h;
    int list_h = bounds.size.h - LIST_TOP;

    // Background
    graphics_context_set_fill_color(ctx, C_SCREEN);
    graphics_fill_rect(ctx, bounds, 0, GCornerNone);

    bool empty = (s_merged_count == 0);

    // Status title "HISTORY"
    graphics_context_set_text_color(ctx, C_ON_SCREEN);
#ifdef PBL_ROUND
    graphics_draw_text(ctx, "HISTORY", app_font(AF_STATUS),
        GRect(0, LIST_TOP - 20, w, STATUS_H), GTextOverflowModeFill, GTextAlignmentCenter, NULL);
#else
    graphics_draw_text(ctx, "HISTORY", app_font(AF_STATUS),
        GRect(STATUS_TITLE_X, 1, STATUS_HDR_TITLE_W(w), STATUS_H),
        GTextOverflowModeFill, GTextAlignmentLeft, NULL);
    // Clock on the action bar when empty; full-width header when populated.
    if (empty) {
        draw_status_clock_on_bar(ctx, bounds);
    } else {
        draw_status_clock(ctx, bounds.size.w);
    }
    draw_status_divider(ctx, bounds);
#endif

    // Empty state: full action bar + record button, message aligned with it.
    if (empty) {
        draw_action_bar(ctx, bounds);
        draw_icon_record(ctx, GPoint(action_bar_icon_x(bounds), btn_select_y(bounds)), ACTION_ICON_COLOR);
#ifdef PBL_ROUND
        int tw = w;                       // center across full width on round
#else
        int tw = content_area_w(bounds);  // center in the area left of the bar
#endif
        graphics_context_set_text_color(ctx, C_ON_SCREEN);
        graphics_draw_text(ctx, "NO ENTRIES", app_font(AF_TITLE),
            GRect(0, h / 2 - 22, tw, app_font_h(AF_TITLE) + 8), GTextOverflowModeFill, GTextAlignmentCenter, NULL);
        graphics_context_set_text_color(ctx, GColorLightGray);
        graphics_draw_text(ctx, "PRESS TO SPEAK", app_font(AF_STATUS),
            GRect(0, h / 2 + 16, tw, app_font_h(AF_STATUS) + 4), GTextOverflowModeFill, GTextAlignmentCenter, NULL);
        return;
    }

    for (int i = 0; i < s_merged_count; i++) {
        int y = LIST_TOP + i * MENU_ROW_H - s_hist_scroll;
        if (y + MENU_ROW_H <= LIST_TOP || y >= bounds.size.h) continue;  // offscreen
        draw_merged_row(ctx, &s_merged[i], y, w, i == s_hist_sel);
    }

    int footer_y = LIST_TOP + s_merged_count * MENU_ROW_H - s_hist_scroll;
    if (footer_y + MENU_ROW_H > LIST_TOP && footer_y < bounds.size.h) {
        draw_hist_footer_row(ctx, footer_y, w);
    }

    // Scroll indicator on the far right edge
    int total = hist_content_height();
    if (total > list_h) {
        int knob_h = list_h * list_h / total;
        if (knob_h < 8) knob_h = 8;
        int knob_y = LIST_TOP + (list_h - knob_h) * s_hist_scroll / (total - list_h);
        graphics_context_set_fill_color(ctx, GColorLightGray);
        graphics_fill_rect(ctx, GRect(w - 2, knob_y, 2, knob_h), 0, GCornerNone);
    }
}

// Keep the selected row fully visible.
static void hist_scroll_to_selection(int visible_h) {
    int top = s_hist_sel * MENU_ROW_H;
    int bot = top + MENU_ROW_H;
    if (top < s_hist_scroll) s_hist_scroll = top;
    else if (bot > s_hist_scroll + visible_h) s_hist_scroll = bot - visible_h;
}

static void hist_down_click(ClickRecognizerRef rec, void *ctx) {
    int list_h = layer_get_bounds(s_hist_list_layer).size.h - LIST_TOP;
    if (s_hist_sel < s_merged_count - 1) {
        s_hist_sel++;
        hist_scroll_to_selection(list_h);
    } else if (s_hist_scroll < hist_max_scroll(list_h)) {
        s_hist_scroll = hist_max_scroll(list_h);
    }
    layer_mark_dirty(s_hist_list_layer);
}

static void hist_up_click(ClickRecognizerRef rec, void *ctx) {
    int list_h = layer_get_bounds(s_hist_list_layer).size.h - LIST_TOP;
    if (s_hist_scroll > hist_last_item_scroll(list_h)) {
        s_hist_scroll = hist_last_item_scroll(list_h);
    } else if (s_hist_sel > 0) {
        s_hist_sel--;
        hist_scroll_to_selection(list_h);
    }
    layer_mark_dirty(s_hist_list_layer);
}

// ctx is non-NULL only for the timer the Quick Launch path registers at boot.
static void hist_start_dictation_cb(void *ctx) {
    start_dictation(ctx != NULL);
}

// SELECT is count-aware so it stays correct even when the list empties out via
// deletions (which fire `appear`, not `load`): empty → it's the record button;
// populated → open the selected row.
static void hist_select_click(ClickRecognizerRef rec, void *ctx) {
    if (s_merged_count == 0) {
        window_stack_pop(true);
        app_timer_register(300, hist_start_dictation_cb, NULL);
    } else {
        merged_item_open(s_hist_sel);
    }
}

static void hist_list_click_config(void *ctx) {
    window_single_click_subscribe(BUTTON_ID_UP,     hist_up_click);
    window_single_click_subscribe(BUTTON_ID_DOWN,   hist_down_click);
    window_single_click_subscribe(BUTTON_ID_SELECT, hist_select_click);
}

static void history_window_appear(Window *window) {
    merged_rebuild();
    if (s_hist_sel >= s_merged_count && s_merged_count > 0) {
        s_hist_sel = s_merged_count - 1;
    }
    if (s_hist_list_layer) layer_mark_dirty(s_hist_list_layer);
}

static void history_window_load(Window *window) {
    Layer *root = window_get_root_layer(window);
    GRect b = layer_get_bounds(root);
    // History is full-width (no action bar) per handoff.
    int content_w = b.size.w;

    window_set_background_color(window, C_SCREEN);
    merged_rebuild();

    // One canvas renders both the list and the empty state (title, clock,
    // message, record affordance) so the chrome is identical either way.
    s_hist_row_w  = content_w;
    s_hist_sel    = 0;
    s_hist_scroll = 0;
    s_hist_list_layer = layer_create(GRect(0, 0, content_w, b.size.h));
    layer_set_update_proc(s_hist_list_layer, hist_list_update);
    layer_add_child(root, s_hist_list_layer);

    // One provider for both states; hist_select_click branches on the live count
    // so record vs open-item is always correct (see hist_select_click).
    window_set_click_config_provider(window, hist_list_click_config);
}

static void history_window_unload(Window *window) {
    if (s_hist_list_layer) { layer_destroy(s_hist_list_layer); s_hist_list_layer = NULL; }
}

// ============================================================================
// REMINDER DETAIL WINDOW
// ============================================================================

static void rem_detail_up_click(ClickRecognizerRef rec, void *ctx) {
    s_rem_scroll_offset -= RESP_SCROLL_STEP;
    if (s_rem_scroll_offset < 0) s_rem_scroll_offset = 0;
    scroll_layer_set_content_offset(s_rem_detail_scroll,
        GPoint(0, -s_rem_scroll_offset), true);
}

static void rem_detail_down_click(ClickRecognizerRef rec, void *ctx) {
    GSize cs = scroll_layer_get_content_size(s_rem_detail_scroll);
    GRect fr = layer_get_frame(scroll_layer_get_layer(s_rem_detail_scroll));
    int16_t max_scroll = cs.h - fr.size.h;
    if (max_scroll < 0) max_scroll = 0;
    s_rem_scroll_offset += RESP_SCROLL_STEP;
    if (s_rem_scroll_offset > max_scroll) s_rem_scroll_offset = max_scroll;
    scroll_layer_set_content_offset(s_rem_detail_scroll,
        GPoint(0, -s_rem_scroll_offset), true);
}

static void rem_detail_select_click(ClickRecognizerRef rec, void *ctx) {
    if (!s_rem_confirm) {
        s_rem_confirm = true;
        strncpy(s_rem_header_buf, "DELETE?", sizeof(s_rem_header_buf) - 1);
        s_rem_header_buf[sizeof(s_rem_header_buf) - 1] = '\0';
        text_layer_set_text(s_rem_detail_header, s_rem_header_buf);
        layer_mark_dirty(s_rem_detail_canvas_layer);
    } else {
        reminders_delete(s_rem_detail_idx);
        window_stack_pop(true);
    }
}

static void rem_detail_click_config(void *ctx) {
    window_single_click_subscribe(BUTTON_ID_UP,     rem_detail_up_click);
    window_single_click_subscribe(BUTTON_ID_DOWN,   rem_detail_down_click);
    window_single_click_subscribe(BUTTON_ID_SELECT, rem_detail_select_click);
}

static void rem_detail_canvas_update(Layer *layer, GContext *ctx) {
    GRect bounds = layer_get_bounds(layer);
    graphics_context_set_fill_color(ctx, C_SCREEN);
    graphics_fill_rect(ctx, bounds, 0, GCornerNone);
    draw_action_bar(ctx, bounds);
#ifndef PBL_ROUND
    draw_status_clock_on_bar(ctx, bounds);
    draw_status_divider(ctx, bounds);
#endif
    int ax = action_bar_icon_x(bounds);
    if (s_rem_confirm) {
        draw_icon_checkmark(ctx, GPoint(ax, btn_select_y(bounds)), ACTION_ICON_COLOR);
    } else {
        draw_icon_trash(ctx, GPoint(ax, btn_select_y(bounds)), ACTION_ICON_COLOR);
    }
}

static void rem_detail_window_load(Window *window) {
    Layer *root = window_get_root_layer(window);
    GRect b = layer_get_bounds(root);
    int content_w = content_area_w(b);

    window_set_background_color(window, C_SCREEN);

    s_rem_detail_canvas_layer = layer_create(b);
    layer_set_update_proc(s_rem_detail_canvas_layer, rem_detail_canvas_update);
    layer_add_child(root, s_rem_detail_canvas_layer);

    strncpy(s_rem_header_buf, "NOTE", sizeof(s_rem_header_buf) - 1);
#ifdef PBL_ROUND
    int rhdr_w = content_w - 8;
#else
    int rhdr_w = STATUS_HDR_TITLE_W(content_w);
#endif
    s_rem_detail_header = text_layer_create(GRect(STATUS_TITLE_X, 1, rhdr_w, STATUS_H));
    text_layer_set_background_color(s_rem_detail_header, GColorClear);
    text_layer_set_text_color(s_rem_detail_header, C_ON_SCREEN);
    text_layer_set_font(s_rem_detail_header, app_font(AF_STATUS));
    text_layer_set_overflow_mode(s_rem_detail_header, GTextOverflowModeTrailingEllipsis);
    text_layer_set_text(s_rem_detail_header, s_rem_header_buf);
    layer_add_child(root, text_layer_get_layer(s_rem_detail_header));

    int si = s_rem_count - 1 - s_rem_detail_idx;
    s_rem_scroll_offset = 0;
    GRect scroll_frame = GRect(0, STATUS_H + 2, content_w, b.size.h - STATUS_H - 2);
    s_rem_detail_scroll = scroll_layer_create(scroll_frame);
    layer_add_child(root, scroll_layer_get_layer(s_rem_detail_scroll));

    int inner_w = content_w - 8;
    s_rem_detail_content = text_layer_create(GRect(4, 4, inner_w, 2000));
    text_layer_set_background_color(s_rem_detail_content, GColorClear);
    text_layer_set_text_color(s_rem_detail_content, C_ON_SCREEN);
    text_layer_set_font(s_rem_detail_content, app_font(AF_BODY));
    text_layer_set_overflow_mode(s_rem_detail_content, GTextOverflowModeWordWrap);
    text_layer_set_text(s_rem_detail_content, s_rem_text[si]);
    scroll_layer_add_child(s_rem_detail_scroll,
                           text_layer_get_layer(s_rem_detail_content));

    GSize ts = text_layer_get_content_size(s_rem_detail_content);
    scroll_layer_set_content_size(s_rem_detail_scroll, GSize(inner_w, ts.h + 24));

    window_set_click_config_provider(window, rem_detail_click_config);
}

static void rem_detail_window_unload(Window *window) {
    layer_destroy(s_rem_detail_canvas_layer);   s_rem_detail_canvas_layer = NULL;
    text_layer_destroy(s_rem_detail_header);    s_rem_detail_header  = NULL;
    text_layer_destroy(s_rem_detail_content); s_rem_detail_content = NULL;
    scroll_layer_destroy(s_rem_detail_scroll); s_rem_detail_scroll  = NULL;
}

// ============================================================================
// WINDOW PUSH HELPERS
// ============================================================================

static void home_window_push(void) {
    s_home_window = window_create();
    window_set_window_handlers(s_home_window, (WindowHandlers) {
        .load   = home_window_load,
        .unload = home_window_unload,
    });
    window_stack_push(s_home_window, true);
}

static void confirm_window_push(void) {
    if (s_confirm_window) {
        window_destroy(s_confirm_window);
        s_confirm_window = NULL;
    }
    s_confirm_window = window_create();
    window_set_window_handlers(s_confirm_window, (WindowHandlers) {
        .load   = confirm_window_load,
        .unload = confirm_window_unload,
    });
    window_stack_push(s_confirm_window, true);
    // Ask phone to classify in parallel so the target label can be shown
    if (is_phone_connected()) {
        appmsg_classify_note(s_note_buf);
    }
}

static void response_window_push(void) {
    if (s_response_window) {
        window_destroy(s_response_window);
        s_response_window = NULL;
    }
    s_response_window = window_create();
    window_set_window_handlers(s_response_window, (WindowHandlers) {
        .load   = response_window_load,
        .unload = response_window_unload,
    });
    window_stack_push(s_response_window, true);
}

static void history_window_push(void) {
    history_load_from_persist();
    reminders_load_from_persist();
    if (s_history_window) {
        window_destroy(s_history_window);
        s_history_window = NULL;
    }
    s_history_window = window_create();
    window_set_window_handlers(s_history_window, (WindowHandlers) {
        .load   = history_window_load,
        .unload = history_window_unload,
        .appear = history_window_appear,
    });
    window_stack_push(s_history_window, true);
}

static void detail_window_push(int idx) {
    s_detail_idx = idx;
    s_detail_confirm = false;
    if (s_detail_window) {
        window_destroy(s_detail_window);
        s_detail_window = NULL;
    }
    s_detail_window = window_create();
    window_set_window_handlers(s_detail_window, (WindowHandlers) {
        .load   = detail_window_load,
        .unload = detail_window_unload,
    });
    window_stack_push(s_detail_window, true);
}

static void success_window_create_and_push(void) {
    s_success_scrolled = false;
    if (s_success_window) {
        // Unloading the old window clears these; they belong to the new one.
        bool pending = s_success_pending, exit_on_close = s_success_exit_on_close;
        if (window_stack_contains_window(s_success_window)) {
            window_stack_remove(s_success_window, false);
        }
        window_destroy(s_success_window);
        s_success_window = NULL;
        s_success_pending = pending;
        s_success_exit_on_close = exit_on_close;
    }
    s_success_window = window_create();
    window_set_window_handlers(s_success_window, (WindowHandlers) {
        .load   = success_window_load,
        .unload = success_window_unload,
    });
    window_stack_push(s_success_window, true);
}

// The send is confirmed. If the SENDING screen is still up, flip it to DUMPED
// in place (keeping the user's scroll position); otherwise push a fresh one.
static void success_window_push(int dest) {
    s_success_dest = dest;
    if (s_success_pending && s_success_window && window_stack_contains_window(s_success_window)) {
        s_success_pending = false;
        success_relayout();
        return;
    }
    s_success_pending = false;
    success_window_create_and_push();
}

// A cloud note just went to the phone: show it while the phone works on it.
// The destination chip appears once ROUTING_DONE names it.
static void success_window_push_pending(void) {
    s_success_dest = -1;
    s_webhook_msg_buf[0] = '\0';
    s_success_pending = true;
    success_window_create_and_push();
}

// The send failed or turned into an AI conversation: take the SENDING screen
// down so the fallback / response window isn't stacked on top of it.
static void success_pending_dismiss(void) {
    if (!s_success_pending) return;
    s_success_pending = false;
    if (s_success_window && window_stack_contains_window(s_success_window)) {
        window_stack_remove(s_success_window, false);
    }
}

static void rem_detail_window_push(int display_idx) {
    s_rem_detail_idx = display_idx;
    s_rem_confirm    = false;
    if (s_rem_detail_window) {
        window_destroy(s_rem_detail_window);
        s_rem_detail_window = NULL;
    }
    s_rem_detail_window = window_create();
    window_set_window_handlers(s_rem_detail_window, (WindowHandlers) {
        .load   = rem_detail_window_load,
        .unload = rem_detail_window_unload,
    });
    window_stack_push(s_rem_detail_window, true);
}

// ============================================================================
// APP LIFECYCLE
// ============================================================================

#ifdef DEBUG_SEED
static void debug_seed(void) {
    // Seed each store independently so a leftover store from a prior run
    // doesn't suppress seeding of the other.
    if (s_history_count == 0) {
        history_save_item("Draft Q3 deck for Sarah",
                          "Sure — quick outline:\n1. Vision\n2. Metrics\n3. The ask",
                          DEST_AI, NULL);
        history_save_item("Renew the domain", NULL, DEST_TODOIST, NULL);
    }
    if (s_rem_count == 0) {
        reminders_add("Buy oat milk");
        reminders_add("Voice memo widget idea");
    }
}
#endif

static void init(void) {
    // Sample the OS "Text Size" pref so all chrome scales to match the native UI.
    s_csize = preferred_content_size();

    // AppMessage — register before open
    app_message_register_inbox_received(inbox_received_callback);
    app_message_register_inbox_dropped(inbox_dropped_callback);
    app_message_register_outbox_failed(outbox_failed_callback);
    // Outbox fits a full continuous-mode note (NOTE_TEXT up to NOTE_BUF_SIZE)
    // plus its recording log.
    uint32_t outbox = NOTE_BUF_SIZE + sizeof(s_rec_summary) + 128;
    if (outbox > app_message_outbox_size_maximum()) outbox = app_message_outbox_size_maximum();
    app_message_open(512, outbox);

    app_focus_service_subscribe_handlers((AppFocusHandlers) {
        .did_focus = app_focus_changed,
    });
    connection_service_subscribe((ConnectionHandlers) {
        .pebble_app_connection_handler = connection_handler,
    });

    // Dictation session: one chunk's worth of transcript
    s_dictation_session = dictation_session_create(
        DICTATION_BUF_SIZE, dictation_callback, NULL);
    dictation_session_enable_confirmation(s_dictation_session, false);
    // No system "Missed that. Try again." dialogs: with them on, the OS
    // silently restarts recording after a failed take and never tells the
    // app, so the words are lost without warning. Off, every failure reaches
    // dictation_callback, which warns (SOS) and handles silence quietly.
    dictation_session_enable_error_dialogs(s_dictation_session, false);

    // Hero icon
    s_brain_bmp = gbitmap_create_with_resource(RESOURCE_ID_IMG_BRAIN_MIC);

    // Load persistent data
    s_history_full = malloc(MAX_HISTORY * HISTORY_FULL_LEN);
    history_load_from_persist();
    reminders_load_from_persist();

#ifdef DEBUG_SEED
    debug_seed();
#endif

    // Quick Launch behavior flags — last values pushed from the phone settings.
    s_quick_launch = (launch_reason() == APP_LAUNCH_QUICK_LAUNCH);
    // persist_read_bool() already reports false for a key never written, so the
    // opt-in flag needs no persist_exists(). Auto-exit defaults to ON, so there
    // the exists check is what separates "never configured" from "turned off".
    s_cfg_quick_skip_confirm = persist_read_bool(PERSIST_QUICK_SKIP_CONFIRM);
    s_cfg_skip_confirm       = persist_read_bool(PERSIST_SKIP_CONFIRM);
    s_cfg_continuous         = persist_read_bool(PERSIST_CONTINUOUS);
    s_cfg_silence_timeout_s  = persist_read_int(PERSIST_SILENCE_TIMEOUT);
    s_cfg_max_note_min = persist_exists(PERSIST_MAX_NOTE_MIN)
                         ? max_note_min_clamp(persist_read_int(PERSIST_MAX_NOTE_MIN))
                         : MAX_NOTE_MIN_DEFAULT;
    s_cfg_vibe_start = persist_exists(PERSIST_VIBE_START)
                       ? vibe_preset_clamp(persist_read_int(PERSIST_VIBE_START))
                       : VIBE_PRESET_DEFAULT;
    s_cfg_vibe_stop  = persist_exists(PERSIST_VIBE_STOP)
                       ? vibe_preset_clamp(persist_read_int(PERSIST_VIBE_STOP))
                       : VIBE_PRESET_DEFAULT;
    s_cfg_quick_auto_exit    = !persist_exists(PERSIST_QUICK_AUTO_EXIT)
                               || persist_read_bool(PERSIST_QUICK_AUTO_EXIT);

    // Push home window
    home_window_push();

    // If launched via quick launch (long-press from watch face), auto-start dictation
    if (s_quick_launch) {
        app_timer_register(400, hist_start_dictation_cb, (void *)1);
    }
}

static void deinit(void) {
    connection_service_unsubscribe();
    if (s_dictation_session) {
        dictation_session_destroy(s_dictation_session);
        s_dictation_session = NULL;
    }
    if (s_brain_bmp)         { gbitmap_destroy(s_brain_bmp); s_brain_bmp = NULL; }
    free(s_history_full); s_history_full = NULL;
    if (s_fallback_window)   { window_destroy(s_fallback_window);   }
    if (s_success_window)    { window_destroy(s_success_window);    }
    if (s_continue_window)   { window_destroy(s_continue_window);   }
    if (s_rem_detail_window) { window_destroy(s_rem_detail_window); }
    if (s_detail_window)     { window_destroy(s_detail_window);     }
    if (s_history_window)    { window_destroy(s_history_window);    }
    if (s_confirm_window)    { window_destroy(s_confirm_window);    }
    if (s_response_window)   { window_destroy(s_response_window);   }
    if (s_home_window)       { window_destroy(s_home_window);       }
    app_message_deregister_callbacks();
}

int main(void) {
    init();
    app_event_loop();
    deinit();
    return 0;
}
