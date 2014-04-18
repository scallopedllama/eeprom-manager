#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include "eeprom-manager.h"

int verbosity = 1;

#define WARN(format,args...) do { \
	if (verbosity) { \
		printf("WARNING: "); \
		printf(format, ## args); \
} while(0)

#define ERROR(format,args...) do { \
	if (verbosity) { \
		printf("ERROR: "); \
		printf(format, ## args); \
} while(0)

#define INFO(format,args...) do { \
if (verbosity) { \
	printf(format, ## args); \
	} while(0)

void usage()
{
	fprintf(stderr, "Usage: %s [arguments] (operation) [operation arguments]\n"
	                "Manages JSON-encoded non-volatile data stored in EEPROM(s).\n\n"
	                "\n"
	                "Operations:\n"
	                "\tread (key)              - Read value from key in EEPROM\n"
	                "\tset  (key) (value)      - set value to key in EEPROM\n"
	                "\tclear                   - Erase all data from EEPROM\n"
	                "\tverify                  - Verify EEPROM integrity\n"
	                "\n"
	                "Arguments:\n"
	                "\t-q         - Supress all output except for read value (no output without -r).\n"
	                "\t-z         - Fills the EEPROM with zeros before writing (slow, write only).\n"
	                "\t-n         - Do not create key on EEPROM if not present (write only).\n"
	                "\t-v         - Enable verbosity in eeprom-manager.\n"
	                "\n"
	                "Exit Value\n"
	                "Exits 0 when a value has been succcessfully written / read to / from EEPROM,\n"
	                "exits 2 when a key was not present in the EEPROM,\n"
	                "exits 3 when all EEPROM devices report corrupted SHA256,\n\n", argv[0]);
	exit(1);
}


/**
 * Sets key to value in EEPROM
 * @return value returned by eeprom_manager_set
 */
int set(char *key, char *value, int zero, int no_add)
{
	int r, err;
	int flags = (zero ? EEPROM_MANAGER_SET_ZERO : 0) | (no_add ? EEPROM_MANAGER_SET_NO_CREATE : 0);
	r = eeprom_manager_set_value(key, value, strlen(value), flags);
	err = errno;
	if (!r)
		ERROR("Failed to set value in EEPROM: %s\n", strerror(err));
	else
		INFO("Set value for key %s to %s.\n", key, value);
	return r;
}


/**
 * Reads key from EEPROM
 * @return value returned by eeprom_manager_read
 */
int read(char *key)
{
	int r, err;
	char value[EEPROM_MANAGER_MAX_VALUE_LENGTH];
	r = eeprom_manager_read_value(key, value, sizeof(value));
	err = errno;
	if (!r)
		ERROR("Failed to read from EEPROM: %s\n", strerror(err));
	else
	{
		INFO("Read value for key %s: ", key);
		printf("%s\n", value);
	}
	return r;
}


/**
 * Clears EEPROM contents and prints results
 * @return value returned by eeprom_manager_clear
 */
int clear()
{
	int r, err;
	r = eeprom_manager_clear();
	err = errno;
	if (!r)
		ERROR("Failed to clear EEPROM: %s\n", strerror(err));
	return r;
}


/**
 * Verifies EEPROMs and prints results
 * @return value returned by eeprom_manager_verify
 */
int verify()
{
	int r, err;
	r = eeprom_manager_verify();
	err = errno;
	switch(r)
	{
		case 2:
			INFO("One or more EEPROMs did not pass verification but have since been corrected.\n");
			INFO("Everything is ok.\n");
			break;
		case 1:
			INFO("All EEPROMs passed verification.\n");
			break;
		case 0:
			ERROR("All EEPROMs failed verification.\n");
			break;
		case -1:
			ERROR("Failed to check EEPROM: %s\n", strerror(err));
			break;
		default:
			ERROR("Unknown eeprom-manager error: %s\n", err);
			break;
	}
	return r;
}


/**
 * Main function
 * 
 * Parses arguments and performs relevant actions
 * @return return from respective action
 */
int main(int argc, char **argv)
{
	int zero = 0, no_add = 0, eeprom_manager_verbosity = 1;
	
	while ((c = getopt (argc, argv, "qznv")) != -1)
	{
		switch (c)
		{
			case 'q':
				verbosity = 0;
				break;
			case 'v':
				eeprom_manager_set_verbosity(eeprom_manager_verbosity++);
				break;
			case 'z':
				zero = 1;
				break;
			case 'n':
				no_add = 1;
				break;
			default:
				usage();
		}
	}
	
	if (strcmp(argv[optind], "set") == 0)
		return set(argv[optind + 1], argv[optind + 2], zero, no_add);
	else
	{
		if (zero)
			WARN("Ignoring argument -z\n");
		if (no_add)
			WARN("Ignoring argument -n\n");
		
		if (strcmp(argv[optind], "read") == 0)
			return read(argv[optind + 1]);
		else if (strcmp(argv[optind], "clear") == 0)
			return clear();
		else if (strcmp(argv[optind], "clear") == 0)
			return verify();
		else
		{
			ERROR("Unrecognized operation %s\n\n", argv[optind]);
			usage();
		}
	}
	
	return 0;
}
