# AGENTS.md

Repository instructions for AI coding agents. These apply to every task in this
repo, not just the one that created this file.

## Communication 

- Be as concise, direct and to-the-point as possible.
- Address all the concerns of the user but do so in the least words possible. This is not so much about using incomplete sentences but about not being redundant or verbose. Don't explain things that aren't directly relevant, have already been explained or could reasonably be assumed to already be understood by the user.
- Use precise language. For example, if we've been discussing normalization, don't suddenly start calling the normalization "the divide". That's confusing. Our words are functional here, not poetic, and we're not going for high word count. 
- In general, the algorithms you're working with will be understood very well by the user at a high level. Your job is to handle the low-level implementation, which means that not every detail needs to be explained to the user. If the user does need more detail the user will explicitly ask. Humans cannot read things instantaneously like AIs, so it is much easier to just ask for more detail than to have to wade through a huge essay at every turn to find the relevant information. When it comes to corresponding with the user, brevity will be lauded, not thoroughness. Be thorough in your understanding of the project but brief in your explanations of it. 
- Don't describe things that haven't changed. That's just redundant and makes it sound like they have changed. If something is not mentioned then it can be assumed that it has not changed. This goes both ways and should be assumed by the agent as well as the user. 
- Testing and verification are important, of course, but the user doesn't need to be told about it. The user will assume if you submit something, you have tested it thoroughly enough to have verified it is working correctly. 

These things apply to messages sent to the user, reports and even comments in the code. 

- In regards to comments in the code, a human will almost never look at the code at all, so if you can just as easily understand the code by looking at it, there is no need to make a comment. Comments are for things that are not immediately obvious from just looking at the local code. This is one way to keep line count down which should be considered a goal.

Use markdown. 
- Bullet points help keep things organized.
- Put code (variables, functions, etc.) `in backticks`.

## The Code Itself 

- Make it clean, make it streamlined and make it elegant.
- Maximize efficiency wherever possible. 
- Absolutely **do not add things to the code that the user did not ask for**. This is a cardinal sin.
- It is a goal to limit line count wherever possible.
- Unused or redundant variables, functions, etc. can and should be removed. This should be done as a matter of course.
- The user does not use command line. The user will always want a command line-free way of operating things. Internally, if a `.bat` file needs to set a command line flag for an executable it's running, for example, that's fine as long as it's automatic from the user's perspective. 

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
- Don't dig through old repository states unless absolutely necessary. If something was deleted or rewritten it was for a reason. You can't assume old code or documents hold any relevant information to the current state. In fact, reading old obsolete files should be explicitly avoided so as not to taint your context. 

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
  `song_builder.exe` builds from `song_builder.c` and `cumulative_transience.c`;
  `flux_separator.exe` builds from `flux_separator.c`.
- Windows build: `make analyze.exe`, `make song_builder.exe`, and
  `make flux_separator.exe` in `analysis/` with mingw-w64
  (`x86_64-w64-mingw32-gcc`) on PATH.
- `analysis/analyze.exe`, `analysis/song_builder.exe`, and
  `analysis/flux_separator.exe` are tracked in git on purpose. Whenever the
  underlying C sources change, rebuild each affected `.exe` for Windows and commit the
  new binary together with the source change.
- A cloud sandbox has no Windows runtime, so a rebuilt `.exe` can be compiled but
  not executed there. A Linux
  build of the same sources can be used to exercise the logic.
