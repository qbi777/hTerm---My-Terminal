# hterm

## What's new in 0.3
* **Built-in code editor:** `edit demo.c` (alias `e demo.c`) opens a file in the pane with line numbers,
  syntax colours (C/C++/Java/JS/Go/Rust, Python/shell/YAML/Makefile), auto-indent and Tab = 4 spaces.
  **Ctrl+S** (or ⌘S) saves, **Esc** closes (warns if unsaved), **Ctrl+Q** discards and closes.
  Creating a new file works too: `edit new_file.c`, then Ctrl+S.
* `vi demo.c` / `nano demo.c` still open the real editor in a PTY. If the PTY cannot be created hterm
  falls back to the built-in editor automatically. (PTY output is now polled on a timer and the child is
  started with a prepared environment, which is more reliable inside a macOS GUI process.)
* **Quick Commands:** new *FILE EDITING* group (edit, vi, nano, touch, cat, less, head, tail, grep, wc, cp, mv, rm, diff)
  and a *VI KEYS* cheat-sheet group (click = explanation in the footer, right-click = copy).
* **Animated pixel scenes** (replace the old flame; auto-rotate every ~14 s, click = next, right-click = previous):
  FIRE · THUNDER · SEA · AIR · DARK CLOUDS · EYE OF HORUS · CROSS ON THE MOUNTAIN · PYRAMIDS · STAGE DANCER · RELATIVITY (E=MC² chalkboard)
  · CUSTOM (yours, see below).
* **Your own scene:** copy `examples/custom_scene.example.txt` to `~/.hterm/custom_scene.txt` and edit it.
  It is 40 lines x 64 characters; `.` is transparent, `0`-`9` and `a`-`f` pick a palette colour
  (0 black, 1 navy, 2 red, 3 green, 4 amber, 5 blue, 6 purple, 7 pale, 8 slate, 9 pink, a mint, b gold, c sky, d lilac, e cyan, f white).
  The sprite then appears as the last scene. (Use this for any character art you want to draw yourself.)

## What's new in 0.2
* **Editors work.** `vi new_file.c`, `vim`, `nvim`, `nano`, `less`, `man`, `top`/`htop`, `ssh`, `python`/`node` REPLs,
  `bash`/`zsh`, `sudo ...`, and `git commit` (opens your editor) now run inside a real PTY with a built-in
  xterm-256color emulator (`src/vt.c`: cursor addressing, colours, alt-screen, scroll regions, UTF-8, line drawing,
  arrow/function keys, window-resize → SIGWINCH). Quit the editor and you are back at the hterm prompt.
  Prefix any command with `pty ` to force this mode (e.g. `pty ./my_menu_program`).
  Output of non-fullscreen programs (e.g. `sudo ls`) is copied back into the scrollback when they exit.
* **Split panes.** ⌘D split right · ⌘⇧D split below · ⌘W close pane · ⌘[ / ⌘] previous/next pane.
  Fallbacks if macOS swallows ⌘: Ctrl+Shift+D (below) · Ctrl+Shift+R (right) · Ctrl+Shift+W (close).
  The buttons in the top bar do the same. Every pane has its own shell, directory, scrollback and PTY;
  the explorer, git panel and status bar follow the focused pane. New panes start in the focused pane's directory.
* Not yet: mouse selection/copy inside the full-screen view (paste with ⌘V / Ctrl+Shift+V works), scrollback inside it.

A terminal-styled developer workstation written in C + GTK3 (neon "hacker" theme).

* Left: terminal with prompt `user@host:dir (git-branch)$`, built-ins, Up/Down history.
* Right (scrolls if the window is short):
  * **System monitor** – CPU graph + per-core bars, memory/swap, task table with Running / Sleeping / Stopped status.
  * **File explorer** – current folder tree (lazy-expanding, item counts) + parent folder with sibling folders.
    *Click any folder* → hterm runs `cd '<abs path>'` in the terminal (paths with spaces are quoted), the prompt,
    both listings and the git panel refresh. Click the `↰ parent` row or press **Alt+↑** to go up.
    Double-click/Enter also works. The cwd of the terminal session is the single source of truth.
  * **Profile card** – animated red flame (heat-propagation fire, ~12 fps) instead of a portrait.
  * **Quick commands** – C/build, navigation, git, system utilities.
    *Click* = insert into the terminal input (never runs by itself), *double-click* = run,
    *right-click* = copy to clipboard; the footer says whether it was inserted / copied / executed.
    `rm app`, `make clean`, `git push` ask for confirmation. Commands with placeholders
    (`cd folder`, `git commit -m "message"`, …) are insert-only; `top`/`htop` need a real TTY and are insert-only.
  * **Git info** – real data from `git status --porcelain=v2`: repo, branch (detached HEAD shown), ahead/behind,
    remote + sanitised URL (credentials never shown), modified/staged/untracked counts, last commit,
    “Open remote ↗” button. Shows *Not a git repository* / *no remote* / *no upstream* states.
* Bottom: status bar (user, host, cwd, git branch, session state, history size, clock).

## Build
macOS (Homebrew):

    xcode-select --install          # compiler, once
    brew install gtk+3 pkg-config
    make && ./hterm

Debian/Ubuntu/Kali:

    sudo apt install build-essential pkg-config libgtk-3-dev
    make && ./hterm
    make test        # history + sysinfo unit tests (no GTK needed)

## Where commands are saved
`~/.hterm/history.log` (mode 0600), one line per command:

    <unix-ts> TAB <user> TAB <cwd> TAB <exit-status> TAB <command>

Limits live in `src/config.h`:
* `HIST_MAX_ENTRIES` (1000) – newest N commands kept.
* `HIST_MAX_BYTES` (256 KiB) – when the file passes this, it is rewritten to ~75% of the cap by dropping the oldest lines (atomic temp-file + rename).
* Blank commands, commands starting with a space, and consecutive duplicates are not saved.
* `history` lists them, `history -c` wipes memory and file.

## Layout
    src/history.[ch]   persistent, rotating history/log
    src/vt.[ch]        terminal emulator + PTY launcher (no GTK; unit-tested)
    src/sysinfo.[ch]   CPU / memory / process stats from /proc
    src/main.c         GTK UI, terminal behaviour, dashboard
    src/config.h       limits and UI constants

## Limits
Plain commands run via `/bin/sh -c` with stdin closed; anything interactive is routed to the PTY mode above
(auto-detected from the command name, or force it with `pty <cmd>`).
Stats: Linux reads `/proc` (`sysinfo.c`); macOS uses Mach/libproc (`sysinfo_mac.c`, only shows processes you own unless run as root).
