---
name: save
description: Save trading-engine-core changes with an automatically written English Git commit message, then push to GitHub. Use when the user invokes $save or explicitly asks to commit and push this repository.
---

# Save trading-engine-core

Complete this workflow when the user invokes `$save` or requests commit and push.
That invocation authorizes a normal local commit and push for this repository;
choose the commit message and proceed without another routine confirmation.
Creating, inspecting, or explaining the skill does not invoke it. Honor requests
for preview-only, selected files, a supplied message, or commit without push.
Keep Codex permission controls in place; request tool approval when required.

## Repository and scope

Use the Git repository containing this skill at `.agents/skills/save/SKILL.md`.
Resolve its root rather than using an unrelated chat working directory. Confirm
`origin` points to `github.com/hojin-shin/trading-engine-core` (HTTPS or SSH).
If the target differs, ask for the intended destination before publishing.
Use the current named branch; stop for detached HEAD, unresolved conflicts, or
an in-progress merge/rebase/cherry-pick. Do not create branches or change remotes.

Read applicable repository instructions, `git status`, staged and unstaged diffs,
and relevant non-ignored new files. A bare `$save` covers current project changes,
including related new files and deletions. Respect narrower user scope. If scope
conflicts with already staged files, resolve it before committing; never silently
unstage or include changes the user excluded.

This is a clean-room public portfolio. Keep build output, credentials, private
trading strategies, employer code, and production data out of commits. If review
finds concrete sensitive or unrelated files, leave them untouched and ask only
about the ambiguous scope. Do not copy code from other repositories.

## Verify, commit, and push

1. Run `git diff --check`. For C++ or build changes, build and run CTest using a
   configured Linux build directory. On the author's Windows setup, Ubuntu-24.04
   has the build under the default Linux user's `~/build/trading-engine-core`;
   use WSL for the build and Windows Git for the existing GitHub credentials.
   Confirm the build's source directory matches this checkout. Reuse successful
   checks from this conversation if the tested source has not changed. Documentation
   and skill-only changes need focused validation, not a full C++ rebuild.
2. Derive a concise English commit message from the actual diff, for example
   `feat: add replay event tracing`. Use the user's supplied message if present.
   Briefly state the selected message and scope, then continue; do not stop at
   suggesting a message or instructing the user to run Git manually.
3. Stage the reviewed files with explicit paths, including reviewed deletions.
   Check the staged diff matches the intended scope. Commit using ordinary Git
   commands with proper shell quoting.
4. Push the current branch to `origin` without force, using an explicit destination
   such as `git push --set-upstream origin HEAD:refs/heads/<current-branch>`.
   A normal push also publishes existing unpushed commits on this branch; inspect
   them when present and respect any narrower scope the user supplied.
5. Verify push success, the remote branch tip against local HEAD, and remaining
   local changes. Report the commit ID, message, branch, GitHub commit link, and
   tests actually performed. Distinguish uncommitted, committed, and pushed states.

If there are no content changes, do not make an empty commit. Push existing local
commits if any; otherwise report that the repository is already synchronized.
For authentication failures or a non-fast-forward rejection, preserve the local
commit and explain the blocker. Do not automatically amend, reset, force-push,
merge, rebase, or modify authentication settings. On retry, inspect the state and
push the existing commit instead of creating a duplicate commit.
