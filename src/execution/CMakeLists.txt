set(SOURCES execution_manager.cpp executor_explain_analyze.cpp)
add_library(execution STATIC ${SOURCES})

target_link_libraries(execution system record transaction planner)
