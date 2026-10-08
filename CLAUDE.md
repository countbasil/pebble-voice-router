# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

"Brain Dump" — a Pebble watchapp (SDK 3, platforms basalt/chalk/gabbro/diorite/emery) that captures a dictated voice note on the watch and routes it, via PebbleKit JS on the phone, to one of several destinations: Google Tasks, Notion, an AI chat endpoint, a custom webhook, Discord, Todoist, Nextcloud Notes, Nextcloud Tasks, Joplin, Pebble Timeline (Rebble), or an on-watch local reminders list.

There are two source files, each large and single-file by design:
- `src/c/main.c` — the watch UI (windows, history, local reminders, persistence).
- `src/pkjs/index.js` — the phone side: intent classification, all destination integrations, retry queue, and the settings page.

## Commands

Build and install use the Pebble SDK CLI. On this machine it's installed with `uv tool install pebble-tool --python 3.13` (it doesn't support Python 3.14) at `~/.local/bin/pebble`, which may not be on PATH. The SDK comes from `pebble sdk install latest`. `wscript` is the standard SDK build script:

```sh
pebble build                       # outputs build/pebble-voice-router.pbw
pebble install --emulator emery    # boots QEMU in a GUI window; first boot can time out, so just rerun
pebble kill                        # stop the emulator
```

To drive the app end-to-end in the emulator without a mic, fake the dictation result, then press buttons and grab screenshots:

```sh
pebble transcribe --emulator emery "remind me to call mom tomorrow at 5pm" &   # answers the next dictation request
pebble emu-button --emulator emery click select    # start dictation; press again on the review screen to send
pebble screenshot --emulator emery --no-open out.png
pebble logs --emulator emery                       # console.log from pkjs + APP_LOG from C
```

`targetPlatforms` in `package.json` doesn't include `flint` (Pebble 2 Duo), though the SDK supports it.

Before building with Google Tasks support, copy `src/pkjs/secrets.example.js` to `src/pkjs/secrets.js` (gitignored) and set the OAuth client secret. It is bundled into the .pbw; without it the build still works but Tasks token exchange fails.

Tests are plain Node scripts with no framework or package deps. Each exits non-zero on failure:

```sh
node test/extractTime.test.js                       # single test file
for f in test/*.test.js; do node "$f" || exit 1; done   # all tests
```

## Test harness: two styles

- **Copied-function tests** (`extractTime`, `offlineQueue`, `webhookTemplate`): the functions under test are **copied into the test file** ("keep in sync with src/pkjs/index.js"). If you change those functions in `index.js`, update the copies too, or the tests will keep passing against stale code.
- **vm-loaded tests** (`joplinFormat`, `timelineFormat`, `webhookDiscordFormat`): load the real `index.js` into a `vm` context with stubbed `Pebble`, `localStorage`, `XMLHttpRequest`, and `setTimeout`, then drive the registered event listeners. These also regex-check `src/c/main.c` (e.g. `#define DEST_JOPLIN 9`, the `dest_full_name` case, the glyph letter) to keep watch and phone in sync.

## Architecture

### Watch ↔ phone protocol
All messages use AppMessage keys declared in `package.json` → `pebble.messageKeys` (exposed as `MESSAGE_KEY_*` in C). A new key must be added there before either side can use it.

Note flow:
1. The watch sends `NOTE_TEXT` + `NOTE_IS_CLASSIFY_ONLY` → the phone replies `ROUTING_DONE` (dest index) and optionally `DUE_LABEL`, which the review/confirm screen shows as "→ X".
2. On confirm, the watch sends `NOTE_TEXT` (+ `NOTE_IS_FOLLOWUP` for AI threads) → `routeAndSend` → the phone replies `ROUTING_DONE`, then `CONFIRM: 1` (success, with `DEST_USED`, optional `EXTERNAL_ID` / `WEBHOOK_MSG`) or `CONFIRM: 2` + `ERROR_MSG`. AI replies come back as `AI_RESPONSE` / `AI_RESPONSE_DONE`.
3. On failure the watch shows a fallback screen. "Retry later" sends `QUEUE_RETRY`, and the phone moves `s_pending_retry` into the persistent retry queue. "Save to local" keeps the note on-watch. These two paths are deliberately mutually exclusive so a note is never duplicated.

`pickDest()` is the single source of truth for destination choice, used by both the classify-only preview and the real send, so the preview always matches. It also forces task-like text away from `ai`.

`sendToWatch()` serializes outgoing AppMessages through an outbox, because only one can be in flight at a time. Always use it, never `Pebble.sendAppMessage` directly.

### Destination identity must match on both sides
Each destination has a numeric index that is duplicated across files:
- JS: `DEST_INDEX`, `computeDestMask()` bit positions, `QUEUEABLE_DESTS`, `DEST_LABEL` in `routeAndSend`, `getEnabledDests()`, and the `sendToDest()` switch.
- C: `DEST_*` / `DEST_BIT_*` defines in `main.c`, plus the `dest_full_name()` label and `draw_glyph_letter` icon cases.

Adding a destination means touching all of these plus the settings page (`openSettings`) and `classifyIntent` keywords. Follow an existing recent destination (Joplin, Timeline, Discord) as the template, along with its vm-loaded test.

### Phone-side state
- Config, history, and the retry queue are JSON in `localStorage` under versioned keys (`brain_dump_cfg_v1`, `_hist_v1`, `_queue_v1`, etc.).
- The retry queue (`flushQueue` / `flushStep`) drains on `ready` and after any successful live send. Failures are tracked per destination within a pass, so one down service doesn't block the others. Senders receive and return an opaque `retryState` so multi-step sends (Tasks, Discord, Timeline) can resume.
- The settings page is a single HTML string built in `openSettings()` and opened as a `data:` URL. It returns config via `pebblejs://close#<json>`, and `webviewclosed` saves it and re-sends `watchMirroredSettings()` (dest mask and Quick Launch flags). Google Tasks OAuth uses PKCE, with the verifier persisted in `localStorage`.

### Watch-side persistence
`main.c` uses numbered persist keys with explicit layout (`PERSIST_*` defines). History is versioned by `PERSIST_HISTORY_VERSION`, and long text is split into 200-byte chunks because of the 256-byte persist value cap. If you change the history record layout, bump the version.
