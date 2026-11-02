# AGENTS.md

Repository instructions for AI coding agents. These apply to every task in this
repo, not just the one that created this file.

## Git workflow: always use a pull request

- Never commit directly to `main`.
- For each change, create a new branch, commit there, push the branch, and give
  the user a GitHub link to open the pull request.
- Every push ends with two things handed to the user, without being asked:
  - a link they can click to open the pull request for the pushed branch, and
  - a zip file they can download holding every file the push changed or added,
    with paths relative to the repo root preserved. Save it inside the
    workspace under `artifacts/`, named after the branch, e.g.
    `artifacts/<branch-name>-changes.zip`, and link it. A zip cannot record a
    deletion, so name any files the push removed in the message, and list the
    zip's contents so the user can see nothing is missing.
- Name branches after the change in short hyphenated form, e.g.
  `copy-song-builder-in-analyzer`.
- Commit as often as the work needs, and keep committing to the same open branch
  even between merges of other pull requests. A fresh branch is required only
  when the work is a genuinely different change, not for every commit.
- One pull request per logical change. Keep unrelated drive-by edits out of it.
- Do not merge, rebase, force-push, or push to `main`. The user merges the PR.

## Commit messages

- Subject line: sentence case, imperative, no trailing period, matching the
  existing history, e.g. `Copy song_builder.exe from analyze.exe instead of
  after it exits`.
- Body: a short paragraph on why the change was made (the problem it fixes), not
  a list of what was edited. Wrap around 72 columns.
- Commits are a collaboration between the user and Codebuff, and every single
  commit credits both — no exceptions, including small follow-ups,
  documentation-only commits, `.exe` rebuild commits and amended commits.
  Credit means two things together, and a commit missing either one is wrong:
  1. the git **author** is the user's own GitHub identity, and
  2. the message body ends with the **`Co-Authored-By: Codebuff`** trailer below,
     which is the only place GitHub shows Codebuff's part (the commit list shows
     the author alone, so a missing trailer is invisible there and the commit
     reads as the user's own unaided work).

  ```
  🤖 Generated with Codebuff
  Co-Authored-By: Codebuff <noreply@codebuff.com>
  ```

- The repository's configured git identity is `Codebuff <noreply@codebuff.com>`
  (the Freebuff default), so pass the user's identity explicitly when committing:

  ```
  git -c user.name='To the Sun' \
      -c user.email='4305621+to-the-sun@users.noreply.github.com' \
      commit -m "$(cat <<'EOF'
  Subject line here

  Why the change was made.

  🤖 Generated with Codebuff
  Co-Authored-By: Codebuff <noreply@codebuff.com>
  EOF
  )"
  ```

- Do not commit as Codebuff alone, and do not use the user's identity without the
  co-author trailer; either one misattributes the commit. Change the credit only
  when the user explicitly asks.
- Check the commit you just made before you push anything:

  ```
  git log -1 --format='%an <%ae>%n%B'
  ```

  The first line must be the user's identity and the body must end with the two
  trailer lines. If the author is the user but the trailer is missing, or the
  author is Codebuff, the commit is misattributed: amend it (`git commit
  --amend`) until both are right, then push. Never treat the trailer as optional
  or assume it was added — verify it.
- If a misattributed commit has already been pushed, do not rewrite it with a
  force-push on your own. Tell the user which commit is affected and ask whether
  to amend and force-push that branch.

## Building the analyzer

- `analysis/` holds the C tools. `analyze.exe` builds from `analyze.c`,
  `exporter.c`, `cumulative_transience.c`, and `crossfade.c`;
  `flux_separator.exe` builds from `flux_separator.c`, `flux_shared.c`, and
  `flux_envelope.c`; `flux_report.exe` builds from `flux_report.c`,
  `flux_report_html.c`, and `flux_shared.c`.
- Windows build: `make analyze.exe` in `analysis/` with mingw-w64
  (`x86_64-w64-mingw32-gcc`) on PATH.
- `analysis/analyze.exe`, `analysis/song_builder.exe`,
  `analysis/flux_separator.exe`, and `analysis/flux_report.exe` are tracked in
  git on purpose. Whenever the underlying C sources change, rebuild each
  affected `.exe` for Windows (`make analyze.exe`, `make flux_separator.exe`,
  `make flux_report.exe` in `analysis/`) and commit the new binary together with
  the source change.
- A cloud sandbox has no Windows runtime, so a rebuilt `.exe` can be compiled but
  not executed there. Say so plainly instead of implying it was tested; a Linux
  build of the same sources can be used to exercise the logic.

## Reporting results

- Name what was actually verified (compiled, ran, which platform) and what was
  not, rather than summarizing it as "tested".
