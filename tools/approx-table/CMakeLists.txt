# approx-table: captures the rcpss table of the physics (phys::num::RcpApproxBits) and checks it against the running CPU.
add_executable(approx-table main.cpp)
target_link_libraries(approx-table PRIVATE phys-num)
target_compile_options(approx-table PRIVATE /W4 /permissive-)
target_compile_definitions(approx-table PRIVATE _CRT_SECURE_NO_WARNINGS)
