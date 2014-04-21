cmake_minimum_required (VERSION 2.8) 
project (eeprom-manager)

# Add debug flags if necessary
set(DEBUG_FLAGS "-Wall -Wextra -ggdb")
IF("Debug" MATCHES "${CMAKE_BUILD_TYPE}")
	set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${DEBUG_FLAGS}")
ENDIF()

set(LIBRARY_OUTPUT_PATH ${PROJECT_BINARY_DIR}/lib)

# Specify the files belonging to the DSP algorithm library
set(SOURCES eeprom-manager.c main.c)
# add_library(eeprom-manager SHARED ${SOURCES})
add_executable(eeprom-manager ${SOURCES})

target_link_libraries(eeprom-manager crypto ssl jansson)

# Define the BUILD, SO Version for library
# set(VERSION_MAJOR "1")
# set(VERSION_MINOR "1")
# set(SO_VERSION ${VERSION_MAJOR})
# set(BUILD_VERSION ${VERSION_MINOR})
# set(VERSION_STRING ${SO_VERSION}.${BUILD_VERSION})

# Set build VERSION and SO_VERSION for target
#set_target_properties(eeprom-manager PROPERTIES VERSION ${VERSION_STRING} SOVERSION ${SO_VERSION} )

# Define the install directory for the library, define permisions
#set(LIB_INSTALL "${CMAKE_INSTALL_PREFIX}/lib/")
#set(LIB_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)

# Install Target
# By default the link and the library itself will be isntalled
#install(TARGETS eeprom-manager LIBRARY
#        DESTINATION ${LIB_INSTALL}
#        PERMISSIONS ${LIB_PERMISSIONS})

# Define directory for header file install
#set(LIBH_INSTALL "${CMAKE_INSTALL_PREFIX}/include/")
#set(LIBH_PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)

# Install target for library header file
#install(FILES eeprom-manager.h
#	DESTINATION ${LIBH_INSTALL}
#	PERMISSIONS ${LIBH_PERMISSIONS})
