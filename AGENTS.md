# AGENTS.md

Repository instructions for AI coding agents. These apply to every task in this
repo, not just the one that created this file.

## Git workflow: always use a pull request

- Never commit directly to `main`.
- For each change, create a new branch, commit there, push the branch, and give
  the user a GitHub link to open the pull request.
- Name branches after the change in short hyphenated form, e.g.
  `copy-song-builder-in-analyzer`.
- One logical change per branch. Keep drive-by edits out of it.
- Do not merge, rebase, force-push, or push to `main`. The user merges the PR.

## Commit messages

- Subject line: sentence case, imperative, no trailing period, matching the
  existing history, e.g. `Copy song_builder.exe from analyze.exe instead of
  after it exits`.
- Body: a short paragraph on why the change was made (the problem it fixes), not
  a list of what was edited. Wrap around 72 columns.
- Every commit keeps this attribution footer, unless the user explicitly asks to
  drop it:

  ```
  🤖 Generated with Codebuff
  Co-Authored-By: Codebuff <noreply@codebuff.com>
  ```

## Building the analyzer

- `analysis/` holds the C analyzer. Sources: `analyze.c`, `exporter.c`,
  `cumulative_transience.c`, `crossfade.c`.
- Windows build: `make analyze.exe` in `analysis/` with mingw-w64
  (`x86_64-w64-mingw32-gcc`) on PATH.
- `analysis/analyze.exe` and `analysis/song_builder.exe` are tracked in git on
  purpose. When C sources change, rebuild `analyze.exe` and commit the new
  binary together with the source change.
- A cloud sandbox has no Windows runtime, so a rebuilt `.exe` can be compiled but
  not executed there. Say so plainly instead of implying it was tested; a Linux
  build of the same sources can be used to exercise the logic.

## Reporting results

- Name what was actually verified (compiled, ran, which platform) and what was
  not, rather than summarizing it as "tested".
