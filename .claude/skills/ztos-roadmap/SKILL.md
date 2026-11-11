---
name: ztos-roadmap
description: Conventions for ROADMAP.md, covering who edits it, item numbering, status words and size. Use it when adding a request to the roadmap or updating an item's status.
---

# ROADMAP.md

- **Only the coordinating session edits it.** Agents never do; they put status in their final report, so parallel branches don't conflict on it. The roadmap lives on the integration branch (`feature/desktop-dp`).
- **Every owner request goes in as it arrives**, then gets an agent where one can work on it. The status column names the agent's branch: "Agent: `branch`".
- Numbering: add to the current section (section 8 for desktop, sleep and clock work) with the next free number. Don't renumber old items, since other text refers to them.
- Row: `| N | **Short title.** What the owner asked for, in plain words. | Size | Status |`
  - Size: S = under a day, M = a few days, L = a week or more.
  - Status, from the file's header: **Confirmed** (seen working or failing on the phone), **Likely**, **Unknown**, **Review** (found by reading code). Add "host test only" or "not yet on the phone" when that's true.
- When an agent reports, update the row from its evidence. Don't upgrade to Confirmed without on-device proof.
- Bump "Last updated" at the top. Commit as `roadmap: ...` and push.
