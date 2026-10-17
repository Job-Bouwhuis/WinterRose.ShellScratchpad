# Shell Scratchpad Features

Shell Scratchpad is a desktop application for writing and running Bash scripts.
It pairs a script editor with a live output panel and common desktop workflows.

## At a glance

- Runs Bash scripts from a graphical editor.
- Provides live syntax highlighting while editing.
- Lets you choose where the script runs.
- Shows output and run status in the application.
- Opens scripts and working directories from KDE Dolphin.
- Supports multiple independent application windows.

## Script editor

- Edit scripts in a multiline text editor.
- Use a dark gray background for the code area.
- Keep the rest of the interface styled by the active KDE theme.
- Show line numbers beside the script.
- Fold and unfold common Bash blocks from markers in the line-number gutter.
- Recognize `if`/`fi`, loop/`done`, `case`/`esac`, and brace-delimited blocks.
- Fold here-document sections from their `<<` opener through the matching delimiter.
- Fold nested blocks independently and retain inner folds when reopening an outer block.
- Show an artifact with the hidden line count at the end of each collapsed opening line.
- Click the artifact center to select the folded text it represents.
- Click its left edge to place the caret before the section, or its right edge to place it after.
- Type or press Enter at the after-fold caret to continue after the hidden section, including at file end.
- Highlight the current editor line.
- Zoom the editor text in and out.
- Reset the editor zoom to its default size.
- Use ordinary single-key shortcuts alongside registered multi-step key chords.
- See a status-bar prompt when a chord prefix is waiting for its follow-up key.
- While a chord is pending, suppress single-key shortcuts; a nonmatching follow-up cancels the chord and is consumed.
- Use normal cursor movement and text selection.
- Select text with the mouse or keyboard.
- Undo recent edits.
- Redo undone edits.
- Cut selected script text.
- Copy selected script text.
- Paste from the clipboard.
- Select the complete script.
- Search within the script using Find.
- Use the standard editor context menu.
- Apply syntax colors as text changes.
- Keep syntax analysis separate from the color palette.
- Classify common shell syntax without a full Bash parser.
- Highlight shell comments.
- Give here-document sections (`<<EOF` through `EOF`) a distinct configurable background.
- Highlight single-quoted strings.
- Highlight double-quoted strings.
- Recognize variable expansions inside double-quoted strings.
- Recognize ordinary variable expansions.
- Recognize special Bash parameters.
- Recognize escape sequences.
- Recognize command-substitution starts.
- Recognize arithmetic-expansion starts.
- Recognize shell operators.
- Recognize redirection operators.
- Classify shell keywords.
- Classify shell builtins.
- Classify words in command position.
- Classify assignment names.
- Classify command options.
- Classify path-like words.
- Classify ordinary arguments and values.
- Carry quote and heredoc context across lines where supported.
- Treat the tokenizer as a best-effort highlighter for common scripts.

## Syntax colors and debugging

- Open **Tools → Code Style…** to customize syntax colors.
- Choose a color for each supported token role.
- Choose a separate background color for here-document sections.
- Apply color changes to open script editors immediately.
- Restore the default syntax color palette.
- Store syntax color preferences in the user's application configuration.
- Keep syntax colors separate from the KDE widget theme.
- Open **Tools → Syntax Debugger…** to inspect tokenization.
- Load the current script into the debugger.
- Try example snippets in the debugger window.
- See the token count and recognized token roles.
- Edit debugger input and observe updated token analysis.
- Use the debugger to investigate tokenizer behavior.
- Refreshing command cache is present in Tools, but command lookup caching is not implemented yet.

## Script files and drafts

- Start a new script in a fresh window.
- Open an existing script through the file chooser.
- Open a script path supplied on the command line.
- Save the current script as a `.sh` file.
- Save scripts with executable permissions.
- Save into the selected working directory by default.
- Choose a different save location when needed.
- Detect edits to opened or previously saved files.
- Prompt before discarding changes to a file-backed script.
- Save, discard, or cancel when replacing a modified file-backed script.
- Save, discard, or cancel when closing a modified file-backed script.
- Avoid save prompts for never-saved scratch scripts.
- Avoid save prompts for sessions that were not opened from a script file.
- Autosave recovery drafts for unexpected application closure.
- Offer recovery of available drafts on startup.
- Keep recovered work separate from the original script file until saved.
- Use **Ctrl+S** to save an opened script in place.
- Leave **Ctrl+S** as a no-op while editing an untitled scratch script.
- Use the Save Script menu or control to choose a name and location for an untitled script.
- Save reusable plain Bash scripts from **Script → Save Quick Script…**.
- Browse user quick-saves above built-in system diagnostic scripts in one modal window.
- Delete saved user quick-scripts from the list while protecting built-in scripts and the Latest Draft.
- Open a listed script directly in the editor for inspection, modification, and execution.
- Keep a single automatically updated **Latest Draft** entry for the current unsaved draft.
- Clear old output and its filter whenever a new or existing script is loaded.

