cmake_minimum_required(VERSION 3.21)
project(ShellScratchpad VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

function(enable_project_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

if(WIN32)
    find_package(Qt6 6.12 REQUIRED COMPONENTS Core Widgets)
else()
    find_package(Qt6 6.12 REQUIRED COMPONENTS Core Widgets DBus)
endif()

add_library(shell-scratchpad-syntax STATIC
    src/syntax/SyntaxToken.h
    src/syntax/SyntaxTokenizer.h
    src/syntax/SyntaxTokenizer.cpp
)
target_include_directories(shell-scratchpad-syntax PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(shell-scratchpad-syntax PUBLIC Qt6::Core)
enable_project_warnings(shell-scratchpad-syntax)

add_library(git-workspace STATIC
    src/git/GitWorkspaceWindow.h
    src/git/GitWorkspaceWindow.cpp
    src/git/RepositoryWorkspace.h
    src/git/RepositoryWorkspace.cpp
    src/git/RepositoryOperationCoordinator.h
    src/git/RepositoryOperationCoordinator.cpp
    src/git/BatchOperationsDialog.h
    src/git/BatchOperationsDialog.cpp
    src/output/TerminalOutput.h
    src/output/TerminalOutput.cpp
    src/util/FuzzyMatcher.h
    src/util/FuzzyMatcher.cpp
    src/editor/CodeEditor.h
    src/editor/CodeEditor.cpp
    src/completion/CompletionController.h
    src/completion/CompletionController.cpp
    src/snippets/SnippetStore.h
    src/snippets/SnippetStore.cpp
    src/syntax/SyntaxHighlighter.h
    src/syntax/SyntaxHighlighter.cpp
    src/syntax/SyntaxColorSettings.h
    src/syntax/SyntaxColorSettings.cpp
)
target_include_directories(git-workspace PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(git-workspace PUBLIC Qt6::Widgets shell-scratchpad-syntax)
enable_project_warnings(git-workspace)

if(NOT WIN32)
add_executable(shell-scratchpad
    packaging/git-icons.qrc
    src/language/BashExecutionHandler.h
    src/language/BashExecutionHandler.cpp
    src/main.cpp
    src/snippets/SnippetManagerDialog.h
    src/snippets/SnippetManagerDialog.cpp
    src/snippets/SnippetCreateDialog.h
    src/snippets/SnippetCreateDialog.cpp
    src/syntax/SyntaxColorDialog.h
    src/syntax/SyntaxColorDialog.cpp
    src/syntax/SyntaxDebugDialog.h
    src/syntax/SyntaxDebugDialog.cpp
)
target_link_libraries(shell-scratchpad PRIVATE Qt6::Widgets shell-scratchpad-syntax git-workspace)
enable_project_warnings(shell-scratchpad)
endif()

if(WIN32)
    # The Windows product is deliberately Git-only. It reuses the exact same
    # Git workspace implementation as the Linux app, with a small platform
    # launcher instead of compiling the Bash scratchpad entry point.
    add_executable(shell-scratchpad-git
        src/git/main.cpp
        packaging/git-icons.qrc
    )
    target_link_libraries(shell-scratchpad-git PRIVATE git-workspace)
    enable_project_warnings(shell-scratchpad-git)
    set_target_properties(shell-scratchpad-git PROPERTIES
        WIN32_EXECUTABLE TRUE
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}"
    )
endif()

if(NOT WIN32)
add_executable(git-credential-kwallet
    src/git/KWalletCredentialHelper.cpp
)
target_link_libraries(git-credential-kwallet PRIVATE Qt6::Core Qt6::DBus)
enable_project_warnings(git-credential-kwallet)
endif()

include(CTest)
if(BUILD_TESTING AND NOT WIN32)
    add_executable(shell-scratchpad-syntax-tests
        tests/syntax_tokenizer_tests.cpp
    )
    target_link_libraries(shell-scratchpad-syntax-tests PRIVATE shell-scratchpad-syntax Qt6::Core)
    enable_project_warnings(shell-scratchpad-syntax-tests)
    add_test(NAME syntax-tokenizer-tests COMMAND shell-scratchpad-syntax-tests)

    add_executable(shell-scratchpad-terminal-output-tests
        tests/terminal_output_tests.cpp
        src/output/TerminalOutput.h
        src/output/TerminalOutput.cpp
    )
    target_include_directories(shell-scratchpad-terminal-output-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(shell-scratchpad-terminal-output-tests PRIVATE Qt6::Widgets)
    enable_project_warnings(shell-scratchpad-terminal-output-tests)
    add_test(NAME terminal-output-tests COMMAND shell-scratchpad-terminal-output-tests)
    set_tests_properties(terminal-output-tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(shell-scratchpad-repository-workspace-tests
        tests/repository_workspace_tests.cpp
        src/git/RepositoryWorkspace.h
        src/git/RepositoryWorkspace.cpp
        src/git/RepositoryOperationCoordinator.h
        src/git/RepositoryOperationCoordinator.cpp
        src/git/BatchOperationsDialog.h
        src/git/BatchOperationsDialog.cpp
    )
    target_include_directories(shell-scratchpad-repository-workspace-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(shell-scratchpad-repository-workspace-tests PRIVATE Qt6::Widgets)
    enable_project_warnings(shell-scratchpad-repository-workspace-tests)
    add_test(NAME repository-workspace-tests COMMAND shell-scratchpad-repository-workspace-tests)
    set_tests_properties(repository-workspace-tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

include(GNUInstallDirs)
if(WIN32)
    install(TARGETS shell-scratchpad-git RUNTIME DESTINATION .)
else()
install(TARGETS shell-scratchpad RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(TARGETS git-credential-kwallet RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES packaging/org.winterrose.shellscratchpad.desktop
        packaging/org.winterrose.shellscratchpad-git.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES packaging/shell-scratchpad.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)

install(FILES packaging/org.winterrose.shellscratchpad-servicemenu.desktop
        packaging/org.winterrose.shellscratchpad-script-servicemenu.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/kio/servicemenus)
endif()
