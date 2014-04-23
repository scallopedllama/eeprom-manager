#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#include <string.h>
#include <jansson.h>
#include <errno.h>
#include <openssl/sha.h>

#include "eeprom-manager.h"


// TODO: ALL THESE API CALLS AND SUCH MUST BE PROTECTED BY A SEMAPHORE TO BE THREAD SAFE!!
// TODO: Needs check to make sure all data fits in eeprom
// TODO: Add a level of caching to all this, need a function that checks the last block on all devices and uses previous json state if wc hasn't changed.
// TODO: Need mechanism to remove misbehaving eeprom from pool if it's failing to write and such
// TODO: Check all functions for situations where bad input could cause segfault and handle it.
// TODO: Add check for EEPROM write roll-over
// TODO: handle janson error returns
// TODO: for api functions, return < 0 indicates error. -1 is for errno, < -1 is eeprom-manager specific errors
// TODO: Make sure no error bubbling will cause a loop to exit when the error can be handled

int eeprom_manager_verbosity = 0;
size_t eeprom_data_size = 0;
int number_eeproms = 0;
struct eeprom *first_eeprom = NULL;
struct eeprom *last_eeprom = NULL;
struct eeprom *good_eeprom = NULL;
json_t *json_root = NULL;
json_error_t json_error;


/**
 * Frees eeprom data if it exists
 */
void free_eeprom_data(struct eeprom *device)
{
	if (device->data != NULL)
	{
		free(device->data);
		device->data = NULL;
	}
}


/**
 * Safely Clears the eeproms linked list
 */
void clear_eeprom_metadata()
{
	struct eeprom *current_eeprom = first_eeprom;
	while (current_eeprom != NULL)
	{
		struct eeprom *prev_eeprom = current_eeprom;
		current_eeprom = current_eeprom->next;
		free_eeprom_data(prev_eeprom);
		free(prev_eeprom);
		prev_eeprom = NULL;
	}
}

/**
 * Pushes a new eeprom metadata object onto the linked list
 */
void push_eeprom_metadata(struct eeprom *new_eeprom)
{
	new_eeprom->next = NULL;
	new_eeprom->fd = 0;
	
	if (first_eeprom == NULL)
		first_eeprom = last_eeprom = new_eeprom;
	else
	{
		last_eeprom->next = new_eeprom;
		last_eeprom = new_eeprom;
	}
	number_eeproms++;
}


/**
 * Calculates a sha256 checksum for a string
 * 
 * Provided a string buffer, calculates the sha256 sum
 * then stores the resultant sha256 sum as a string in
 * the passed variable.
 * 
 * @param data_buffer Text to checksum
 * @param sha256      Buffer to fill with checksum. Must be SHA_STRING_LENGTH in length.
 */
void get_sha256_string(char *data_buffer, char *sha256)
{
	unsigned char sha256_data[SHA256_DIGEST_LENGTH];
	int i;
	SHA256((unsigned char*)&data_buffer, strlen(data_buffer), sha256_data);
	for (i = 0; i < SHA256_DIGEST_LENGTH; i++)
		sprintf(sha256 + (i * 2), "%02x", sha256_data[i]);
}


/**
 * Clears bytes after the NULL byte
 * 
 * Will scan the buffer looking for a '\0' byte
 * and clears all subsequent bytes
 * 
 * @param buf    - Buffer to scan and clear
 * @param length - Length of buffer
 * @return Position of null byte or -1 if not found.
 */
int clear_after_null(char *buf, int length)
{
	int j, r = -1;
	for (j = 0; j < length; j++)
	{
		// Look for null byte
		if (*(buf + j) == '\0' && r < 0)
			r = j;
		// Clear bytes after the null terminator
		if (r >= 0)
			*(buf + j) = '\0';
	}
	return r;
}


/**
 * Write wrapper
 * 
 * Attempts to write the data to the fd a maximum MAX_RW_ATTEMPTS
 * and drops a warning if it does not manage to do that.
 * 
 * @param fd    File descriptor to write to
 * @param buf   Buffer to write from
 * @param count Number of bytes to write from buf to fd
 * @return number of bytes read / written on success, < 0 on error
 */
