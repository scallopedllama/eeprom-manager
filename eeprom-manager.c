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

int verbosity = 0;

/**
 * EEPROM metadata structure
 * 
 * There is one of these for each EEPROM device specified in the
 * config file in a linked list starting at first_eeprom and ending at last_eeprom.
 */
struct eeprom {
	char path[EEPROM_PATH_MAX_LENGTH];     /**< Path to the EEPROM device */
	size_t bs;                             /**< Block size to write (specified by EEPROM driver) */
	size_t count;                          /**< Number of blocks that can be written */
	int fd;                                /**< File descriptor number for the opened file (0 if closed) */
	struct eeprom *next;                   /**< Next eeprom in the list */
};
struct eeprom *first_eeprom = NULL;
struct eeprom *last_eeprom = NULL;


/**
 * Safely Clears the eeproms linked list
 */
void clear_eeprom_metadata()
{
	struct eeprom *current_eeprom = first_eeprom;
	while (current_eeprom->next != NULL)
	{
		struct eeprom *prev_eeprom = current_eeprom;
		current_eeprom = current_eeprom->next;
		free(prev_eeprom);
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
 * Reads or Writes the EEPROM contents
 * 
 * Either reads from the already-open fd writing into string or writes from the
 * string into the fd in bs chunks a maximum of count times, stopping after the
 * first null byte is encountered.
 * 
 * Also reads or writes the sha256 from the end of the device to / from the sha string.
 * Returns -EIO if it fails while trying to read or write the EEPROM.
 * Returns -EINVAL if op was not 'r' or 'w'.
 * 
 * @param op     'r' or 'w' to Read or Write respectively
 * @param fd     Opened EEPROM file
 * @param buf    String into which the contents should be written. sizeof must be at least (bs * (count - 1) + 1)
 * @param sha    Buffer into which the sha read from the end of the device is written. sizeof must be at least SHA256_DIGEST_LENGTH
 * @param bs     Block size to read
 * @param count  Number of blocks in whole EEPROM device
 * @return < 0 to indicate failure, else length of the JSON string read
 */
size_t read_write_eeprom(char op, int fd, char *buf, unsigned char *sha, size_t bs, size_t count)
{
	int i, j, retval, r, attempts, null_found;
	char *pos = buf;
	
	if (op != 'r' && op != 'w')
		return -EINVAL;
	
	// Start at the beginning of the device
	lseek(fd, 0, SEEK_SET);
	
	// Clear the buffer if reading
	if (op == 'r')
		memset(buf, 0, (bs * count) + 1);
	
	// Read / write count blocks
	for (i = 0; i < count; i++)
	{
		// Do a whole block
		j = 0;
		r = 0;
		null_found = 0;
		attempts = 0;
		
		// Clear bytes after null if writing here
		if (op == 'w')
			null_found = clear_after_null(pos, bs);
		
		// Do read or write
		if (op == 'r')
			while (((r += read(fd, pos + r, bs - r)) < bs) && (attempts++ < MAX_RW_ATTEMPTS));
		else
			while (((r += write(fd, pos + r, bs - r)) < bs) && (attempts++ < MAX_RW_ATTEMPTS));
		
		// Make sure it was all read or written
		if (attempts >= MAX_RW_ATTEMPTS)
		{
			fprintf(stderr, "ERROR: Attempted %d times to %s %d bytes at offset %d\n", MAX_RW_ATTEMPTS, (op == 'r' ? "read" : "write"), bs, i);
			fprintf(stderr, "       but only managed to %s %d bytes. Aborting.\n", (op == 'r' ? "read" : "write"), r);
			return -EIO;
		}
		
		// If reading, clear bytes after null here
		if (op == 'r')
			null_found = clear_after_null(pos, bs);
		
		// Done if there was a null
		if (null_found)
			break;
		
		// Advance the position in the buffer
		pos += bs;
	}
	
	// Calculate return value
	if (null_found)
		retval = (i * bs) + j + 1;
	else
		retval = (count * bs) + 1;
	
	// Read / write the sha from the end of the device
	r = 0;
	attempts = 0;
	if (op == 'r')
		while (((r += read((fd + bs * (count - 1)), sha, SHA256_DIGEST_LENGTH - r)) < SHA256_DIGEST_LENGTH) && (attempts++ < MAX_RW_ATTEMPTS));
	else
		while (((r += write((fd + bs * (count - 1)), sha, SHA256_DIGEST_LENGTH - r)) < SHA256_DIGEST_LENGTH) && (attempts++ < MAX_RW_ATTEMPTS));
		
	// Make sure it was all read
	if (attempts >= MAX_RW_ATTEMPTS)
	{
		fprintf(stderr, "ERROR: Attempted %d times to %s %d bytes at offset %d\n", MAX_RW_ATTEMPTS, (op == 'r' ? "read" : "write"), SHA256_DIGEST_LENGTH, bs * (count - 1)));
		fprintf(stderr, "       but only managed to %s %d bytes. Aborting.\n", (op == 'r' ? "read" : "write"), r);
		return -EIO;
	}
	
	return retval;
}

int write_eeprom(int fd, char *buf, size_t bs, size_t count)
{
	// Calculate sha256
	unsigned char sha256[SHA256_DIGEST_LENGTH];
	SHA256((unsigned char*)&buf, strlen(buf), &sha256);
	
	//TODO: Also write the GMT Unix timestamp after the sha to keep track of
	//      most up-to-date value
	read_write_eeprom('w', fd, buf, sha256, bs, count);
}

int verify_eeprom(int fd, char *buf, size_t bs, size_t count)
{
	
}

// TODO: ALL THESE API CALLS AND SUCH MUST BE PROTECTED BY A SEMAPHORE TO BE THREAD SAFE!!


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
}


/**
 * Initializes EEPROM Manager
 * 
 * Loads the config file and parses its contents, building up the
 * metadata necessary to work correctly.
 * 
 * @return 0 on success, -1 on error
 */
int initialize()
{
	static int initialized = 0;
	FILE *config = NULL;
	
	// Don't do anything if already initialized
	if (initialized) return 0;
	
	// Parse config file
	config = fopen(EEPROM_MANAGER_CONF_PATH, "r");
	while(true)
	{
		struct eeprom *new_eeprom = NULL;
		char path[EEPROM_PATH_MAX_LENGTH];
		int bs;
		int size;
		
		// Skip comments
		if (fgetc(config) == '#')
		{
			while(fgetc(config) != '\n');
			continue;
		}
		
		fscanf(config, "%s %d %d\n", path, bs, size);
		new_eeprom = malloc(sizeof(struct eeprom));
		if (new_eeprom == NULL)
		{
			fprintf(stderr, "ERROR: Cannot allocate memory for EEPROM metadata.\n");
			return -ENOMEM;
		}
		
		strncpy(new_eeprom->path, path, EEPROM_PATH_MAX_LENGTH);
		new_eeprom->bs = bs;
		new_eeprom->count = (size / bs);
		
		push_eeprom_metadata(new_eeprom);
	}
	
	fclose(config);
	
	initialized = 1;
}

/*
 *  API Function Definitions
 */
void eeprom_manager_set_verbosity(int level)
{
	verbosity = level;
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