## Script diagnostics

- Run ShellCheck against Bash scripts after a 250 ms editing pause.
- Start a fresh ShellCheck check when a script is run.
- Review ShellCheck errors, warnings, and informational findings in the dockable Script Diagnostics list.
- Jump from a finding to its source line by activating the list entry.
- Continue editing and running scripts when ShellCheck is unavailable; the app reports that checks cannot run.

## Optional run history

- Open the latest run's command history from **Tools → Run History…** in a separate window.
- Keep command history out of the script output pane.
- Record top-level Bash commands as the script runs.
- Mark commands invoked through the app's `sudo` helper as privileged commands.
- Keep the history in memory for the current window and replace it when the next run starts.

## Built-in diagnostic scripts

- Inspect current and previous system boot journal entries.
- Review GPU devices, Mesa/OpenGL, Vulkan, and kernel details.
- List NVMe and block devices and request SMART health summaries when tools are installed.
- Inspect network interfaces, routes, DNS resolver configuration, and name lookups.
- Review memory pressure, swap, filesystem space, and inode usage.
- Edit each built-in diagnostic script as ordinary Bash before running it.

## Working directory

- Display the current working directory above the editor.
- Type a directory path directly.
- Browse for a directory with the directory chooser.
- Use the chosen directory as the script's process working directory.
- Initialize the directory to the home directory for a new session.
- Initialize the directory to the script's parent folder when opening a file.
- Set the starting directory with `--working-directory <directory>`.
- Expand `~` and `~/...` in a supplied working-directory path.
- Open the app on a selected directory from Dolphin.

## Running and stopping scripts

- Run the current script from the Script menu.
- Run the current script with **Alt+Enter**.
- Run with **Ctrl+Enter** while the editor has focus.
- Stop an active script from the Script menu or control.
- Stop an active script with **Shift+Alt+Enter**.
- Run using the selected working directory.
- Pass the script to Bash through an in-memory file descriptor.
- Avoid creating a temporary script file for execution.
- Keep the app responsive to normal interaction during script execution.
- Display the current run state in the application status area.
- Display the process exit code when a run finishes.
- Display the elapsed run time.
- Report when a process could not be started.
- Report whether a run succeeded, failed, or was stopped.
- Keep run feedback outside the script's output text.
- Choose whether output is cleared when starting a run.
- Clear output independently using the Script menu.
- Run scripts with the current user's permissions.

## Interactive input and sudo

- Detect common Bash `read` prompts.
- Show a dialog for common `read` input requests.
- Support `read -p` prompt text.
- Mask input for `read -s`.
- Use yes/no dialogs for likely yes/no questions.
- Use a text field for ordinary prompt responses.
- Detect some prompts from external commands after output pauses.
- Show a masked password dialog when a script invokes `sudo`.
- Keep the sudo password out of displayed script output.
- Avoid passing the sudo password as a command-line argument.
- Control GUI sudo prompts with the sudo prompt checkbox.
- Optionally reuse sudo authentication during the current run.
- Limit password reuse to the current script run.
- Clear run-scoped password state after the execution ends.
- Use command noninteractive options when a privileged command asks for additional input.
- Continue to allow scripts to run without GUI sudo prompts when that option is disabled.

## Output panel

- Display standard output as the script runs.
- Display standard error in the same output stream.
- Keep output text separate from the script editor.
- Select and copy a portion of the output.
- Copy all output when there is no selection.
- Use **Ctrl+C** to copy selected output when the output panel has focus.
- Preserve the text when copying; output styling does not alter copied content.
- Search output from the output find field.
- Filter displayed lines as the search text changes.
- Highlight matching characters in visible output lines.
- Clear the output panel without clearing the script.
- Show ordinary command output without adding synthetic run messages.
- Keep run status, exit code, and elapsed time in the app status area.
- Use the output splitter to adjust space between editor and output.
- Show or hide the output panel from the View menu.
- Reset the layout to its default arrangement.

