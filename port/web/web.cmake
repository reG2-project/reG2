# The web build (emcmake): the game as reg2.js and reg2.wasm, the importer as reg2_import.js and .wasm, and the page.
# Included from CMakeLists.txt with the gta2 target defined.
set(web ${CMAKE_CURRENT_LIST_DIR})
target_sources(gta2 PRIVATE ${web}/src/web_platform.cpp ${web}/src/web_input.cpp)
target_link_libraries(gta2 PRIVATE gta2gl bink SDL3::SDL3)
# The game's loop yields to the browser at one place (port_WebYield in GameWinMain's loop): Asyncify only
# instruments the functions on that stack (asyncify_only.json), so the rest of the code runs as compiled.
target_link_options(gta2 PRIVATE
    -sASYNCIFY -sASYNCIFY_IGNORE_INDIRECT -sASYNCIFY_ONLY=@${web}/asyncify_only.json
    -sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2 -sALLOW_MEMORY_GROWTH -sINITIAL_MEMORY=64MB -sSTACK_SIZE=2MB
    -sMODULARIZE -sEXPORT_NAME=createReg2 -sENVIRONMENT=web -sFORCE_FILESYSTEM -sINVOKE_RUN=0
    -sEXPORTED_RUNTIME_METHODS=FS,ENV,HEAPU8,callMain,ccall -sEXPORTED_FUNCTIONS=_main,_port_WebState,_port_WebPause,_port_WebPad,_port_WebCursorX,_port_WebCursorY,_port_WebPlayer,_port_WebTestCar,_port_WebMenu
    --js-library ${web}/src/web_library.js)
set_target_properties(gta2 PROPERTIES SUFFIX .js)

add_executable(reg2_import ${web}/src/import_main.cpp)
target_link_libraries(reg2_import PRIVATE gta2setup)
target_compile_options(reg2_import PRIVATE -fno-short-wchar)
target_link_options(reg2_import PRIVATE -sALLOW_MEMORY_GROWTH -sMAXIMUM_MEMORY=4GB -sMODULARIZE
    -sEXPORT_NAME=createReg2Import -sENVIRONMENT=worker -sFORCE_FILESYSTEM -lworkerfs.js -sINVOKE_RUN=0
    -sEXPORTED_RUNTIME_METHODS=FS,WORKERFS,ccall -sEXPORTED_FUNCTIONS=_main,_import_check,_import_install,_import_done,_import_total)
set_target_properties(reg2_import PROPERTIES SUFFIX .js)
# The Khronos headers the renderer declares its OpenGL entry points with.
target_include_directories(gta2 PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../renderer/khronos)
target_include_directories(gta2gl PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../renderer/khronos)
set_target_properties(gta2 PROPERTIES LINK_DEPENDS "${web}/src/web_library.js;${web}/asyncify_only.json")
