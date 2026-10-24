# Shell Scratchpad

A small Qt 6 desktop app for KDE/Linux that runs multiline Bash scripts from an editor. The script is kept in an anonymous in-memory file descriptor while it runs, so **execution does not create a temporary script file** and Bash can still receive input through standard input.

## Features

- Multiline Bash editor with a clear border
- Live combined stdout/stderr output
- Terminal-style output handles carriage-return progress updates, common cursor movement and erase controls, and ANSI text colors/styles
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
- A KDE/Dolphin **Actions → Clone Git Repository Here…** action opens a clone dialog and clones the entered repository into a new subfolder of the selected directory, then opens it in Git Workspace
- A KDE/Dolphin **Open Folder With → Shell Scratchpad** application association, installed by `setup.sh`, to open a directory in folder mode with the file browser
- A KDE/Dolphin context-menu action, **Open in Shell Scratchpad**, for `.sh` files; script paths can also be passed on the command line
- `Ctrl+Enter` runs the script while the editor is focused
- Common Bash `read` prompts (including `read -p`) request input in a dialog. Likely yes/no prompts use a Yes/No dialog, and other input uses a text field; `read -s` is masked.
- The **Output** panel has a **Send input** field that sends a line to the running process's standard input. It works with input-reading commands launched directly or through another shell, such as `sh -c 'read answer; echo "$answer"'`.
- Scripts that call `sudo` use a separate masked password dialog, including calls made from nested shells such as `sh -c`; the password is not printed in the output or passed as a command-line argument
- Output from external commands is also checked for likely prompts (for example `[Y/n]`) after a short quiet period
- Find in Output filters to matching lines while typing and highlights matching characters; Ctrl+F follows the focused editor or output pane
- Run state, exit code, and elapsed time appear in the app status area, outside the script output
- GUI sudo prompts and reuse of one password across the current run, including nested shell commands, can be controlled with checkboxes. When reuse is enabled, the app keeps the password in memory only until that run ends.
- Quick-save and delete user Bash scripts from the Script menu; built-in diagnostic scripts are shown separately and protected from deletion
- Keep one Latest Draft and clear old output whenever a new or existing script is opened
- Debounced ShellCheck findings appear in a dockable list and can navigate to the source line; `setup.sh` installs ShellCheck on supported distributions
- **Tools → Run History…** opens the latest run's top-level commands in a separate window, with `sudo` requests marked and kept out of normal output
- **Tools → Git Workspace…** opens a built-in repository manager with change status, per-file stage/unstage/discard actions, diffs, branch switching, commit history, fetch/pull/push, and a raw Git command console
- Git Workspace has its own application-menu entry and can be opened with `shell-scratchpad --git-workspace [repository-folder]`; it is another window mode of the same executable
- The Git command console uses the app's editor suggestions, including Git subcommands and command-specific option hints. Credential setup configures a Git credential helper, and entered tokens are handed directly to that helper instead of being stored in Shell Scratchpad settings
- Right-click a file or folder in the folder browser to add its repository-root-relative path to `.gitignore`, or remove the exact entry previously added there

## One-command setup

From the extracted project directory:

```bash
chmod +x setup.sh
./setup.sh
```

The script installs supported system build prerequisites, including Qt 6.12 or newer, builds the app, installs it into `~/.local`, and refreshes the KDE application cache when available. It supports Fedora, Debian/Ubuntu and derivatives, openSUSE, and Arch-based distributions. On SteamOS, it uses a Fedora Distrobox container for build dependencies and keeps the app and launcher in your home directory, so it does not need to make the host filesystem writable. SteamOS must have `distrobox` and its container engine (`podman` or `docker`) available. It also installs the Dolphin/KIO service menu under `~/.local/share/kio/servicemenus/`.

### Windows Subsystem for Linux 2

For an existing WSL 2 distribution with WSLg enabled, run `./setup-wsl2.sh` from a folder containing `clone-and-setup.sh`. It clones, builds, and installs the app, installs the Qt XCB runtime dependency, and creates a Windows Desktop shortcut, Start menu entry, and command launcher under `%LOCALAPPDATA%\Programs\Shell Scratchpad`. The app and Bash scripts continue to run inside WSL; Windows file paths passed to the launcher are translated to WSL paths. The Windows launcher is a `.cmd` entry point rather than a native Windows executable. Remove the integration and Linux installation with `./setup-wsl2.sh --uninstall`.

For a new WSL setup, keep `setup-from-scratch-wsl2.ps1`, `setup-wsl2.sh`, and `clone-and-setup.sh` together and run the PowerShell script as Administrator. It installs or updates WSL2, then lets you choose an installed WSL distribution or create a clean Ubuntu 24.04 instance named `shell-scratchpad-host`. It copies both Bash scripts into the chosen distribution and runs `setup-wsl2.sh`. That script calls its adjacent `clone-and-setup.sh`, which installs Git if needed, clones the app repository, installs build prerequisites, builds and installs the app, and then configures its Windows Desktop and Start menu shortcuts. If Windows requires a restart to enable WSL, restart and rerun the PowerShell script.

### Native Windows Git client

The native Windows build is a Git-only app. It reuses the same Git workspace source as the Linux application and starts directly in the Git workspace; the Bash scratchpad window and script execution flow are not included. It runs directly on Windows and does not require WSL. Install Git for Windows and a Qt 6.12+ MSVC build, then open a Visual Studio developer PowerShell and run:

```powershell
./package-windows.ps1 -QtRoot 'C:/Qt/6.12.0/msvc2022_64'
```

The script builds `shell-scratchpad-git.exe` and deploys the Qt runtime beside it in `build/windows-release`. The Git client expects Git for Windows on `PATH`; Git Credential Manager is recommended for HTTPS authentication. Git for Windows supplies `ssh`, `ssh-keygen`, and `ssh-add` for SSH operations.

## Manual build

Install a C++ compiler, CMake, and Qt 6.12 or newer Core and Widgets development files from your distribution's package manager. Then run:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
./build/shell-scratchpad
```

## Notes and limitations

- Scripts execute with your normal user permissions. This is a shell, not a sandbox: review commands before running them, especially commands using `sudo`, deleting files, or changing system settings.
- Saved scripts are made executable for owner, group, and others. Scripts are saved in the current working directory unless you explicitly choose another location.
- Standard input stays open while a script runs. Use **Send input** to provide lines to any command reading stdin, including commands started through `sh -c`. A command that reads stdin until EOF (for example, a bare `cat`) keeps waiting until it receives enough input; redirect it from a file or use `</dev/null` when it should see immediate EOF. The app also heuristically detects common external prompts and opens a dialog. Full-screen terminal programs that require a TTY are not supported by the pipe-based input/output panel.
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
