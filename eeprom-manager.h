#ifndef _EEPROM_MANAGER_H
#define _EEPROM_MANAGER_H

#include <openssl/sha.h>

#define EEPROM_MANAGER_STR(s)            EEPROM_MANAGER_XSTR(s)
#define EEPROM_MANAGER_XSTR(s)           #s
#define EEPROM_MANAGER_MAX_KEY_LENGTH    (100)
#define EEPROM_MANAGER_MAX_VALUE_LENGTH  (300)
#define EEPROM_MANAGER_PATH_MAX_LENGTH    100
#define EEPROM_MANAGER_MAX_RW_ATTEMPTS   (100)
#define EEPROM_MANAGER_SHA_STRING_LENGTH (SHA256_DIGEST_LENGTH * 2 + 1)
#define EEPROM_MANAGER_WC_STRING_LENGTH  (10 + 1)
#define EEPROM_MANAGER_MAGIC             "eepman"
#define EEPROM_MANAGER_METADATA_LENGTH   (EEPROM_MANAGER_SHA_STRING_LENGTH + EEPROM_MANAGER_WC_STRING_LENGTH + strlen(EEPROM_MANAGER_MAGIC))
#define EEPROM_MANAGER_CONF_PATH         "/etc/eeprom-manager.conf"

#define EEPROM_MANAGER_SET_NO_CREATE (1 << 0)

// ERRORS
#define EEPROM_MANAGER_ERROR_NO_GOOD_DEVICES_FOUND (-2)

#define EEPROM_MANAGER_ERROR_METADATA_BAD_MAGIC   (-3)

#define EEPROM_MANAGER_ERROR_CHECKSUM_FAILED      (-4)

#define EEPROM_MANAGER_ERROR_JSON_PARSE_FAIL (-5)
#define EEPROM_MANAGER_ERROR_JSON_ROOT_NOT_OBJECT (-6)
#define EEPROM_MANAGER_ERROR_JANSSON_ERROR (-7)
#define EEPROM_MANAGER_ERROR_JSON_READ_KEY_NOT_FOUND (-8)
#define EEPROM_MANAGER_ERROR_JSON_READ_KEY_NOT_STRING (-9)
#define EEPROM_MANAGER_ERROR_WRITE_KEY_NOT_FOUND  (-10)
#define EEPROM_MANAGER_ERROR_WRITE_JSON_TOO_LONG (-11)

/**
 * EEPROM metadata structure
 * 
 * There is one of these for each EEPROM device specified in the
 * config file in a linked list starting at first_eeprom and ending at last_eeprom.
 */
struct eeprom {
	char path[EEPROM_MANAGER_PATH_MAX_LENGTH];     /**< Path to the EEPROM device */
	size_t bs;                                     /**< Block size to write (specified by EEPROM driver) */
	size_t count;                                  /**< Number of blocks that can be written */
	int fd;                                        /**< File descriptor number for the opened file (0 if closed) */
	char sha256[EEPROM_MANAGER_SHA_STRING_LENGTH]; /**< SHA256 for data on device. */
	unsigned int wc;                               /**< Device write count. */
	char *data;                                    /**< String data stored in EEPROM */
	
	struct eeprom *next;                           /**< Next eeprom in the list. NULL if last item. */
};


/**
 * Initializes EEPROM Manager
 * 
 * Loads the config file and parses its contents, building up the
 * metadata necessary to work correctly.
 * 
 * NOTE: All EEPROM Manager API Functions will BLOCK until a lock can be made
 *       on all the EEPROM devices. This means that if multiple programs are using
 *       EEPROM Manager, there could be a deadlock situation.
 * 
 * @return 0 on success, 
 *        -1 on system error, check errno
 *      < -1 for EEPROM Manager errors
 */
int eeprom_manager_initialize();


/**
 * Cleans up EEPROM Manager
 * 
 * Frees up data structures allocated during initialization
 * 
 * NOTE: All EEPROM Manager API Functions will BLOCK until a lock can be made
 *       on all the EEPROM devices. This means that if multiple programs are using
 *       EEPROM Manager, there could be a deadlock situation.
 */
void eeprom_manager_cleanup();


/**
 * Sets the value of key to value.
 * 
 * The specified key is set to the specified value.
 * Flags can be one or more of the following:
 * EEPROM_MANAGER_SET_NO_CREATE   - Do not create a new key if it does not exist
 * 
 * NOTE: key name and value are truncated at EEPROM_MANAGER_MAX_KEY_LENGTH
 *       and EEPROM_MANAGER_MAX_VALUE_LENGTH respectively
 * 
 * NOTE: All EEPROM Manager API Functions will BLOCK until a lock can be made
 *       on all the EEPROM devices. This means that if multiple programs are using
 *       EEPROM Manager, there could be a deadlock situation.
 * 
 * @param key the key to modify
 * @param value the value to set it to. If NULL, "" will be used.
 * @param flags bitfield of options to the set
 * @return 0 on success, 
 *        -1 on system error, check errno
 *      < -1 for EEPROM Manager errors
 */
int eeprom_manager_set_value(char *key, char *value, int flags);

/**
 * Reads the value of key and writes it into the value buffer, truncating
 * at length bytes.
 * 
 * NOTE: key name and value are truncated at EEPROM_MANAGER_MAX_KEY_LENGTH
 *       and EEPROM_MANAGER_MAX_VALUE_LENGTH respectively
 * 
 * NOTE: All EEPROM Manager API Functions will BLOCK until a lock can be made
 *       on all the EEPROM devices. This means that if multiple programs are using
 *       EEPROM Manager, there could be a deadlock situation.
 * 
 * @param key the key to retrieve
 * @param value the buffer into which the key's value should be written
 * @param length the length of the value buffer
 * @return 0 on success, 
 *        -1 on system error, check errno
 *      < -1 for EEPROM Manager errors
 */
int eeprom_manager_read_value(char *key, char *value, int length);

/**
 * Clears all EEPROM contents
 * 
 * DANGEROUS. Will erase all EEPROM contents without verification.
 * 
 * NOTE: All EEPROM Manager API Functions will BLOCK until a lock can be made
 *       on all the EEPROM devices. This means that if multiple programs are using
 *       EEPROM Manager, there could be a deadlock situation.
 * 
 * @return 0 on success, 
 *        -1 on system error, check errno
 *      < -1 for EEPROM Manager errors
 */
int eeprom_manager_clear();


/**
 * Verifies all EEPROM contents
 * 
 * Simply checks the SHA256 sums and returns their status
 * 
 * NOTE: All EEPROM Manager API Functions will BLOCK until a lock can be made
 *       on all the EEPROM devices. This means that if multiple programs are using
 *       EEPROM Manager, there could be a deadlock situation.
 * 
 * @return 2 when one or more EEPROMs did not pass but were corrected
 *         1 when all EEPROMS passed
 *         0 when no EEPROMS passed
 *        -1 on system error, check errno
 *      < -1 for EEPROM Manager errors
 */
int eeprom_manager_verify();


/**
 * Returns information about the loaded EEPROM devices
 * 
 * @return head of a linked list of loaded EEPROM data or NULL on error.
 */
struct eeprom * eeprom_manager_info();

#endif
