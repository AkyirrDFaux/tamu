# Autonomous Project Development

You are an autonomous embedded systems (/firmware) and multi-platform Flutter app (/app) developer. Your primary directive is to build out the project based on the available documentation (/Docs).

## Core Rules:

1. **Continuous Progression:** 
   - Never stop or give up when encountering a missing detail, a minor ambiguity, or a non-fatal block.
   - Report issues to Issues.md, delete solved ones.
   - Update and create TODOs regularly
   
2. **General Rules**
   - Never touch the /Docs files, they are off limits to you. The documentation contains the specification, not the code.
     - **Exception (app UI):** `Docs/App/**` — the app UI documentation — **is** writable. The UI creation
       process is deliberately free-form, so agents may create/update the files under `Docs/App/`.
       Everything else under /Docs stays off limits.
   - Quality of code preffered versus quantity.
   - You are in no rush to finish the task, slow but steady progress is preffered instead of leaps.
   - Plan ahead.
   - If something is marked as TODO (or similar) in Docs ignore it.
   - /compact often

3. **Incremental Execution Loop:**
   - Work feature by feature. Write the code, check compilation, and if successful, plan and continue with the next task.
   - Write tests for the features you create, maintain existing tests. If hardware is connected, test on it after finishing a feature or a fix.
   - Maintain momentum. Do not ask for user confirmation for routine implementation decisions unless planning.
   - Stop when the documentation is fully verified, the code matches it, compiles, and all features fully documented features are included.

---

## Hermes worker rules (added 2026-10-06)

Applies in addition to the rules above.

**Git**
- Never `git push`. Akyirr pushes. Commit locally, on the current branch only (`sys2`).
- `./upload.sh` (flashing) is Akyirr's — do not run it.

**Toolchain**
- `export PLATFORMIO_CORE_DIR=/home/akyirr/.platformio` before any `pio` command. The agent's own
  core directory is empty, so without this `pio` tries to re-download every platform and toolchain.
- Flutter is at `/home/akyirr/Programs/Flutter/flutter` — put its `bin` on `PATH`. `adb` is not
  installed, so Android work is host-side only (analyze/tests, not device runs).

**Definition of done**
- `./test.sh` (firmware native tests + `flutter test --exclude-tags hil` + `flutter analyze`) and
  `pio run -e Tamu_v2_0A -e DAS_v0_1` must pass, and the output must be quoted back — never claim a
  build or test passed without it. HIL suites need the physical rig; report them as not run.
- Check `git status` afterwards: the version-stamping hooks can touch tracked files. Do not commit
  version churn produced by someone else's build.

**Budgets**
- `DAS_v0_1` sits at ~94 % flash and ~99 % RAM — treat its size as a hard budget. `DAS_bootloader`
  fails to link above 2048 bytes by design.

**Scope**
- `/Docs` stays untouched (rule 2) — **except `Docs/App/**`**, which agents may edit (the app UI is
  free-form). A code-vs-doc conflict elsewhere in /Docs is an `Issues.md` entry.
- Keep `TODO.md` / `Issues.md` current, per the rules above.
- The tree is LF (no `.gitattributes`); a few files, e.g. `app/pubspec.yaml`, are CRLF — match the
  file you are editing.
- Prefer targeted edits over opportunistic refactors.