ssize_t read_write_all(struct eeprom *device, char op, void *buf, size_t count)
{
	ssize_t r = 0, ret = 0;
	unsigned int attempts = 0;
	
	if (op != 'r' && op != 'w')
	{
		errno = EINVAL;
		return -1;
	}
	
	while (ret < (ssize_t) count)
	{
		if (op == 'w')
			r = write(device->fd, buf + ret, count - ret);
		else
			r = read(device->fd, buf + ret, count - ret);
		
		// Catch read / write failures
		if (r < 0)
			return r;
		
		// Add bytes read to the return variable
		ret += r;
		
		// Have a maximum number of tries
		if (++attempts > EEPROM_MANAGER_MAX_RW_ATTEMPTS)
			break;
	}
	
	// Make sure it was all read or written
	if (attempts >= EEPROM_MANAGER_MAX_RW_ATTEMPTS)
	{
		fprintf(stderr, "ERROR: Attempted %d times to %s %lu bytes\n", EEPROM_MANAGER_MAX_RW_ATTEMPTS, (op == 'r' ? "read" : "write"), count);
		fprintf(stderr, "       but only managed to %s %lu bytes. Aborting.\n", (op == 'r' ? "read" : "write"), r);
		errno = EIO;
		return -1;
	}
	
	return ret;
}


/**
 * Reads or Writes Metadata for passed EEPROM device
 * 
 * Either loads the sha256 and wc from the EEPROM into the passed
 * eeprom struct or saves the sha256 and wc from the eeprom struct
 * into the EEPROM device.
 * When writing, the sha256 and wc variables must be set inside
 * the eeprom struct before calling this function.
 * 
 * If the EEPROM device appears to be uninitialized, it will return -EMEDIUMTYPE.
 * 
 * @param op     'r' or 'w' to Read or Write respectively
 * @param device EEPROM device to load metadata for
 * @return -1 on failure, 0 on success
 */
int read_write_eeprom_metadata(struct eeprom *device, char op)
{
	char buffer[EEPROM_MANAGER_WC_STRING_LENGTH];
	int r = 0;
	// Read the device->sha256 and device->wc at the end of the device
	lseek(device->fd, -1 * device->bs, SEEK_END);
	
	// Read the Magic
	r = read_write_all(device, 'r', buffer, strlen(EEPROM_MANAGER_MAGIC));
	if (r < 0)
		return r;
	if (strcmp(buffer, EEPROM_MANAGER_MAGIC) != 0)
		return EEPROM_MANAGER_ERROR_METADATA_BAD_MAGIC;
	
	// Read the SHA
	r = read_write_all(device, op, device->sha256, EEPROM_MANAGER_SHA_STRING_LENGTH);
	if (r < 0)
		return r;
	
	if (op == 'w')
		sprintf(buffer, "%010u", device->wc);
	
	r = read_write_all(device, op, buffer, EEPROM_MANAGER_WC_STRING_LENGTH);
	if (r < 0)
		return r;
	
	if (op == 'r')
		sscanf(buffer, "%010u", &(device->wc));
	
	return r;
}

/**
 * Reads or Writes the EEPROM contents
 * 
 * Either reads from the already-open fd writing into device data variable or writes
 * from the device data variable into the fd in bs chunks a maximum of count times,
 * stopping after the first null byte is encountered.
 * 
 * Also reads or writes the sha256 and write count from the end of the device.
 * Returns -EIO if it fails while trying to read or write the EEPROM.
 * Returns -EINVAL if op was not 'r' or 'w'.
 * 
 * @param op     'r' or 'w' to Read or Write respectively
 * @param device Loaded EEPROM structure with updated sha256 and wc variables
 * @return < 0 to indicate failure, else length of the JSON string read
 */
