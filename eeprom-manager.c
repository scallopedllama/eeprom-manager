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

int eeprom_manager_verbosity = 0;
size_t eeprom_data_size = 0;
int number_eeproms = 0;
struct eeprom *first_eeprom = NULL;
struct eeprom *last_eeprom = NULL;
struct eeprom *good_eeprom = NULL;


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
		if (prev_eeprom->data != NULL)
		{
			free(prev_eeprom->data);
			prev_eeprom->data = NULL;
		}
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
		last_eeprom->next = new_eeprom;
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
		if (*(buf + j) == '\0')
			r = j;
		// Clear bytes after the null terminator
		if (r)
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
 */
size_t read_write_all(struct eeprom *device, char op, void *buf, size_t count)
{
	size_t r = 0;
	unsigned int attempts = 0;
	
	if (op != 'r' && op != 'w')
		return -EINVAL;
	
	// Read and write may return less than count so keep trying until that much is read or written.
	if (op == 'w')
		while (((r += write(device->fd, buf, count)) < count) && (attempts++ < EEPROM_MANAGER_MAX_RW_ATTEMPTS));
	else
		while (((r += read(device->fd, buf, count)) < count) && (attempts++ < EEPROM_MANAGER_MAX_RW_ATTEMPTS));
	
	// Make sure it was all read or written
	if (attempts >= EEPROM_MANAGER_MAX_RW_ATTEMPTS)
	{
		fprintf(stderr, "ERROR: Attempted %d times to %s %d bytes\n", EEPROM_MANAGER_MAX_RW_ATTEMPTS, (op == 'r' ? "read" : "write"), count);
		fprintf(stderr, "       but only managed to %s %d bytes. Aborting.\n", (op == 'r' ? "read" : "write"), r);
		return -EIO;
	}
	
	return r;
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
	if (strcmp(buffer, EEPROM_MANAGER_MAGIC) != 0)
		return -EMEDIUMTYPE;
	
	// Read the SHA
	r = read_write_all(device, op, device->sha256, EEPROM_MANAGER_SHA_STRING_LENGTH);
	if (r == 0)
	{
		if (op == 'w')
			sprintf(buffer, "%010u", device->wc);
		
		r = read_write_all(device, op, buffer, EEPROM_MANAGER_WC_STRING_LENGTH);
		
		if (op == 'r')
			sscanf(buffer, "%010u", &(device->wc));
	}
	
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
	int i, retval, r, null_found;
	char *pos = NULL;
	
	if (op != 'r' && op != 'w')
		return -EINVAL;
	
	// Make sure data is allocated
	if (op == 'r')
	{
		if (device->data == NULL)
			device->data = malloc(eeprom_data_size);
		if (device->data == NULL)
		{
			fprintf(stderr, "ERROR: Cannot allocate memory for EEPROM data.\n");
			return -ENOMEM;
		}
	}
	else if (device->data == NULL)
		return -EINVAL;
	
	// Set starting position
	pos = device->data;
	
	// Clear the buffer if reading
	if (op == 'r')
		memset(device->data, 0, (device->bs * device->count) + 1);
	// Clear the last block if writing
	if (op == 'w')
	{
		char zero_block[device->bs];
		memset(zero_block, 0, device->bs);
		lseek(device->fd, -1 * device->bs, SEEK_END);
		// TODO: Check for errors
		r = read_write_all(device, 'w', zero_block, device->bs);
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
		// TODO: Check for errors
		r = read_write_all(device, op, pos, device->bs);
		
		// If reading, clear bytes after null here
		if (op == 'r')
			null_found = clear_after_null(pos, device->bs);
		
		// Done if there was a null
		if (null_found)
			break;
		
		// Advance the position in the buffer
		pos += device->bs;
	}
	
	// Calculate return value
	if (null_found)
		retval = (i * device->bs) + null_found + 1;
	else
		retval = (device->count * device->bs) + 1;
	
	// Read / write the device->sha256 and device->wc at the end of the device
	// TODO: This may not be required for reading, but is definitely required for writing
	read_write_eeprom_metadata(device, op);
	
	return retval;
}


/**
 * Writes data to EEPROM
 * 
 * Calculates the sha256 checksum for device->data
 * then writes that data to the provided eeprom device.
 * 
 * @param device EEPROM device to write to
 * @return < 1 on error, number of bytes written on success (can be 0, not error)
 */
int write_eeprom(struct eeprom *device)
{
	if (device->data == NULL)
		return -EINVAL;
	
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
 * Reads the contents of the eeprom and verifies the sha256 sum,
 * returing the write count on success. Frees read data if invalid.
 * 
 * @param device EEPROM device to verify
 * @return -1 on non-match or error, write count on success
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
		free(device->data);
		device->data = NULL;
		return -1;
	}
	return device->wc;
}


/**
 * Opens all EEPROM files
 * 
 * Iterates through the eeproms list and opens all the files then gets an advisory lock on them.
 * 
 * @return 0 on success, -1 on error (check errno)
 */
