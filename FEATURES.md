# Shell Scratchpad Features

Shell Scratchpad is a Linux desktop app for writing, checking, and running Bash scripts. The editor and the rest of the app follow the KDE theme; the code editor keeps its own dark background and syntax color settings.

## Write and navigate scripts

- Start with a new scratch script, open a `.sh` file, or pass a script path when launching the app. Save file-backed scripts with **Ctrl+S** or **File → Save Script**. Ctrl+S does nothing for an untitled scratch script.
- Create more than one independent window with **File → New Window**. The Window menu switches between open windows.
- Edit Bash in a monospace editor with line numbers, current-line highlighting, undo and redo, and four-space tab width. With multiple lines selected, **Tab** indents every line and **Shift+Tab** removes one indentation level.
- Insert a blank line above the current line with **Ctrl+Enter**, or below it with **Shift+Enter**.
- Find text with **Edit → Find** or **Ctrl+F**. The search bar appears above the editor. Enter and the next button find forward; Shift+Enter and the previous button find backward. Search wraps around, and **Options → Case sensitive** changes matching.
- Fold supported Bash blocks and here-documents from the editor. A collapsed section is represented by an inline artifact: select it to select the hidden text, or place the caret immediately before or after it to edit around the folded section. **Ctrl+M**, then **Ctrl+O** folds all sections; **Ctrl+M**, then **Ctrl+P** unfolds them. The status bar shows when the app is waiting for the second key.
- Click a line number, or press **F9** on a line, to toggle a breakpoint. Breakpoint line numbers appear red. Run normally to stop at set breakpoints, or use **Script → Start Debugging** to pause before the first command.
- While paused, the current line is highlighted. Use **F5** to continue, **F10** to step over, **F11** to step into a script-local function, **Shift+F11** to step out of one, or the corresponding Script menu actions. Hover over a variable while paused to inspect its live Bash declaration and value.
- The debugger uses Bash's command tracing hooks to stop between shell commands. It can step through Bash statements and script-local functions; external programs execute as individual commands and are not debugged internally.
- Right-click the editor for actions relevant to the clicked token or selection. Depending on context, you can paste, run code, run selected code, copy, create a snippet, open a command's man page, navigate to a declaration, or copy a line together with its diagnostics. Hold **Ctrl** over a navigable in-script symbol to highlight it, then click to jump to its declaration.
- A delayed hover tooltip shows available token descriptions. Typing dismisses it until typing pauses; editor focus loss hides suggestions and tooltips.

## Syntax, suggestions, and diagnostics

- Bash syntax is highlighted as you type, including comments, quoted strings, variables, keywords, commands, arguments, paths, operators, redirections, and here-documents. Here-documents use the string style and do not receive ordinary code suggestions.
- Change syntax colors and code style with **Tools → Code Style**. The highlighter recognizes common Bash syntax and shell context without trying to parse every Bash construct.
- Suggestions appear while typing and include Bash keywords and builtins, executable commands, command options, snippets, environment variables, script variables, functions, and special Bash parameters. Suggestions respect context, including variable expansion and the command position after `sudo`.
- Suggestion matching accepts exact and prefix matches first, then ranks reasonable fuzzy matches. Select an entry to insert it; command suggestions and other entries retain their descriptions where available.
- Environment variables from the selected run profile are included in variable suggestions. The editor also tracks declarations in the current script so suggestions and navigation can point back to their source.
- ShellCheck runs after edits pause and before a script runs. Its errors and warnings appear in the diagnostics list and can be opened at their source line. Errors and warnings receive distinct squiggly underlines; right-click a diagnostic line to copy the line and its diagnostics.
- When an unknown variable resembles a known script or environment variable, the app can report a suggested correction and offer a direct fix.
- Use **Tools → Refresh Command Cache** after changing the system's available commands. **Tools → Syntax Debugger** opens the tokenizer inspection window.

## Snippets

