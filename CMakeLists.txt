cmake_minimum_required (VERSION 2.8) 
project (eeprom-manager)

# Add debug flags if necessary
set(DEBUG_FLAGS "-Wall -Wextra -ggdb")
IF("Debug" MATCHES "${CMAKE_BUILD_TYPE}")
	set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${DEBUG_FLAGS}")
ENDIF()

# Add libeeprommanager library
add_library(libeeprommanager SHARED eeprom-manager.c)
target_link_libraries(libeeprommanager crypto ssl jansson)
set(LIBRARY_OUTPUT_PATH ${PROJECT_BINARY_DIR}/lib)

# Set library version
set(VERSION_MAJOR "1")
set(VERSION_MINOR "0")
set(SO_VERSION ${VERSION_MAJOR})
set(BUILD_VERSION ${VERSION_MINOR})
set(VERSION_STRING ${SO_VERSION}.${BUILD_VERSION})
set_target_properties(libeeprommanager PROPERTIES VERSION ${VERSION_STRING} SOVERSION ${SO_VERSION} )

# Install target for library
set(LIB_INSTALL "${CMAKE_INSTALL_PREFIX}/lib/")
set(LIB_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
install(TARGETS libeeprommanager LIBRARY
       DESTINATION ${LIB_INSTALL}
       PERMISSIONS ${LIB_PERMISSIONS})

# Install target for header
set(LIBH_INSTALL "${CMAKE_INSTALL_PREFIX}/include/")
set(LIBH_PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)
install(FILES eeprom-manager.h
	DESTINATION ${LIBH_INSTALL}
	PERMISSIONS ${LIBH_PERMISSIONS})

# Add the eeprom-manager executable
add_executable(eeprom-manager main.c)
target_link_libraries(eeprom-manager libeeprommanager)

# Install target for eeprom-manager executable
set(DIR_INSTALL "${CMAKE_INSTALL_PREFIX}/bin/")
set(EXE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
install(TARGETS eeprom-manager RUNTIME
        DESTINATION ${DIR_INSTALL}
        PERMISSIONS ${EXE_PERMISSIONS})