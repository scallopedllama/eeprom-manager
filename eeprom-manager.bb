SUMMARY = "Library and Utility for managing settings on EEPROM(s)"
DESCRIPTION = "EEPROM Manager is a simple library and utility that \
can be used to safely manage settings stored on EEPROM devices in \
Linux. It supports redundant storage and error recovery."
SECTION = "libs"
LICENSE    = "LGPLv2.1"

DEPENDS           = "openssl jansson"
RDEPENDS_${PN}    = ""

SRC_URI = "git://git@github.com:scallopedllama/eeprom-manager.git;tag=eeprom-manager-1.0.y;protocol=ssh;user=git \
           file://eeprom-manager.conf "
S = "${WORKDIR}/git"
LIC_FILES_CHKSUM = "file://LICENSE;md5=4fbd65380cdd255951079008b364516c"

inherit cmake pkgconfig

do_install_append(){
	install -d ${D}${sysconfdir}
	install -m 0755 ${WORKDIR}/eeprom-manager.conf ${D}${sysconfdir}
}
