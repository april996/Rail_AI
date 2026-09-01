get_filename_component(CABIN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../../cabin" ABSOLUTE)

include_directories(${PROJECT_SOURCE_DIR}
    ${CABIN_DIR}/include
)


 add_executable(sherpa-onnx-microphone-test1
    sherpa-onnx-microphone.cc
    microphone.cc
    ${CABIN_DIR}/src/CabinProtocol.cpp
    ${CABIN_DIR}/src/PttController.cpp
  )


target_link_libraries(sherpa-onnx-microphone-test1 portaudio_static sherpa-onnx-core  zmq   /usr/local/lib/libzmq_component.so  ${ZeroMQ_LIBRARIES}    pthread  
)


target_link_libraries(sherpa-onnx-microphone-test1 "-Wl,-rpath,${SHERPA_ONNX_RPATH_ORIGIN}/../lib")
target_link_libraries(sherpa-onnx-microphone-test1 "-Wl,-rpath,${SHERPA_ONNX_RPATH_ORIGIN}/../../../sherpa_onnx/lib")
  
install(
    TARGETS sherpa-onnx-microphone-test1 
    DESTINATION
      bin

  )
