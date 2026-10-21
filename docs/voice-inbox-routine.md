# Brain Dump → PKM router routine

Every note you dictate goes to the **PKM router** Claude Code routine (formerly "Voice inbox"). It files captures into the Notion PKM lists and/or sends them to Readwise Reader, Gmail, TickTick or Google Calendar. Brain Dump is one of several senders; the others are iPhone Shortcuts, the iOS share sheet and Mac hotkeys.

The routine's prompt, behavior and setup docs live in `~/Developer/pkm`: see `routine/README.md` and `routine/prompt.md`. This page covers only the watch side.

## Point Brain Dump's webhook at the routine

In the Pebble app, open Brain Dump's settings.

Under **Custom Webhook**:

| Field | Value |
|---|---|
| Enabled | on |
| URL | `https://api.anthropic.com/v1/claude_code/routines/trig_0114xdVHWQNcsQ63o1LmyGJ9/fire` |
| Method | POST |
| Content-Type | `application/json` |
| Body (optional) | `{"text": "{text}"}` |
| Bearer token | `sk-ant-oat01-…` |
| Extra request headers | `anthropic-version: 2023-06-01`<br>`anthropic-beta: experimental-cc-routine-2026-04-01` |

The extra-headers field comes from upstream PR #16, which is merged into the `voice-inbox` branch. `anthropic-version` is required. The beta header is optional today but documented.

Also tick **Send my exact words** in the webhook section, so the routine sees "remind me to…", "email me…" and the other trigger phrases as you said them.

Tick **Add recording times to the note** too. The watch then ends each note with a line like `[Recording: 48s, 1m 12s (not recognized); total 2m]`, which the routine strips from the note and logs in the Notion log's **Recording** column. It shows how long each recording take was and which ones failed, which helps when a note comes through cut off.

Leave **Also ring same-day reminders on the watch** ticked (it's on by default). See "Things to know" below.

Under **Routing**, turn on **Disable smart routing** and set **Default destination** to **Webhook**. Then every note goes to the routine, and Claude does the classifying instead of the keyword matcher.

## Watch-side things to know

- **Watch reply:** after a send, the watch shows the start of the API's JSON response (`{"type":"routine_fire",…`). The real result arrives later in Notion, TickTick, email, Reader or your calendar.
- **Same-day reminders also ring on the watch:** TickTick on the iPhone may not alert for a new task until its app has synced it, and a backgrounded app may not sync in time. So when a note starts with "remind me" and gives a clock time or a delay ("at 5pm", "in 20 minutes") that falls later today, Brain Dump also pins a Timeline reminder on the watch, parsing the time itself on the phone. You may get two alerts. A note retried from the queue gets no pin, because "in 20 minutes" would no longer mean the same moment.
- **Possible duplicates:** the endpoint has no idempotency key. If a send times out after the routine already started and you choose "Retry later", the note can run twice.
- **Limits:** 30 fires per hour per routine. Each run uses your Claude subscription the same way an interactive Claude Code session does.
- **Token:** it can only start this one routine. Fire text arrives marked as untrusted, which is why the router prompt explicitly opts in to acting on it, and only with a short list of allowed destination types.
