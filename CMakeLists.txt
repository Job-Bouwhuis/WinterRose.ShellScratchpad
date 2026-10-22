cmake_minimum_required(VERSION 3.21)
project(ShellScratchpad VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 6.12 REQUIRED COMPONENTS Core Widgets)

add_library(shell-scratchpad-syntax STATIC
    src/syntax/SyntaxToken.h
    src/syntax/SyntaxTokenizer.h
    src/syntax/SyntaxTokenizer.cpp
)
target_include_directories(shell-scratchpad-syntax PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(shell-scratchpad-syntax PUBLIC Qt6::Core)
target_compile_options(shell-scratchpad-syntax PRIVATE -Wall -Wextra -Wpedantic)

add_executable(shell-scratchpad
    src/main.cpp
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
    src/snippets/SnippetManagerDialog.h
    src/snippets/SnippetManagerDialog.cpp
    src/snippets/SnippetCreateDialog.h
    src/snippets/SnippetCreateDialog.cpp
    src/syntax/SyntaxHighlighter.h
    src/syntax/SyntaxHighlighter.cpp
    src/syntax/SyntaxColorSettings.h
    src/syntax/SyntaxColorSettings.cpp
    src/syntax/SyntaxColorDialog.h
    src/syntax/SyntaxColorDialog.cpp
    src/syntax/SyntaxDebugDialog.h
    src/syntax/SyntaxDebugDialog.cpp
)
target_link_libraries(shell-scratchpad PRIVATE Qt6::Widgets shell-scratchpad-syntax)
target_compile_options(shell-scratchpad PRIVATE -Wall -Wextra -Wpedantic)

include(CTest)
if(BUILD_TESTING)
    add_executable(shell-scratchpad-syntax-tests
        tests/syntax_tokenizer_tests.cpp
    )
    target_link_libraries(shell-scratchpad-syntax-tests PRIVATE shell-scratchpad-syntax Qt6::Core)
    target_compile_options(shell-scratchpad-syntax-tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME syntax-tokenizer-tests COMMAND shell-scratchpad-syntax-tests)
endif()

include(GNUInstallDirs)
install(TARGETS shell-scratchpad RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES packaging/org.winterrose.shellscratchpad.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES packaging/shell-scratchpad.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)

install(FILES packaging/org.winterrose.shellscratchpad-servicemenu.desktop
        packaging/org.winterrose.shellscratchpad-script-servicemenu.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/kio/servicemenus)
