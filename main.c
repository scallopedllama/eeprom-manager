#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#include "eeprom-manager.h"

int verbosity = 1;

#define WARN(format,args...) do { \
	if (verbosity) { \
		printf("WARNING: "); \
		printf(format, ## args); \
	} \
} while(0)

#define ERROR(format,args...) do { \
	if (verbosity) { \
		printf("ERROR: "); \
		printf(format, ## args); \
	} \
} while(0)

#define INFO(format,args...) do { \
if (verbosity) \
	printf(format, ## args); \
} while(0)

// TODO: Print all feature

void usage(char *name)
{
	fprintf(stderr, "Usage: %s [arguments] (operation) [operation arguments]\n"
	                "Manages JSON-encoded non-volatile data stored in EEPROM(s).\n\n"
	                "\n"
	                "Operations:\n"
	                "\tread (key)              - Read value from key in EEPROM\n"
	                "\tset  (key) (value)      - set value to key in EEPROM\n"
	                "\tclear                   - Erase all data from EEPROM\n"
	                "\tverify                  - Verify EEPROM integrity\n"
					"\tinfo                    - Print EEPROM info\n"
	                "\n"
	                "Arguments:\n"
	                "\t-q         - Supress all output except for read value (no output without -r).\n"
	                "\t-n         - Do not create key on EEPROM if not present (write only).\n"
	                "\n"
	                "Exit Value\n"
	                "Exits 0 when a value has been succcessfully written / read to / from EEPROM,\n"
	                "exits 2 when a key was not present in the EEPROM,\n"
	                "exits 3 when all EEPROM devices report corrupted SHA256,\n\n", name);
	exit(1);
}


/**
 * Sets key to value in EEPROM
 * @return value returned by eeprom_manager_set
 */
int set_key(char *key, char *value, int no_add)
{
	int r, err;
	int flags = (no_add ? EEPROM_MANAGER_SET_NO_CREATE : 0);
	r = eeprom_manager_set_value(key, value, flags);
	err = errno;
	if (r >= 0)
		INFO("Set value for key %s to %s.\n", key, value);
	else if (r == -1)
		ERROR("Failed to set value in EEPROM: %s\n", strerror(err));
	else if (r < -1)
		ERROR("eeprom manager error.\n");
	return r;
}


/**
 * Reads key from EEPROM
 * @return value returned by eeprom_manager_read
 */
int read_key(char *key)
{
	int r, err;
	char value[EEPROM_MANAGER_MAX_VALUE_LENGTH];
	r = eeprom_manager_read_value(key, value, sizeof(value));
	err = errno;
	if (r >= 0)
	{
		INFO("Read value for key %s: ", key);
		printf("%s\n", value);
	}
	else if (r == -1)
		ERROR("Failed to read value in EEPROM: %s\n", strerror(err));
	else if (r < -1)
		ERROR("eeprom manager error.\n");
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
	if (r == -1)
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
			ERROR("Unknown eeprom-manager error: %d\n", err);
			break;
	}
	return r;
}


/** 
 * Prints infomration about the EEPROMs
 */
int info()
{
	// Should print info about configured EEPROMS:
	// 01: 512K EEPROM at /sys/bus/... using blocksize 256
	// 02: 512K EEPROM at /sys/bus/... using blocksize 256
	
	// Then stats about storing data inside:
	// Currently using 4532 of 512000 Bytes (0.08%)
	
	unsigned int i = 0;
	struct eeprom *eeprom_list_start = eeprom_manager_info();
	struct eeprom *current_eeprom = NULL;
	
	if (eeprom_list_start == NULL)
	{
		ERROR("Failed to get EEPROM info.\n");
		return -1;
	}
	
	INFO("Defined EEPROM devices. All sizes are in Bytes.\n");
	INFO("%4s\t%10s\t%5s\t%5s\t%" EEPROM_MANAGER_STR(EEPROM_MANAGER_PATH_MAX_LENGTH) "s\n", "#", "Size", "BS", "Count", "Path");
	for (current_eeprom = eeprom_list_start; current_eeprom != NULL; current_eeprom = current_eeprom->next)
		printf("%4u\t%10lu\t%5lu\t%5lu\t%" EEPROM_MANAGER_STR(EEPROM_MANAGER_PATH_MAX_LENGTH) "s\n", ++i, (current_eeprom->bs * current_eeprom->count), current_eeprom->bs, current_eeprom->count, current_eeprom->path);
	
	return 0;
}


/**
 * Main function
 * 
 * Parses arguments and performs relevant actions
 * @return return from respective action
 */
int main(int argc, char **argv)
{
	int r, ret = 0, c, no_add = 0;
	
	if (argc < 2)
		usage(argv[0]);
	
	while ((c = getopt (argc, argv, "qnh")) != -1)
	{
		switch (c)
		{
			case 'q':
				verbosity = 0;
				break;
			case 'n':
				no_add = 1;
				break;
			case 'h':
			default:
				usage(argv[0]);
		}
	}
	
	r = eeprom_manager_initialize();
	if (r == -1)
	{
		int err = errno;
		ERROR("Failed to initialize EEPROM Manager: %s.\n", strerror(errno));
		if (err == ENOENT)
			ERROR("Could not open config file at %s\n", EEPROM_MANAGER_CONF_PATH);
		ret = r;
	}
	if (r == EEPROM_MANAGER_ERROR_NO_GOOD_DEVICES_FOUND)
	{
		if (!verbosity)
			ret = r;
		else
		{
			int c;
			INFO("No EEPROM devices are initialized. Run Clear (Y/n)? ");
			c = getchar();
			if (c == 'y' || c == 'Y' || c == '\n')
				clear();
			else
				ret = r;
		}
	}
	
	if (ret == 0 && strcmp(argv[optind], "set") == 0)
		ret = set_key(argv[optind + 1], argv[optind + 2], no_add);
	else if (ret == 0)
	{
		if (no_add)
			WARN("Ignoring argument -n\n");
		
		// TODO: If there isn't any arguments for read, consider printing everything in key = value list
		//       This would allow for easy fast export to bash variables
		if (strcmp(argv[optind], "read") == 0)
			ret = read_key(argv[optind + 1]);
		else if (strcmp(argv[optind], "clear") == 0)
			ret = clear();
		else if (strcmp(argv[optind], "verify") == 0)
			ret = verify();
		else if (strcmp(argv[optind], "info") == 0)
			ret = info();
		else
		{
			ERROR("Unrecognized operation %s\n\n", argv[optind]);
			eeprom_manager_cleanup();
			usage(argv[0]);
		}
	}
	
	eeprom_manager_cleanup();
	return ret;
}
