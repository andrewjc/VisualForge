if(NOT EXISTS "${PLUGIN}")
    message(FATAL_ERROR "VisualForge plugin does not exist: ${PLUGIN}")
endif()

if(NOT EXISTS "${DUMPBIN}")
    message(FATAL_ERROR "dumpbin does not exist: ${DUMPBIN}")
endif()

execute_process(
    COMMAND "${DUMPBIN}" /nologo /exports "${PLUGIN}"
    RESULT_VARIABLE dump_result
    OUTPUT_VARIABLE dump_output
    ERROR_VARIABLE dump_error
)

if(NOT dump_result EQUAL 0)
    message(FATAL_ERROR "dumpbin failed (${dump_result}): ${dump_error}")
endif()

foreach(expected_count IN ITEMS "2 number of functions" "2 number of names")
    string(FIND "${dump_output}" "${expected_count}" count_position)
    if(count_position EQUAL -1)
        message(FATAL_ERROR "Unexpected plugin export count; expected '${expected_count}'\n${dump_output}")
    endif()
endforeach()

foreach(required_export IN ITEMS F4SEPlugin_Load F4SEPlugin_Version)
    string(FIND "${dump_output}" "${required_export}" export_position)
    if(export_position EQUAL -1)
        message(FATAL_ERROR "Missing required export ${required_export}\n${dump_output}")
    endif()
endforeach()
