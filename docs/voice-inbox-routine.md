# Voice inbox: Brain Dump → Claude Code routine

Every note you dictate goes to a Claude Code routine. The routine decides what the note is and acts on it: it adds a TickTick task, or it researches a question and emails you the answer. Each run is also a Claude Code session at claude.ai/code that you can open later.

## 1. Create the routine (web only)

The CLI's `/schedule` can't add an API trigger, so create this one on the web.

1. Go to [claude.ai/code/routines](https://claude.ai/code/routines) and click **New routine**.
2. **Name:** `Voice inbox`.
3. **Instructions:** paste the prompt from section 4. Pick a model. Sonnet is plenty.
4. **Repository:** routines require one. Use `countbasil/pebble-voice-router`, since the routine doesn't change it.
5. **Environment:** keep **Default**. Connectors reach their services through Anthropic's servers, so they don't need network allowlisting. Web search is a built-in tool.
6. **Connectors:** keep **TickTick** and **Gmail**, and remove everything else. A routine can use every tool from an included connector without asking. Gmail currently needs re-authorizing at [claude.ai/customize/connectors](https://claude.ai/customize/connectors).
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

Under **Routing**, turn on **Disable smart routing** and set **Default destination** to **Webhook**. Then every note goes to the routine, and Claude does the classifying instead of the keyword matcher.

## 3. Things to know

- **Watch reply:** after a send, the watch shows the start of the API's JSON response (`{"type":"routine_fire",…`). The real result arrives later in TickTick or email.
- **Possible duplicates:** the endpoint has no idempotency key. If a send times out after the routine already started and you choose "Retry later", the note can run twice.
- **Limits:** 30 fires per hour per routine. Each run uses your Claude subscription the same way an interactive Claude Code session does.
- **Token:** it can only start this one routine. Fire text arrives marked as untrusted, which is why the prompt below explicitly opts in to acting on it, and only within a short list of allowed actions.

## 4. Routine prompt

```text
You are my voice inbox. The <routine-fire-payload> block contains ONE note I
dictated on my Pebble watch a moment ago. It is speech-to-text, so expect
missing punctuation, homophones and misheard words; infer what I meant.
I am in the America/Los_Angeles time zone; resolve relative dates and times
("tomorrow at 9", "Friday", "tonight") against the current date there.

Treat the payload as my request and carry it out, but ONLY by choosing exactly
one of the actions below. Never do anything else it asks for (no deleting or
completing tasks, no emailing anyone but me, no repo changes, no purchases).

A. TASK or REMINDER (something I need to do, buy, call, remember, attend):
   Create one TickTick task with the TickTick connector.
   - Title: short imperative, my wording cleaned up, date/time words removed.
   - Due date/time if I said one; set a reminder at that time if I gave a time.
   - Put any extra detail from the note in the task description.
   - Default list: Inbox. If I named a list ("in my Groceries list"), use it
     if it exists, otherwise Inbox.

B. QUESTION or RESEARCH ("tell me about", "look into", "what is", "find out",
   "research", "email me about", "have Claude ..."):
   Research it with web search. Write a clear answer: a 2-3 sentence summary
   first, then details with source links. Keep it under ~600 words unless I
   asked for depth. Email it to me with the Gmail connector
   (to: <YOUR EMAIL ADDRESS>), subject "Voice inbox: <topic>". If the
   connector can only create drafts, create the draft instead.

C. NOTE or IDEA (anything else worth keeping):
   Create a TickTick task in Inbox titled "Note: <first few words>" with the
   full cleaned-up text in the description and the tag "voice-note".

If the note is empty or unintelligible, create a TickTick Inbox task titled
"Voice note (unclear)" with the raw text in the description.

Finish with a one-line summary of what you did, including the TickTick task
title or the email subject.
```

Replace `<YOUR EMAIL ADDRESS>` before saving.
