# Voice inbox: Brain Dump → Claude Code routine

Every note you dictate goes to a Claude Code routine. The routine picks an action from how the note starts: "remind me to…" adds a TickTick task, "email me…" researches and emails you the answer, "add to buffer…" researches and saves it to Readwise Reader (tag `buffer`), "create an event" / "make an event" / "appointment…" adds a Google Calendar event, and anything else becomes a TickTick note. Each run is also a Claude Code session at claude.ai/code that you can open later.

## 1. Create the routine (web only)

The CLI's `/schedule` can't add an API trigger, so create this one on the web.

1. Go to [claude.ai/code/routines](https://claude.ai/code/routines) and click **New routine**.
2. **Name:** `Voice inbox`.
3. **Instructions:** paste the prompt from section 4. Pick a model. Sonnet is plenty.
4. **Repository:** routines require one. Use `countbasil/pebble-voice-router`, since the routine doesn't change it.
5. **Environment:** keep **Default**. Connectors reach their services through Anthropic's servers, so they don't need network allowlisting. Web search is a built-in tool.
6. **Connectors:** keep **TickTick**, **Gmail**, **Readwise** and **Google Calendar**, and remove everything else. A routine can use every tool from an included connector without asking. Gmail currently needs re-authorizing at [claude.ai/customize/connectors](https://claude.ai/customize/connectors).
7. **Trigger:** choose **API**, then click **Create**.
8. Edit the routine, open the API trigger, and copy the **URL**. Click **Generate token** and copy the token now, because it's shown only once.
9. Test it with **Run now**, using text like `remind me to call the dentist tomorrow at 9am`.

## 2. Point Brain Dump's webhook at it

In the Pebble app, open Brain Dump's settings.

Under **Custom Webhook**:

| Field | Value |
|---|---|
| Enabled | on |
| URL | `https://api.anthropic.com/v1/claude_code/routines/trig_…/fire` (from step 8) |
| Method | POST |
| Content-Type | `application/json` |
| Body (optional) | `{"text": "{text}"}` |
| Bearer token | `sk-ant-oat01-…` (from step 8) |
| Extra request headers | `anthropic-version: 2023-06-01`<br>`anthropic-beta: experimental-cc-routine-2026-04-01` |

The extra-headers field comes from upstream PR #16, which is merged into the `voice-inbox` branch. `anthropic-version` is required. The beta header is optional today but documented.

Also tick **Send my exact words** in the webhook section, so the routine sees "remind me to…", "email me…" and the other trigger phrases as you said them.

Under **Routing**, turn on **Disable smart routing** and set **Default destination** to **Webhook**. Then every note goes to the routine, and Claude does the classifying instead of the keyword matcher.

## 3. Things to know

- **Watch reply:** after a send, the watch shows the start of the API's JSON response (`{"type":"routine_fire",…`). The real result arrives later in TickTick, email, Reader or your calendar.
- **Possible duplicates:** the endpoint has no idempotency key. If a send times out after the routine already started and you choose "Retry later", the note can run twice.
- **Limits:** 30 fires per hour per routine. Each run uses your Claude subscription the same way an interactive Claude Code session does.
- **Token:** it can only start this one routine. Fire text arrives marked as untrusted, which is why the prompt (section 4) explicitly opts in to acting on it, and only within a short list of allowed actions.

## 4. Routine prompt

The prompt lives in [voice-inbox-prompt.md](voice-inbox-prompt.md), which contains nothing but the prompt text. To change it, ask Claude Code in this repo: it edits that file and pushes it to the routine (see CLAUDE.md).