size_t read_write_eeprom(struct eeprom *device, char op)
{
	size_t i;
	int retval, r, null_found;
	char *pos = NULL;
	
	if ((op != 'r' && op != 'w') || (device == NULL))
	{
		errno = EINVAL;
		return -1;
	}
	
	// Make sure data is allocated
	if (op == 'r')
	{
		if (device->data == NULL)
			device->data = malloc(eeprom_data_size);
		if (device->data == NULL)
		{
			fprintf(stderr, "ERROR: Cannot allocate memory for EEPROM data.\n");
			errno = ENOMEM;
			return -1;
		}
	}
	else if (device->data == NULL)
	{
		errno = EINVAL;
		return -1;
	}
	
	// Set starting position
	pos = device->data;
	
	// Clear the buffer if reading
	if (op == 'r')
		memset(device->data, 0, eeprom_data_size);
	// Clear the last block if writing
	if (op == 'w')
	{
		char zero_block[device->bs];
		memset(zero_block, 0, device->bs);
		lseek(device->fd, -1 * device->bs, SEEK_END);
		r = read_write_all(device, 'w', zero_block, device->bs);
		if (r < 0)
			return r;
	}
	
	// Start at the beginning of the device
	lseek(device->fd, 0, SEEK_SET);
	
	// Read / write device->count blocks
	for (i = 0; i < device->count; i++)
	{
		null_found = 0;
		
		// Clear bytes after null if writing here
		if (op == 'w')
			null_found = clear_after_null(pos, device->bs);
		
		// Do read or write
		r = read_write_all(device, op, pos, device->bs);
		if (r < 0)
			return r;
		
		// If reading, clear bytes after null here
		if (op == 'r')
			null_found = clear_after_null(pos, device->bs);
		
		// Done if there was a null
		if (null_found >= 0)
			break;
		
		// Advance the position in the buffer
		pos += device->bs;
	}
	
	// Calculate return value
	if (null_found >= 0)
		retval = (i * device->bs) + null_found;
	else
		retval = (device->count * device->bs);
	
	// Read / write the device->sha256 and device->wc at the end of the device
	// TODO: This may not be required for reading, but is definitely required for writing
	r = read_write_eeprom_metadata(device, op);
	if (r < 0)
		return r;
	
	return retval;
}


/**
 * Writes data to EEPROM
 * 
 * Calculates the sha256 checksum for device->data
 * then writes that data to the provided eeprom device.
 * 
 * @param device EEPROM device to write to
 * @return < 0 on error, number of bytes written on success (can be 0, not error)
 */
int write_eeprom(struct eeprom *device)
{
	if (device->data == NULL)
	{
		errno = -EINVAL;
		return -1;
	}
	
	// Calculate sha256
	char sha256[EEPROM_MANAGER_SHA_STRING_LENGTH];
	get_sha256_string(device->data, sha256);
	
	// Don't write anything if the SHA hasn't changed
	if (strcmp(device->sha256, sha256) == 0)
		return 0;
	
	// Write data
	device->wc++;
	return read_write_eeprom(device, 'w');
}


/**
 * Writes same data from one eeprom to another
 * 
 * @param src eeprom device to get data from
 * @param dest eeprom device to write data to
 * @return return from write_eeprom
 */
int clone_eeproms(struct eeprom *src, struct eeprom *dest)
{
	// write_eeprom will only write data if sha has changed, so clear sha to make sure that happens
	dest->sha256[0] = '\0';
	// Make sure the written eeprom ends up with the same wc as the good eeprom
	dest->wc = src->wc - 1;
	dest->data = src->data;
	return write_eeprom(dest);
}


/**
 * Writes all eeprom data to eeproms from the list.
 * 
 * Iterates through the eeprom list, calling write_eeprom on each.
 * 
 * @param src eeprom from which data should be written to all devices. Use NULL to write what is in the eeprom device's data.
 * @return < 0 on error, total number of bytes written on success (can be 0, not error)
 */
int write_all_eeproms(struct eeprom *src)
{
	struct eeprom *d;
	int r, t = 0;
	for (d = first_eeprom; d != NULL; d = d->next)
	{
		if (src && src != d)
			r = clone_eeproms(src, d);
		else
			r = write_eeprom(d);
		if (r < 0)
			return r;
		t += r;
	}
	return t;
}


/**
 * Reads the contents of the eeprom and verifies the sha256 sum,
 * returing the write count on success. Frees read data if invalid.
 * 
 * @param device EEPROM device to verify
 * @return < 0 on non-match or error, 0 on success
 */
int verify_eeprom(struct eeprom *device)
{
	char sha256[EEPROM_MANAGER_SHA_STRING_LENGTH];
	
	int r = read_write_eeprom(device, 'r');
	if (r < 0)
		return r;
	get_sha256_string(device->data, sha256);
	
	if (strcmp(sha256, device->sha256) != 0)
	{
		// Don't keep invalid data around
		free_eeprom_data(device);
		return EEPROM_MANAGER_ERROR_CHECKSUM_FAILED;
	}
	return 0;
}


/**
 * Opens all EEPROM files
 * 
 * Iterates through the eeproms list and opens all the files then gets an advisory lock on them.
 * 
 * @return 0 on success, < 0 on error (check errno)
 */
