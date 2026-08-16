
add_subdirectory(unittests)
add_subdirectory(integrationtests)
if(GLADIUS_ENABLE_OPENCL)
	add_subdirectory(apitests)
else()
	message(STATUS "OpenCL disabled - excluding API tests that require the legacy compute ABI")
endif()

