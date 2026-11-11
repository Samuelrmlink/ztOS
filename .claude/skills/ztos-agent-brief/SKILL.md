---
name: ztos-agent-brief
description: Template and checklist for briefing a background agent on ztOS work, covering branch setup, file ownership, phone rules and the report format. Use it before spawning any agent that will touch the repo or the phone.
---

# Briefing a ztOS agent

Every brief has these parts. Leave one out and the agent will guess.

1. **Branch.** "You are in your own worktree. First run `git fetch origin && git checkout -b NEW origin/BASE`." BASE is the integration branch (`feature/desktop-dp` today), or the feature branch being continued. A fresh worktree starts on whatever the main checkout has, which is usually the wrong base.
2. **What to read first:** the README or DESIGN for the area, plus the ROADMAP item numbers.
3. **The owner's request, verbatim**, then a condensed list of deliverables.
4. **Ownership.** Name the files and functions that belong to other running agents and must be left alone or kept minimal (big shared files: `ztmetro/ztmetro.c`, `ztmetro/desktop/desktop.c`, `ztmetro/settings/ztsettings.c`, `zrn-sleepd`). "Don't edit ROADMAP.md."
5. **Phone rules:** "Follow the `ztos-phone` skill." Repeat the hard limits inline anyway, since a fresh agent may not load the skill:
   - no EFS, RECOVERY or BOOT writes;
   - register writes need approval;
   - `ztlock run BRANCH`;
   - greetd-only restarts, never `pkill -u samuel`;
   - no Bluetooth actions;
   - `*.pre-BRANCH` backups;
   - don't touch the alarms.
   Say explicitly whether BOOT flashing is approved for this task. The default is no.
6. **Approvals.** Never grant an agent a permission the owner hasn't given. Quote the owner's approval if one exists. Otherwise: "if you need X, stop and report exactly what and why".
7. **Owner's hands.** "Batch steps that need the owner (unplugging, docking, unlocking, feel tests) into a list at the end. Don't wait on them."
8. **Finish:** "Commit in logical commits, push BRANCH, and report root causes with evidence, before/after numbers, which tests ran on-device and which host-only, what's deployed, how to restore it, and what's left for the owner."

After spawning, add or update the ROADMAP row (`ztos-roadmap`). When the agent finishes, merge its branch into the integration branch and tell the owner what changed.