int open_eeproms()
{
	int r;
	struct eeprom *d = first_eeprom;
	for (d = first_eeprom; d != NULL; d = d->next)
	{
		// TODO: Double-check error handling. This fails everything if one EEPROM has troubles
		// Open the file
		while((d->fd = open(d->path, O_RDWR | O_CLOEXEC)) < 0 && errno == EINTR);
		if (d->fd < 0)
			return -1;
		
		// Get an advisory lock on it
		while((r = flock(d->fd, LOCK_EX)) != 0 && errno == EINTR);
		if (r != 0)
			return -1;
	}
	return 0;
}


/**
 * Closes all EEPROM files
 * 
 * Iterates through the eeproms list and releases the advisory lock then closes the file.
 * 
 * @return 0 on success, -1 on error (check errno)
 */
int close_eeproms()
{
	int r;
	struct eeprom *d = first_eeprom;
	for (d = first_eeprom; d != NULL; d = d->next)
	{
		// Release advisory lock on file
		while((r = flock(d->fd, LOCK_UN)) != 0 && errno == EINTR);
		// More concerned with closing the file than unlocking it (since the close should unlock it too)
		
		// Close the file
		while((r = close(d->fd)) != 0 && errno == EINTR);
		if (r != 0)
			return -1;
		d->fd = 0;
	}
	return 0;
}


/**
 * Loads eeprom configuration data
 * 
 * Reads the EEPROM Manager configuration file and loads up a linked list
 * of eeprom device metadata.
 * 
 * @return < 0 on error, 0 on success
 */
int load_conf_data()
{
	FILE *config = NULL;
	size_t last_bs = 0, last_count = 0, min_size = 0;
	int first_config_eeprom = 1;
	
	// Load config file
	config = fopen(EEPROM_MANAGER_CONF_PATH, "r");
	if (config == NULL)
		return -1;
	
	// Parse config file
	while(!feof(config))
	{
		struct eeprom *new_eeprom = NULL;
		char path[EEPROM_MANAGER_PATH_MAX_LENGTH];
		size_t bs, size;
		
		// If fscanf fails to get 3 fields or if after getting 3 fields, the path starts with '#', skip this line
		if ((fscanf(config, "%s %lu %lu\n", path, &bs, &size) < 3) || (path[0] == '#'))
		{
			while(getc(config) != '\n');
			continue;
		}
		
		new_eeprom = malloc(sizeof(struct eeprom));
		if (new_eeprom == NULL)
		{
			fprintf(stderr, "ERROR: Cannot allocate memory for EEPROM metadata.\n");
			errno = ENOMEM;
			return -1;
		}
		
		// TODO: Best way to check for this?
		if (bs < EEPROM_MANAGER_METADATA_LENGTH)
		{
			fprintf(stderr, "ERROR: bs is too small to store SHA and write count in last block. Skipping...\n");
			continue;
		}
		
		// Load up structure
		memset(new_eeprom, 0, sizeof(struct eeprom));
		strncpy(new_eeprom->path, path, EEPROM_MANAGER_PATH_MAX_LENGTH);
		new_eeprom->bs = bs;
		new_eeprom->count = (size / bs);
		
		// If the size of this eeprom is smaller or this is the first eeprom
		if (size < min_size || first_config_eeprom)
			min_size = size;
		
		// Warn if all EEPROMs are not the same size
		if (last_bs == 0) last_bs = bs;
		if (last_count == 0) last_count = new_eeprom->count;
		if (last_bs != bs || last_count != new_eeprom->count)
			fprintf(stderr, "WARNING: EEPROM at path %s does not appear to be the same size as other devices. May have unexpected behavior.\n", path);
		
		push_eeprom_metadata(new_eeprom);
		first_config_eeprom = 0;
	}
	
	eeprom_data_size = min_size;
	
	fclose(config);
	return 0;
}

/**
 * Finds the good eeprom to use
 * 
 * Loads metadata for all eeproms, building a list of eeproms
 * with the max wc (ideally all of them), then scans that list
 * looking for the first one with a correct sha256.
 * That eeprom with the correct sha256 sum is the good_eeprom
 * which is used.
 * 
 * @param good_eeprom, pointer to eeprom device that gets set to the good one
 * @return < 0 on error, 0 on success
 */