- Select code and choose **Create Snippet** from the editor context menu. Give the snippet a unique name and description; snippets are stored as individual JSON files in the snippets folder.
- Open **Tools → Snippets** to browse and manage your own snippets. The manager previews selected code with syntax highlighting and lets you edit its name and description, edit highlight points, reload the file from disk, open the snippets folder, or delete the snippet after confirmation.
- The manager also has a separate read-only view of built-in snippets. Built-ins and user snippets are both offered by the completion menu.
- ShellCheck syntax errors prevent invalid snippets from being saved.
- Highlight points mark editable fields. When a snippet is inserted, the editor selects those fields in order; press **Tab** or **Enter** to advance, or **Escape** to leave snippet navigation.
- Auto-change sections copy the confirmed text of a related highlight point to other places in the snippet. This is useful for repeating names, such as a function name in its body.

## Run scripts and profiles

- Run a full script with **Alt+Enter** or **Script → Run Script**. Run selected text with **Script → Run Selection**. Stop a running script with **Shift+Alt+Enter** or **Script → Stop Script**.
- Set the working directory in the field above the editor, use **Browse**, or select a saved run profile. Profiles hold a working directory and environment overrides. A `.sh` file uses the Default profile unless a profile has been saved for that file; selecting Default removes that file's saved profile choice.
- Choose **Script → Clear output each run** to control whether the previous output is removed before running. Output can also be cleared with **Script → Clear Output**.
- Run progress, elapsed time, and the final exit status appear in the app's status area. Output contains the program's output rather than extra run-status messages.
- The Script menu can show GUI password prompts for `sudo` and reuse a password for the duration of that run, so repeated sudo commands do not ask again. Passwords are not saved as part of a run profile.
- Shell Scratchpad can respond to common Bash `read` prompts with input dialogs, including masked input for `read -s`.
- Open **Tools → Run History** when needed to inspect top-level commands from the latest run, including commands executed through `sudo` and their source lines.

## Output

- Standard output and standard error are shown together in the output pane as the script runs.
- Focus the output pane and press **Ctrl+F** to open its filter bar. Typing filters the pane to lines containing the query and highlights matching characters. Enter and the arrow buttons move through matching lines without selecting their text; Shift+Enter moves backward. Use **Options → Case sensitive** to change matching, or close the bar to show all output again.
- Press **Ctrl+C** in the output pane to copy the selection, or all output if nothing is selected. The **Copy output** button follows the same rule.
- Select output and choose **Bookmark selection** to save it; with no selection, the current line is bookmarked. Bookmarks persist across runs and can be opened, removed, or searched in the current output from **Tools → Output Bookmarks**.

## Quick saved scripts and recovery

- Open **Script → Quick Saved Scripts** or press **Ctrl+Q** to browse reusable scripts. The window previews the selected script on the right.
- Save your own scripts to the list and remove them later. User scripts appear above a separate read-only list of built-in diagnostics for boot logs, graphics and kernel details, NVMe and SMART health, network and DNS, memory, and disk usage. Selecting an entry loads an ordinary Bash script into the editor so it can be inspected, changed, and run.
- The list includes one **Latest Draft** entry for the most recent unsaved draft.
- Opening a new script clears output. Modified saved files ask before being replaced or closed; untitled scratch scripts do not prompt to save. The app keeps recovery drafts and offers to restore an interrupted session on startup.

## Menus, appearance, and installation

- **File** creates, opens, saves, and closes scripts and windows. **Edit** contains standard editing commands and Find. **Script** runs scripts, controls output and sudo behavior, sets the working directory, and opens quick saved scripts. **View** controls output visibility, layout, folding, and zoom.
- **Tools** opens snippets, code style, syntax debugging, command-cache refresh, run history, and output bookmarks. **Window** switches among app windows. **Help** contains keyboard shortcuts and About information.
- The About window credits WinterRose and lists the MPL-2.0 license.
- `setup.sh` builds and installs the app, launcher icon, desktop entry, and Dolphin context-menu actions for opening a directory or a `.sh` file. The setup script supports Fedora, Debian/Ubuntu-based distributions, openSUSE, and Arch-based distributions.
