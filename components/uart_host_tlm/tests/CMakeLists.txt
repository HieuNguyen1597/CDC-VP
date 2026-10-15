add_executable(test_uart_host_bridge test_uart_host_bridge.cpp)
target_link_libraries(test_uart_host_bridge PRIVATE
    cdc::components::uart_host_tlm
    cdc::components::uart2_tlm
    SystemC::systemc)
add_test(NAME uart_host_bridge COMMAND test_uart_host_bridge)
# Replay held until a start event (hold_until; fx1_soc releases its CPUs late).
add_test(NAME uart_host_bridge_hold COMMAND test_uart_host_bridge hold)