int find_good_eeprom(struct eeprom *good_eeprom)
{
	// Load up all meta-data and build a list of EEPROMS with the highest wc
	struct eeprom *d = first_eeprom;
	struct eeprom *max_wc_eeprom[number_eeproms];
	int i = 0, r = 0;
	memset(max_wc_eeprom, 0, number_eeproms);
	good_eeprom = NULL;
	for (d = first_eeprom; d != NULL; d = d->next)
	{
		// Load metadata for this eeprom
		r = read_write_eeprom_metadata(d, 'r');
		if (r == EEPROM_MANAGER_ERROR_METADATA_BAD_MAGIC)
			continue;
		else if (r < 0)
			return r;
		
		// If first one parsed, add to array
		if (max_wc_eeprom[0] == NULL)
		{
			max_wc_eeprom[i++] = d;
			continue;
		}
		
		// Reset max list if this one has a higher wc
		if (d->wc > max_wc_eeprom[0]->wc)
		{
			memset(max_wc_eeprom, 0, number_eeproms);
			i = 0;
			max_wc_eeprom[i++] = d;
		}
		
		// Add this one to the max list if it is the same
		if (d->wc == max_wc_eeprom[0]->wc)
			max_wc_eeprom[i++] = d;
	}
	
	// Find known good eeprom
	for (i = 0; i < number_eeproms && max_wc_eeprom[i] != NULL; i++)
	{
		// verify_eeprom will call read_write_eeprom which will allocate heap
		// storage and load the eeprom contents into device->data.
		// If it checks out, the data remains, if it fails validation, it fress that data.
		// The result here is that after this loop, good_eeprom->data is the only allocated data.
		if (verify_eeprom(max_wc_eeprom[i]) == 0)
		{
			good_eeprom = max_wc_eeprom[i];
			break;
		}
	}
	
	// Return that there was no good eeproms found
	if (good_eeprom == NULL)
		return EEPROM_MANAGER_ERROR_NO_GOOD_DEVICES_FOUND;
	
	return 0;
}


/**
 * Repairs bad EEPROM devices
 * 
 * Scans the list of eeprom devices and repairs all non-correct
 * devices by writing the good_eeprom contents there.
 * 
 * @param good_eeprom  - eeprom object to use a the known-good
 * @return 0 on success, return from write_eeprom on error.
 */
int repair_all_eeproms(struct eeprom *good_eeprom)
{
	struct eeprom *d = first_eeprom;
	int i = 0;
	int r = 0;
	for (d = first_eeprom; d != NULL; d = d->next)
	{
		if (d == good_eeprom)
			continue;
		
		// Repair all eeproms with lower wc or non-matching SHA256
		if (d->wc < good_eeprom->wc || strcmp(d->sha256, good_eeprom->sha256) != 0)
		{
			fprintf(stderr, "WARNING: Repairing EEPROM %d because its write-count or sha256 was incorrect.\n", i);
			r = clone_eeproms(good_eeprom, d);
			// TODO: Handle this error gracefully.
			if (r < 0)
				return r;
		}
		
		i++;
	}
	
	return r;
}


/**
 * Returns whether eeprom manager is initialized
 * 
 * @return 1 if initialized, 0 otherwise
 */
int is_initialized()
{
	// TODO: Semaphore is very required here
	return first_eeprom != NULL;
}

/*
 *  API Function Definitions
 */


int eeprom_manager_initialize()
{
	static int initialized = 0;
	int r = 0;
	
	// Don't do anything if already initialized
	if (initialized) return 0;
	
	// Load the data from the configuration file
	r = load_conf_data();
	if (r < 0)
		return r;
	
	r = open_eeproms();
	if (r < 0)
		return r;
	
	r = find_good_eeprom(good_eeprom);
	if (r < 0)
	{
		int find_ret = r;
		
		// Preference given to the close error. The lack of a good eeprom may have been caused by
		// something related to it.
		r = close_eeproms();
		if (r < 0)
			return r;
		
		// Return find error
		return find_ret;
	}
	
	// Repair any bad eeproms
	r = repair_all_eeproms(good_eeprom);
	if (r < 0)
		return r;
	
	if (close_eeproms() < 0)
		return -1;
	
	initialized = 1;
	return 0;
}


void eeprom_manager_cleanup()
{
	if (is_initialized())
	{
		clear_eeprom_metadata();
	}
}


void eeprom_manager_set_verbosity(int level)
{
	   eeprom_manager_verbosity = level;
}


