idf_component_register(SRCS "main.c" "json_flat.c"
                    INCLUDE_DIRS ".")

# The status page lives in web/index.html; main/web_page.h is generated from it.
# The generator runs at CMake configure time, and CMAKE_CONFIGURE_DEPENDS makes
# any edit of index.html trigger a reconfigure (and therefore a rebuild).
set(WEB_HTML "${CMAKE_CURRENT_LIST_DIR}/../web/index.html")
set(WEB_GEN  "${CMAKE_CURRENT_LIST_DIR}/../tools/gen_web_page.py")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${WEB_HTML}" "${WEB_GEN}")

idf_build_get_property(python PYTHON)
execute_process(COMMAND "${python}" "${WEB_GEN}"
                WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/.."
                RESULT_VARIABLE web_gen_rc)
if(NOT web_gen_rc EQUAL 0)
    message(FATAL_ERROR "tools/gen_web_page.py failed (${web_gen_rc})")
endif()
