# Shell Scratchpad Features

Shell Scratchpad is a KDE/Linux desktop app for writing, checking, and running Bash scripts from a focused editor.

## Script editing

- Edit multiline Bash scripts with line numbers, current-line highlighting, undo/redo, find, a dark code background, and customizable syntax colors.
- Highlight common Bash syntax as you type, including comments, strings, variables, keywords, commands, assignments, options, paths, operators, and redirections. The tokenizer is lightweight and best-effort rather than a complete Bash parser.
- Fold common control-flow blocks and here-documents. Collapsed sections show an inline artifact for selecting the hidden text or placing the caret before or after it.
- Insert a blank line above the current line with **Ctrl+Enter**, or below it with **Shift+Enter**. Zoom the editor with the View menu; use **Ctrl+Y** to redo.
- Customize syntax colors, including the here-document background, with **Tools → Code Style…**. The separate Syntax Debugger remains available for inspecting tokenizer results.

## Suggestions and snippets

- Get context-aware suggestions for Bash keywords, builtins, executable commands, environment and script variables, special parameters, and common command options. Command suggestions include descriptions when available.
- Create Bash snippets from selected editor text. Each snippet has a unique name and description and is saved as a single JSON file named after the snippet in the snippets folder.
- Manage snippets with **Tools → Snippets…**: edit names and descriptions, mark or remove highlight points, reload from disk, open the snippets folder, or delete a snippet with confirmation.
- Preview snippet code with syntax highlighting. ShellCheck syntax errors prevent a snippet from being saved.
- When an inserted snippet has highlight points, the editor selects them in order. Press **Tab** or **Enter** to advance, or **Escape** to stop.

## Running scripts

- Run the full script with **Alt+Enter** or **Script → Run Script**; run selected text with **Script → Run Selection**. Stop a running script with **Shift+Alt+Enter**.
- Choose a working directory directly, browse for one, or select a saved run profile with its own working directory and environment overrides. Opened script files use the default profile unless a profile has been saved for that file; changing back to Default removes its saved override.
- Run scripts through Bash without creating a temporary script file. Output streams live in the app, while run state, elapsed time, and exit status appear in the status area.
- Choose whether output clears at the start of each run. Stop a running process from the Script menu or the Stop control.
- Handle common Bash `read` prompts in dialogs, including masked input for `read -s`, and detect some prompts from external commands. Optional GUI sudo prompts keep passwords out of output and support reuse for the current run.
- Inspect top-level commands from the latest run in the separate **Tools → Run History…** window.

## Output

- View combined standard output and standard error as a script runs.
- Filter large output by matching lines as you type, with matching characters highlighted.
- Copy selected output with **Ctrl+C** or the Copy Output control; with no selection, the control copies all output.
- Save selected output lines as bookmarks. Bookmarks persist across runs and can be opened, removed, or searched in the current output.
- Clear output independently from the script. Output text remains free of synthetic run-status messages.

## Script files and saved scripts

- Create scratch scripts, open `.sh` files, save file-backed scripts, or pass a script path on the command line. **Ctrl+S** saves an opened file and does nothing for an untitled scratch script.
- Modified file-backed scripts prompt before replacement or close. Untitled scratch scripts do not prompt to save. Recovery drafts are saved automatically and can be restored on startup.
- Save reusable scripts in the Quick Saved Scripts window. User scripts appear separately from built-in system diagnostic scripts, and user entries can be deleted.
- Keep a single Latest Draft entry for the most recent unsaved draft. Opening a new script clears the output and its filter.
- Use the built-in Bash diagnostics for boot logs, graphics and kernel details, NVMe/SMART health, network and DNS, and memory and disk usage. They are ordinary editable scripts.

## Checks and run profiles

- Run ShellCheck after a short pause while editing and before execution. Findings appear in the dockable diagnostics list and can navigate to their source lines. ShellCheck is also used to reject invalid snippet syntax.
- Save and select run profiles containing a working directory and environment overrides.

## Menus, windows, and KDE integration

- Use File, Edit, Script, View, Tools, Window, and Help menus for script actions, editor commands, layout, zoom, tools, window navigation, shortcuts, and About information.
- Open multiple independent app windows and switch between them. Use **Ctrl+M**, then **Ctrl+O** to collapse all sections or **Ctrl+P** to expand them; the status bar indicates when a follow-up key is expected.
- The About dialog identifies WinterRose as the company and MPL-2.0 as the license.
- Install a KDE application launcher and Dolphin context-menu actions for opening a directory in Shell Scratchpad or opening a `.sh` file directly.

## Setup

- Build and install with `setup.sh` or build manually with CMake and Qt 6.6 or newer.
- The setup script supports Fedora, Debian/Ubuntu-based distributions, openSUSE, and Arch-based distributions. It installs the app, icon, desktop entry, and Dolphin service menus into the user's local environment.