int eeprom_manager_set_value(char *key, char *value, int flags)
{
	int r = 0;
	json_t *json_value = NULL;
	if (is_initialized() == 0 || key == NULL || value == NULL)
	{
		errno = EINVAL;
		return -1;
	}
	
	// Load up JSON data
	json_root = json_loads(good_eeprom->data, 0, &json_error);
	if (json_root == NULL)
	{
		// TODO: Handle JSON errors. These should really never happen if the sha checked out
		fprintf(stderr, "JSON error on line %d: %s\n", json_error.line, json_error.text);
		return -1;
	}
	if (!json_is_object(json_root))
	{
		fprintf(stderr, "JSON error: Data is not an object.\n");
		json_decref(json_root);
		return -1;
	}
	
	// Make sure key exists if NO_CREATE is set
	if (flags & EEPROM_MANAGER_SET_NO_CREATE)
	{
		if (json_object_get(json_root, key) == NULL)
		{
			// TODO: set errno correctly here
			return -1;
		}
	}
	
	// Make the value string
	json_value = json_string(value);
	r = json_object_set(json_root, key, json_value);
	if (r < 0)
	{
		// TODO: set errno correctly here
		return -1;
	}
	
	// Make sure there is no eeprom data, the use the reference from json_dumps
	free_eeprom_data(good_eeprom);
	good_eeprom->data = json_dumps(json_root, JSON_COMPACT);
	
	// Write the new data to the eeprom
	write_all_eeproms(good_eeprom);
	
	// Clean up JSON data
	json_decref(json_root);
	
	return 0;
}


int eeprom_manager_read_value(char *key, char *value, int length)
{
	json_t *json_value = NULL;
	char *json_txt_value = NULL;
	if (is_initialized() == 0 || key == NULL || value == NULL)
	{
		errno = EINVAL;
		return -1;
	}
	
	// Load up JSON data
	json_root = json_loads(good_eeprom->data, 0, &json_error);
	if (json_root == NULL)
	{
		// TODO: Handle JSON errors. These should really never happen if the sha checked out
		fprintf(stderr, "JSON error on line %d: %s\n", json_error.line, json_error.text);
		return -1;
	}
	if (!json_is_object(json_root))
	{
		fprintf(stderr, "JSON error: Data is not an object.\n");
		json_decref(json_root);
		return -1;
	}
	
	// Get the value
	json_value = json_object_get(json_root, key);
	if (json_value == NULL)
	{
		// TODO set proper errno
		return -1;
	}
	if (!json_is_string(json_value))
	{
		fprintf(stderr, "JSON error: Key's data is not a string.\n");
		json_decref(json_root);
		return -1;
	}
	
	// Copy the value into the return variable
	json_txt_value = (char *) json_string_value(json_value);
	strncpy(value, json_txt_value, length);
	
	// Clean up JSON data
	json_decref(json_root);
	
	return 0;
}

int eeprom_manager_clear()
{
	int r = 0;
	if (is_initialized() == 0)
	{
		errno = EINVAL;
		return -1;
	}
	
	r = open_eeproms();
	if (r < 0)
		return r;
	
	// Write an empty JSON structure into good_eeprom, and populate that through
	free_eeprom_data(first_eeprom);
	first_eeprom->data = "{}";
	r = write_all_eeproms(first_eeprom);
	if (r < 0)
		return r;
	
	r = close_eeproms();
	if (r < 0)
		return r;
	
	return r;
}

// TODO: Support -1 and 0 returns for this function
int eeprom_manager_verify()
{
	struct eeprom *d = NULL;
	int r = 1, t = 0;
	
	if (is_initialized() == 0)
	{
		errno = EINVAL;
		return -1;
	}
	
	// TODO: Need to get the all-eeproms-are-bunk return from find_good_eeprom here
	//       ----> return 0
	
	for (d = first_eeprom; d != NULL; d = d->next)
	{
		// Initialze function already has verified that the good_eeprom is good, so skip it
		if (d == good_eeprom)
			continue;
		
		t = verify_eeprom(d);
		if (t < 0)
		{
			t = clone_eeproms(good_eeprom, d);
			// TODO: Handle this error return
			r = 2;
		}
	}
	return r;
}

struct eeprom *eeprom_manager_info()
{
	if (is_initialized() == 0)
	{
		errno = EINVAL;
		return NULL;
	}
	
	return first_eeprom;
}

