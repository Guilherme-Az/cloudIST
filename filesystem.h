#ifndef FILESYSTEM__H
#define FILESYSTEM__H

#include <dirent.h>
#include <stddef.h>

/**
 * Checks whether a path exists and is a directory.
 *
 * @param path Directory path.
 *
 * @return 1 if it exists and is a directory, 0 otherwise.
 */
int path_exists(const char *path);

/**
 * Checks whether a path exists and is a regular file.
 *
 * @param path File path.
 *
 * @return 1 if it exists and is a regular file, 0 otherwise.
 */
int file_exists(const char *path);

/**
 * Converts the given path into an absolute path, resolving symbolic links,
 * relative components ('.' and '..'), and redundant separators. The resolved
 * path is copied into the provided buffer.
 *
 * @param path Path to resolve.
 * @param buffer Destination buffer where the absolute path will be stored.
 * @param size Size of the destination buffer, in bytes.
 *
 * @return 0 if the path was successfully resolved and copied to the buffer
 * @return 1 if the path could not be resolved or the buffer is too small.
 */
int absolute_path(const char *path, char *buffer, size_t size);

/**
 * scandir() filter: keeps only the entries whose name ends in ".conf".
 *
 * @param entry Directory entry being checked.
 *
 * @return 1 if it is a .conf file (keep it), 0 otherwise.
 */
int filter_conf_only(const struct dirent *entry);

/**
 * Copies the CONTENTS of source_path into vm_path, recursively: every
 * subdirectory is created and every regular file is copied, keeping the same
 * structure (vm_path gets a.txt and sub/b.txt, not source/a.txt). Symbolic
 * links are followed. Uses only open/read/write and opendir/readdir.
 *
 * vm_path must already exist.
 *
 * @param source_path Directory with the inputs.
 * @param vm_path Destination directory (the VM folder).
 *
 * @return 0 on success.
 * @return 1 on error (the error is printed).
 */
int copy_path_recursive(const char *source_path, const char *vm_path);

/**
 * Copies one regular file (created or truncated at the destination).
 *
 * @param source_file File to copy.
 * @param vm_file Destination file.
 *
 * @return 0 on success.
 * @return 1 on error (the error is printed).
 */
int copy_file(const char *source_file, const char *vm_file);

/**
 * Deletes everything INSIDE vm_path, recursively (vm_path itself stays).
 * Uses lstat, so a symbolic link is removed as a link and its target (which
 * may be outside the VM folder) is never touched.
 *
 * @param vm_path Directory to empty.
 *
 * @return 0 on success.
 * @return 1 on error (the error is printed).
 */
int delete_path_recursive(const char *vm_path);

/**
 * Prepares the VM folder /tmp/CloudIST-114255/<reservation_id>/<vm_id>: creates the
 * folders that don't exist yet, deletes any old content and copies the
 * inputs into it.
 *
 * @param input_folder Directory with the inputs (from the D command).
 * @param reservation_id Id of the VM's reservation.
 * @param vm_id Id of the VM.
 * @param vm_path Buffer where the VM folder path is stored.
 * @param size Size of vm_path, in bytes.
 *
 * @return 0 on success.
 * @return 1 on error (the error is printed).
 */
int make_vm_folder_path(const char *input_folder, const char *reservation_id, const char *vm_id,
                        char *vm_path, size_t size);

/**
 * Deletes the VM folder /tmp/CloudIST-114255/<reservation_id>/<vm_id> and everything
 * inside it. A folder that doesn't exist is not an error.
 *
 * @param reservation_id Id of the VM's reservation.
 * @param vm_id Id of the VM.
 *
 * @return 0 on success.
 * @return 1 on error (the error is printed).
 */
int remove_vm_folder(const char *reservation_id, const char *vm_id);

/**
 * Deletes the (empty) reservation folder /tmp/CloudIST-114255/<reservation_id>.
 * A folder that doesn't exist, or still has something inside, is left alone.
 *
 * @param reservation_id Id of the reservation.
 *
 * @return 0 on success.
 * @return 1 on error (the error is printed).
 */
int remove_reservation_folder(const char *reservation_id);

#endif // FILESYSTEM__H
