# Shell Scratchpad

A small Qt 6 desktop app for KDE/Linux that runs multiline Bash scripts from an editor. The script is kept in an anonymous in-memory file descriptor while it runs, so **execution does not create a temporary script file** and Bash can still receive input through standard input.

## Features

- Multiline Bash editor with a clear border
- Live combined stdout/stderr output
- Output copying uses the selected text when a selection exists, otherwise it copies all output
- Script editor line numbers
- Syntax-aware folding for common `if`, loop, `case`, and brace-delimited blocks
- Collapsible here-document sections
- Multi-step shortcuts to collapse or expand all sections
- Run, Stop, and Copy Output controls
- **Save script** first asks for a script name and saves directly into the current working directory; the standard KDE file chooser opens only when you choose **Choose different save location…**
- Modified files that were opened or saved can be saved, discarded, or left open before replacement or close. Untitled scratch scripts never prompt to save. Modified drafts are autosaved for crash recovery.
- Saved scripts receive executable permissions for owner, group, and others (`chmod +x`-style), so they can be run directly
- **Ctrl+S** saves an opened script in place and does nothing for an untitled scratch script
- Working-directory picker and `--working-directory <directory>` command-line option
- A KDE/Dolphin context-menu action, **Open Shell Scratchpad**, installed by `setup.sh`; right-click a directory or a directory icon on the desktop to open the app with that directory selected
- A KDE/Dolphin context-menu action, **Open in Shell Scratchpad**, for `.sh` files; script paths can also be passed on the command line
- `Ctrl+Enter` runs the script while the editor is focused
- Common Bash `read` prompts (including `read -p`) request input in a dialog. Likely yes/no prompts use a Yes/No dialog, and other input uses a text field; `read -s` is masked.
- Scripts that call `sudo` use a separate masked password dialog; the password is not printed in the output and is not passed as a command-line argument
- Output from external commands is also checked for likely prompts (for example `[Y/n]`) after a short quiet period
- Find in Output filters to matching lines while typing and highlights matching characters; Ctrl+F follows the focused editor or output pane
- Run state, exit code, and elapsed time appear in the app status area, outside the script output
- GUI sudo prompts and reuse of the password for the current run can be controlled with checkboxes
- Quick-save and delete user Bash scripts from the Script menu; built-in diagnostic scripts are shown separately and protected from deletion
- Keep one Latest Draft and clear old output whenever a new or existing script is opened
- Debounced ShellCheck findings appear in a dockable list and can navigate to the source line; `setup.sh` installs ShellCheck on supported distributions
- **Tools → Run History…** opens the latest run's top-level commands in a separate window, with `sudo` requests marked and kept out of normal output

## One-command setup

From the extracted project directory:

```bash
chmod +x setup.sh
./setup.sh
```

The script installs supported system build prerequisites, builds the app, installs it into `~/.local`, and refreshes the KDE application cache when available. It supports Fedora, Debian/Ubuntu and derivatives, openSUSE, and Arch-based distributions. It also installs the Dolphin/KIO service menu under `~/.local/share/kio/servicemenus/`.

## Manual build

Install a C++ compiler, CMake, and Qt 6.6 or newer Widgets development files from your distribution's package manager. Qt 6.6 is required for the child-process setup used to execute scripts from an inherited in-memory file descriptor. Then run:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
./build/shell-scratchpad
```

## Notes and limitations

- Scripts execute with your normal user permissions. This is a shell, not a sandbox: review commands before running them, especially commands using `sudo`, deleting files, or changing system settings.
- Saved scripts are made executable for owner, group, and others. Scripts are saved in the current working directory unless you explicitly choose another location.
- Since standard input stays open for dialogs, a command that reads stdin until EOF (for example, a bare `cat`) can wait for more input; redirect it from a file or use `</dev/null` for commands that should see immediate EOF. The app heuristically detects common prompts from external programs when output pauses at a likely question; unusual prompts or full-screen terminal applications may not be detectable or supported.
- The `sudo` wrapper asks for a password through a masked dialog and supplies it using `sudo -S`. When calling commands that also need additional interactive input under `sudo`, use their noninteractive options when available (for example, `apt-get -y`) because `sudo` consumes the password from its own standard input.
- Output combines stdout and stderr so messages appear in one live stream.
- Execution uses Linux `memfd_create` plus an inherited file descriptor: the script text itself is not saved to a temporary file.
- The Dolphin directory action applies to directory items, including directory icons on the desktop; it does not add an action to the empty desktop background's context menu.

## Version 2.0 development: Bash syntax highlighting

The main Bash editor now applies syntax colors while you type. Its background is dark gray; the rest of the app continues to follow the KDE theme. The **Tools → Syntax Debugger…** window remains available for inspecting tokenizer tokens and loading example scripts. Syntax colors are stored in `~/.config/WinterRose/Shell Scratchpad/syntax-colors.ini`.

The menu bar provides File, Edit, Script, View, Tools, Window, and Help menus. It includes script file actions, standard editing and find commands, run/stop actions, output/layout/zoom controls, multiple-window navigation, keyboard shortcuts, and About information. **Tools → Code Style…** opens syntax color preferences; changes apply immediately to open editors. Ctrl+S saves an opened script and does nothing for an untitled scratch script.

The tokenizer recognizes comments, string delimiters/content, variable and special-parameter expansions, escapes, command/arithmetic substitution starts, shell operators/redirections, and context-classified words such as commands, builtins, keywords, assignments, options, paths, and arguments. Command classification is heuristic: it does not yet resolve executables from PATH, understand aliases or functions, or parse all Bash grammar. Token analysis runs synchronously for now. When enabled, sudo authentication uses a masked dialog; password reuse is limited to that script run.

Run the automated tokenizer checks after building:

```bash
ctest --test-dir build --output-on-failure
```

## License

Shell Scratchpad is distributed under the Mozilla Public License 2.0 (MPL-2.0). See [LICENSE](LICENSE) for the full license text.
