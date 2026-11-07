# ntfy: pushes from outside

`ztos-ntfyd` (user service) subscribes to topics on [ntfy](https://docs.ntfy.sh) servers, ntfy.sh or your own, and shows each message as an ordinary notification. `ztntfy` (the NTFY app) sets it up. There's no account and nothing to register: anything that can make a web request can reach the phone.

```
curl -d "hello" ntfy.sh/mytopic
curl -H "Title: Backup" -H "Priority: 5" -H "Tags: warning" -d "The disk is full" ntfy.sh/mytopic
```

With no topics the daemon connects to nothing, not even the session bus. No topic is shipped.

## Use

1. Open NTFY, tap **add topic**. The server starts as `https://ntfy.sh`. **random name** makes a topic name.
2. Open the topic, tap **send a test**. A notification should arrive.
3. Send your own with the `curl` line the topic's page shows.

**On a public server, a topic's name is its password.** Anyone who knows it can read its messages and send to it, and ntfy.sh doesn't ask who you are. Use a long random name, don't reuse it, and send nothing private through a public server. For anything private, use your own server with a sign-in (a token, or a username and password). Text on ntfy.sh is also visible to whoever runs it.

`ztntfy --add SERVER TOPIC` does the same as the page, for a topic with no sign-in.

## What arrives

| ntfy | notification |
|---|---|
| topic | the app's name in the action centre |
| title | title (no title: the server's name) |
| message | text |
| priority 1, 2 | quiet: no sound, no buzz, no LED |
| priority 3, 4, 5 | normal. ztnotifyd has only those two kinds, so 5 is no louder than 3 |
| tags | known ones (`warning`, `skull`, `tada`, ...) as emoji before the title, others as words after the text. With no emoji font: all as words, and emoji in the text left out |
| attachment | its name, as a word. Never fetched |
| click, actions, icon | ignored. No address from a server is ever opened, and nothing from a server is run |

Text is cleaned first (`ntfy-model.h`): valid UTF-8, no control or bidi characters or invisible ones, a few combining marks at most, and cut on a character's edge to what the shell keeps (31, 47 and 199 bytes). `<` and `>` become look-alikes, because `ztnotifyd` removes anything that looks like markup, and an open `<` would take the rest of the text with it.

A quiet message replaces, in place, an earlier notification with the same topic and title (the shell's `note_keyed` treats a quiet one as an update). So quiet messages with no title, or the same title, show as only the newest. Give them different titles to keep them all.

Not handled: ntfy's `message_delete` and `message_clear` events and `sequence_id` updates. They don't take back or change a notification.

## Behaviour

- **One stream per topic**, each with its own place, sign-in and backoff, up to 16 topics, on any mix of servers. A combined `a,b/json` stream has one `since=`, and saved ids are per topic. libcurl may share one connection among streams to an HTTP/2 server; that hasn't been checked here.
- **Missed messages:** each stream resumes with `since=<last id>`, and a new topic with `since=<when it was added>`, so its older messages stay away. The saved id moves only after the notification service has taken the message. What the server has kept is all that can arrive: the server decides how long that is (ntfy's default was about 12 hours when this was written; not checked against ntfy.sh).
- **Duplicates:** the last 32 ids per topic are remembered and 8 are saved. A message over a minute older than the last one shown is taken for a replay.
- **Backlog:** up to 3 messages from a reconnect show one by one. More become one summary, "N new messages", with the text of the most urgent. It stays quiet only if every message was quiet.
- **Floods:** 10 alerts at once per topic, then one per 15 s, with the rest summarised. A server sending over 32 MB in 10 s is hung up on.
- **Reconnecting:** 5 s doubling to 5 minutes, with jitter. A refused sign-in or a 429 goes straight to the longest wait. A new network address retries at once and restarts any stream that has been quiet for 5 s. 150 s of silence on a stream (ntfy's keepalive is 45 s) ends it.
- **If nothing takes the notification** (`org.freedesktop.Notifications` has no owner), it waits, up to 30 s between tries, and goes as soon as the name appears. But `ztnotifyd` answers even if the shell isn't running, and then drops the message. That's outside this daemon.

## Sleep

It holds no inhibit and no wake alarm, so it never keeps or wakes the phone. Its only timer is `CLOCK_BOOTTIME`, and the socket wakes it otherwise. **While the SoC sleeps, Wi-Fi is unbound (`power/suspend/README.md`), so a push arrives at the next wake**, and not when it's sent. That wake is when rtnetlink reports the address, and the stream reconnects with `since=`.

libcurl's `CURLOPT_LOW_SPEED_*` and `CURLOPT_MAX_RECV_SPEED_LARGE` must stay unused here. The second was measured waking the daemon once a second on a silent stream (`make check-ntfyd`, "real timings"), and the first is avoided on the same suspicion, from memory of curl's source and not measured.

## Security

- `https` is always verified, with no option to turn that off. `http` works only if you typed `http://`, and the app warns. Redirects aren't followed. A proxy from the environment isn't used.
- Sign-ins are in `~/.config/ztmetro/ntfy/subscriptions`, 0600, in a 0700 directory. If the file has become readable by others, the daemon closes it again. Test messages go through `ztos-ntfyd --publish ID`, so secrets stay off command lines.
- The log and the status file name a topic by a random id, never its name, and never hold a secret.
- Lines are capped at 64 KiB. Longer ones are dropped whole and the stream carries on.

## The other ntfy client

`ztnotifyd` has an older, simpler client (its section 3) that reads `~/.config/ztmetro/push`. Nothing writes that file now. It has no `since=` or priorities. Don't use both on a topic, or messages show twice.

## Files

| File | What |
|---|---|
| `ntfy-model.h` | Parsing, sanitising, the files' formats, backlog and backoff. No I/O, no clock |
| `ztos-ntfyd.c` | libcurl, sd-bus, timers, files |
| `ztntfy.c` | The app. Writes the subscriptions file, reads the status |
| `~/.local/state/ztmetro/ntfy-state` | Where each topic has got to |
| `$XDG_RUNTIME_DIR/ztos-ntfyd.status` | How each topic is doing |

## Tests

`make check-ntfy` (any machine, no libraries): hostile input against the model.

`make check-ntfyd` (on the phone): the real daemon against a pretend server on loopback and a pretend notification service, then the real `ztnotifyd`, on a private bus. It runs in its own network and mount namespaces when it can, so it can't reach a real server, and it opens no window. It covers streaming, reconnect, `since=`, dedupe, backlog, floods, oversized and malformed lines, no connection with no topics, TLS (trusted, self-signed, wrong name), secrets not leaking, wake counts and a network change. Without namespaces, the https-success and network-change parts skip.