## Menus and keyboard shortcuts

### File

- **New Script** — start a blank scratch script.
- Shortcut: **Ctrl+N**.
- **New Window** — open another application window.
- Shortcut: **Ctrl+Shift+N**.
- **Open Script…** — choose a script file to edit.
- Shortcut: **Ctrl+O**.
- **Save Script…** — save the current script.
- **Ctrl+S** saves an opened script in place and does nothing for an untitled script.
- **Close Window** — close the current application window.
- Shortcut: **Ctrl+W**.

### Edit

- **Undo** and **Redo** script edits.
- Redo is available with **Ctrl+Y**.
- **Cut**, **Copy**, **Paste**, and **Select All** operate on the focused editor.
- Use the common platform shortcuts for editing actions.
- **Find** opens search for the focused editor or output pane.
- **Ctrl+F** follows the focused editor or output pane.
- **Ctrl+S** saves a file-backed script and does nothing for an untitled script.

### Script

- Run the current script with **Alt+Enter**.
- Stop a running script with **Shift+Alt+Enter**.
- Clear the output panel.
- Clear the script editor.
- Set the working directory with a directory chooser.

### View

- Show or hide output.
- Reset the editor/output layout.
- Zoom in or out in the editor.
- Reset editor zoom.
- Collapse all sections with **Ctrl+M**, then **Ctrl+O**.
- Expand all sections with **Ctrl+M**, then **Ctrl+P**.
- View these shortcuts in the View menu and keyboard shortcut summary.
- After **Ctrl+M**, wait up to 1.8 seconds for the follow-up key.

### Tools

- Open code style preferences.
- Open the syntax tokenizer debugger.
- Refresh command cache (reserved; lookup caching is not implemented).

### Window

- Open a new window.
- Move to the next open window.
- Move to the previous open window.
- Choose a window from the window list.

### Help

- View the keyboard shortcut summary.
- View About Shell Scratchpad information.
- Identify WinterRose as the company.
- Identify MPL-2.0 as the license.

## KDE and Dolphin integration

- Install a Shell Scratchpad launcher in the KDE application menu.
- Use the app icon associated with the installed desktop entry.
- Add **Open Shell Scratchpad** to Dolphin directory context menus.
- Open the selected directory as the app's working directory.
- Add **Open in Shell Scratchpad** for `.sh` files.
- Open selected shell scripts directly in the app.
- Pass script paths to the application from the command line.
- Install Dolphin service menu files for the current user.
- Refresh the KDE service menu cache when the relevant utility is available.
- Keep the main application styled according to the KDE theme.

## Setup and installation

- Run `setup.sh` from the project directory.
- Install supported compiler, CMake, and Qt 6 build prerequisites.
- Support Fedora and Fedora-based distributions.
- Support Debian, Ubuntu, Linux Mint, and Pop!_OS.
- Support openSUSE distributions.
- Support Arch Linux, Manjaro, and EndeavourOS.
- Configure the project with CMake.
- Build the application.
- Run syntax tokenizer checks as part of setup.
- Install the application into the current user's `~/.local` directory.
- Install the desktop launcher and application icon.
- Install the Dolphin service menu entries.
- Use `./setup.sh --build-only` to build without replacing the current installation.
- Use CMake directly for manual development builds.
- Distribute the application under the Mozilla Public License 2.0.

## Current scope and limitations

- The app is intended as a personal Bash scratchpad, not a full IDE.
- Syntax highlighting uses a lightweight tokenizer and heuristics.
- Syntax analysis currently runs synchronously.
- The tokenizer does not implement the complete Bash grammar.
- Command recognition does not resolve every command from the system PATH.
- Aliases and functions are not fully resolved for highlighting.
- External prompt detection handles common cases, not every interactive program.
- Full-screen terminal applications are outside the prompt-dialog workflow.
- Standard output and standard error are combined in the output panel.
- Scripts are not sandboxed and can make changes allowed to the current user.
- Review commands carefully, especially commands that use sudo or modify files.
