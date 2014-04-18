#ifndef _EEPROM_MANAGER_H
#define _EEPROM_MANAGER_H

#define EEPROM_MANAGER_MAX_KEY_LENGTH 100
#define EEPROM_MANAGER_MAX_KEY_LENGTH 300
#define EEPROM_PATH_MAX_LENGTH 100
#define MAX_RW_ATTEMPTS 100
#define EEPROM_MANAGER_CONF_PATH "/etc/eeprom-manager.conf"


#define EEPROM_MANAGER_SET_NO_CREATE (1 << 0)
#define EEPROM_MANAGER_SET_ZERO      (1 << 1)

/**
 * Set EEPROM Manager verbosity
 * 
 * Call this function with a value greater than 0 to enable
 * printing to stderr / stdout from within the EEPROM manager
 * library.
 * 0 is silent (default)
 * 1 is errors only
 * 2 is errors and warnings
 * 3 is errors, warnings, and info
 * 
 * @param level verbosity level.
 */
void eeprom_manager_set_verbosity(int level);

/**
 * Sets the value of key to value.
 * 
 * The specified key is set to the specified value.
 * Flags can be one or more of the following:
 * EEPROM_MANAGER_SET_NO_CREATE   - Do not create a new key if it does not exist
 * EEPROM_MANAGER_SET_ZERO        - Clear the EEPROM device before writing (very slow)
 * 
 * Note: key name and value are truncated at EEPROM_MANAGER_MAX_KEY_LENGTH
 *       and EEPROM_MANAGER_MAX_VALUE_LENGTH respectively
 * 
 * @param key the key to modify
 * @param value the value to set it to. If NULL, "" will be used.
 * @param flags bitfield of options to the set
 * @return 0 on success, -1 on failure (check errno).
 */
int eeprom_manager_set_value(char *key, char *value, int flags);

/**
 * Reads the value of key and writes it into the value buffer, truncating
 * at length bytes.
 * 
 * Note: key name and value are truncated at EEPROM_MANAGER_MAX_KEY_LENGTH
 *       and EEPROM_MANAGER_MAX_VALUE_LENGTH respectively
 * 
 * @param key the key to retrieve
 * @param value the buffer into which the key's value should be written
 * @param length the length of the value buffer
 * @return 0 on success, -1 on failure (check errno).
 */
int eeprom_manager_read_value(char *key, char *value, int length);

/**
 * Clears all EEPROM contents
 * 
 * DANGEROUS. Will erase all EEPROM contents without verification.
 * 
 * @return 0 on success, -1 on failure (check errno).
 */
int eeprom_manager_clear();


/**
 * Verifies all EEPROM contents
 * 
 * Simply checks the SHA256 sums and returns their status
 * 
 * @return 2 when one or more EEPROMs did not pass but were corrected
 *         1 when all EEPROMS passed
 *         0 when no EEPROMS passed
 *        -1 on error (check errno)
 */
int eeprom_manager_verify();

#endif
