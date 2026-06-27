if(NOT EXISTS "${BACKEND}")
    message(FATAL_ERROR "VisualForge renderer backend does not exist: ${BACKEND}")
endif()

if(NOT EXISTS "${DUMPBIN}")
    message(FATAL_ERROR "dumpbin does not exist: ${DUMPBIN}")
endif()

execute_process(
    COMMAND "${DUMPBIN}" /nologo /exports "${BACKEND}"
    RESULT_VARIABLE dump_result
    OUTPUT_VARIABLE dump_output
    ERROR_VARIABLE dump_error
)

if(NOT dump_result EQUAL 0)
    message(FATAL_ERROR "dumpbin failed (${dump_result}): ${dump_error}")
endif()

foreach(expected_count IN ITEMS "1 number of functions" "1 number of names")
    string(FIND "${dump_output}" "${expected_count}" count_position)
    if(count_position EQUAL -1)
        message(FATAL_ERROR "Unexpected backend export count; expected '${expected_count}'\n${dump_output}")
    endif()
endforeach()

string(FIND "${dump_output}" "VFRenderer_QueryInterface" export_position)
if(export_position EQUAL -1)
    message(FATAL_ERROR "Missing required export VFRenderer_QueryInterface\n${dump_output}")
endif()