int open_eeproms()
{
	int r, err;
	struct eeprom *current_eeprom = first_eeprom;
	while (current_eeprom->next != NULL)
	{
		// Open the file
		current_eeprom->fd = open(current_eeprom->path, 0);
		// Get an advisory lock on it
		while((r = flock(current_eeprom->fd, LOCK_EX)) != 0 && errno == EINTR);
		if (r != 0)
		{
			err = errno;
			fprintf(stderr, "Failed to get lock on EEPROM %s: %s\n", current_eeprom->path, strerror(err));
			// TODO: Proper cleanup, or just ignore this one?
			errno = err;
			return -1;
		}
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
	int r, err;
	struct eeprom *current_eeprom = first_eeprom;
	while (current_eeprom->next != NULL)
	{
		// Release advisory lock on file
		while((r = flock(current_eeprom->fd, LOCK_UN)) != 0 && errno == EINTR);
		if (r != 0)
		{
			err = errno;
			fprintf(stderr, "Failed to release lock on EEPROM %s: %s\n", current_eeprom->path, strerror(err));
			// TODO: Proper cleanup, or just ignore this one?
			errno = err;
			return -1;
		}
		// Close the file
		close(current_eeprom->fd);
	}
	return 0;
}


int load_conf_data()
{
	FILE *config = NULL;
	size_t last_bs = 0, last_count = 0;
	int min_size = -1;
	
	// Load config file
	config = fopen(EEPROM_MANAGER_CONF_PATH, "r");
	if (config == NULL)
		return -1;
	
	// Parse config file
	while(!feof(config))
	{
		struct eeprom *new_eeprom = NULL;
		char path[EEPROM_MANAGER_PATH_MAX_LENGTH];
		int bs;
		int size;
		
		// Skip comments
		if (fgetc(config) == '#')
		{
			while(fgetc(config) != '\n');
			continue;
		}
		
		fscanf(config, "%s %d %d\n", path, &bs, &size);
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
		
		if (size < min_size || min_size < 0)
			min_size = size;
		
		// Warn if all EEPROMs are not the same size
		if (last_bs == 0) last_bs = bs;
		if (last_count == 0) last_count = new_eeprom->count;
		if (last_bs != bs || last_count != new_eeprom->count)
			fprintf(stderr, "WARNING: EEPROM at path %s does not appear to be the same size as other devices. May have unexpected behavior.\n", path);
		
		push_eeprom_metadata(new_eeprom);
	}
	
	eeprom_data_size = min_size;
	
	fclose(config);
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
 * @return pointer to the good_eeprom or NULL on error.
 */
struct eeprom *find_good_eeprom()
{
	// Load up all meta-data and build a list of EEPROMS with the highest wc
	struct eeprom *d = first_eeprom;
	struct eeprom *max_wc_eeprom[number_eeproms];
	struct eeprom *r = NULL;
	int i = 0;
	memset(max_wc_eeprom, 0, number_eeproms);
	max_wc_eeprom[i++] = d;
	for (d = first_eeprom; d != NULL; d = d->next)
	{
		// Load metadata for this eeprom
		int r = read_write_eeprom_metadata(d, 'r');
		
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
	for (i = 0; max_wc_eeprom[i] != NULL; i++)
	{
		// verify_eeprom will call read_write_eeprom which will allocate heap
		// storage and load the eeprom contents into device->data.
		// If it checks out, the data remains, if it fails validation, it fress that data.
		// The result here is that after this loop, r->data is the only allocated data.
		if (verify_eeprom(max_wc_eeprom[i]) == max_wc_eeprom[i]->wc)
		{
			r = max_wc_eeprom[i];
			break;
		}
	}
	
	return r;
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
		// Repair all eeproms with lower wc or non-matching SHA256
		if (d->wc < good_eeprom->wc || strcmp(d->sha256, good_eeprom->sha256) != 0)
		{
			fprintf(stderr, "WARNING: Repairing EEPROM %d because its write-count or sha256 was incorrect.\n", i);
			
			// write_eeprom will only write data if sha has changed, so clear sha to make sure that happens
			d->sha256[0] = '\0';
			// Make sure the written eeprom ends up with the same wc as the good eeprom
			d->wc = good_eeprom->wc - 1;
			d->data = good_eeprom->data;
			r = write_eeprom(d);
			// TODO: Handle this error gracefully.
			if (r < 0)
				return r;
		}
		i++;
	}
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
	if (load_conf_data() < 0)
		return -1;
	
	// Open EEPROM files
	// TODO: Handle bad return here
	r = open_eeproms();
	
	// Find the good eeprom and make sure it found one
	good_eeprom = find_good_eeprom();
	if (good_eeprom == NULL)
	{
		fprintf(stderr, "ERROR: No EEPROM devices passed sha256 checksum! All data appears to be lost.\n");
		// TODO: Handle this error situation. negative return from here should maybe trigger a clear().
		fprintf(stderr, "       EEPROM Manager will continue with an empty device.\n");
		return -1;
	}
	
	// Repair any bad eeproms
	r = repair_all_eeproms(good_eeprom);
	// TODO: Handle bad return
	
	// Close EEPROM files
	// TODO: Handle bad return here
	r = open_eeproms();
	
	initialized = 1;
	return 0;
}


void eeprom_manager_cleanup()
{
	clear_eeprom_metadata();
}


void eeprom_manager_set_verbosity(int level)
{
	   eeprom_manager_verbosity = level;
}

int eeprom_manager_set_value(char *key, char *value, int flags)
{
	
}

int eeprom_manager_read_value(char *key, char *value, int length)
{
	
}

int eeprom_manager_clear()
{
	
}

int eeprom_manager_verify()
{
	
}

struct eeprom *eeprom_manager_info()
{
	return first_eeprom;
}

