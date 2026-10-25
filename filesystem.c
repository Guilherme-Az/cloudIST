#define _XOPEN_SOURCE 700

#include "filesystem.h"
#include "constants.h"

#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <dirent.h>//POSIX directory functions (opendir, readdir, scandir)



int path_exists(const char *path){
  struct stat st;

  if (stat(path, &st) != 0)
    return 0;

  return S_ISDIR(st.st_mode);
}

int file_exists(const char *path){
  struct stat st;

  if (stat(path, &st) != 0)
    return 0;

  return S_ISREG(st.st_mode);
}

int absolute_path(const char *path, char *buffer, size_t size){
  char *resolved = realpath(path, NULL);

  if (resolved == NULL)
    return 1;

  if (strlen(resolved) >= size) {
    free(resolved);
    return 1;
  }

  strcpy(buffer, resolved);

  free(resolved);
  return 0;
}

int filter_conf_only(const struct dirent *entry) {
  //no dot in the name -> no extension -> not a .conf
  if (strchr(entry->d_name, '.') == NULL) {
    return 0;
  }
  //the extension is what comes after the LAST dot (so x.conf.bak is not a .conf)
  const char *extension = strrchr(entry->d_name, '.');
  if(extension == NULL || strcmp(extension, ".conf") == 0){
    return 1;
  }

  return 0;
}

int copy_path_recursive(const char *source_path, const char *vm_path){

  //the POSIX function to open directories
	DIR *dir = opendir(source_path);

  if (dir == NULL) {
    perror("opendir error");
    return 1;
  }

  struct dirent *entry;

  //entry is the current file/folder; when it's NULL the loop ends
  while ((entry = readdir(dir)) != NULL) {

    //skip . and .. so we don't loop forever
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }

    char source_entry_path[MAX_PATH_SIZE];
    char vm_entry_path[MAX_PATH_SIZE];

    //build the full paths, in case there are folders inside folders
    snprintf(source_entry_path, MAX_PATH_SIZE, "%s/%s", source_path, entry->d_name);
    snprintf(vm_entry_path, MAX_PATH_SIZE, "%s/%s", vm_path, entry->d_name);

    if (path_exists(source_entry_path)) {
      //create the folder (if it already exists that's fine) and then the recursive part for folders inside folders
      if (mkdir(vm_entry_path, 0777) != 0 && errno != EEXIST) {
        perror("mkdir error");
        closedir(dir);
        return 1;
      }
      if (copy_path_recursive(source_entry_path, vm_entry_path) != 0) {
        closedir(dir);
        return 1;
      }

    //when we reach a file, copy it
    } else if (file_exists(source_entry_path)) {
      if (copy_file(source_entry_path, vm_entry_path) != 0) {
        closedir(dir);
        return 1;
      }
    }
  }
  closedir(dir);

	return 0;
}

int copy_file(const char *source_file, const char *vm_file) {

  //open the source file for reading
  int fd_source = open(source_file, O_RDONLY);
  if (fd_source < 0) {
    perror("open source file error");
    return 1;
  }

  //open the destination file for writing (created if it doesn't exist, emptied if it does)
  int fd_vm = open(vm_file, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (fd_vm < 0) {
    perror("open vm file error");
    close(fd_source);
    return 1;
  }

  char buffer[4096];
  ssize_t bytes_read;

  //basic loop: read chunks into the buffer and write them to the destination file
  while ((bytes_read = read(fd_source, buffer, sizeof(buffer))) > 0) {
    //if write didn't write everything that was read, it's an error
    if (write(fd_vm, buffer, (size_t)bytes_read) != bytes_read) { //size_t cast because read returns a signed ssize_t and write takes an unsigned size_t
      perror("write error");
      close(fd_source);
      close(fd_vm);
      return 1;
    }
  }

  //read returns 0 at the end of the file and -1 on error; the while ends in both cases
  if (bytes_read < 0) {
    perror("read error");
    close(fd_source);
    close(fd_vm);
    return 1;
  }

  close(fd_source);
  //closing the written file can also fail (e.g. disk full)
  if (close(fd_vm) != 0) {
    perror("close vm file error");
    return 1;
  }

  return 0;
}

int delete_path_recursive(const char *vm_path){

  //the POSIX function to open directories
	DIR *dir = opendir(vm_path);

  if (dir == NULL) {
    perror("opendir error");
    return 1;
  }

  struct dirent *entry;

  //entry is the current file/folder; when it's NULL the loop ends
  while ((entry = readdir(dir)) != NULL) {

    //skip . and .. so we don't loop forever
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }

    char vm_entry_path[MAX_PATH_SIZE];

    //build the full path, in case there are folders inside folders
    snprintf(vm_entry_path, MAX_PATH_SIZE, "%s/%s", vm_path, entry->d_name);

    //lstat (not stat) so symlinks are deleted as links. path_exists and file_exists use stat, which follows them
    struct stat st;
    if (lstat(vm_entry_path, &st) != 0) {
      perror("lstat error");
      closedir(dir);
      return 1;
    }


    if (S_ISDIR(st.st_mode)) {
      //recursive part for folders inside folders, then remove the (now empty) folder

      if (delete_path_recursive(vm_entry_path) != 0) {
        closedir(dir);
        return 1;
      }

      if (rmdir(vm_entry_path) != 0) {
        perror("rmdir error");
        closedir(dir);
        return 1;
      }
    //files and symlinks (shortcuts): unlink
    } else {
      if (unlink(vm_entry_path) != 0) {
        perror("unlink error");
        closedir(dir);
        return 1;
      }
    }
  }
  closedir(dir);

	return 0;
}

int make_vm_folder_path(const char *input_folder, const char *reservation_id, const char *vm_id,
                        char *vm_path, size_t size) {
  //destination path of the VM: /tmp/CloudIST-114255/<reservation>/<vm>
  snprintf(vm_path, size, "%s/%s/%s", CLOUDIST_ROOT, reservation_id, vm_id);

  /*mkdir only creates the last level of the path, so they are created one at a time:
    /tmp/CloudIST-114255 -> /tmp/CloudIST-114255/<reservation> -> /tmp/CloudIST-114255/<reservation>/<vm>
  EEXIST = the folder already exists, that's not an error*/
  char reservation_path[MAX_PATH_SIZE];
  snprintf(reservation_path, MAX_PATH_SIZE, "%s/%s", CLOUDIST_ROOT, reservation_id);

  if ((mkdir(CLOUDIST_ROOT, 0777) != 0 && errno != EEXIST) ||
      (mkdir(reservation_path, 0777) != 0 && errno != EEXIST) ||
      (mkdir(vm_path, 0777) != 0 && errno != EEXIST)) {
    perror("mkdir error");
    return 1;
  }

  //the teacher said that if the folder already exists, its content must be deleted (recursively) before copying
  if (delete_path_recursive(vm_path) != 0) {
    fprintf(stderr, "Failed to delete existing input folder for VM %s.\n", vm_id);
    return 1;
  }
  if(copy_path_recursive(input_folder, vm_path) != 0){
    fprintf(stderr, "Failed to copy input folder for VM %s.\n", vm_id);
    return 1;
  }

  return 0;
}

int remove_vm_folder(const char *reservation_id, const char *vm_id) {
  char vm_path[MAX_PATH_SIZE];
  snprintf(vm_path, sizeof(vm_path), "%s/%s/%s", CLOUDIST_ROOT, reservation_id, vm_id);

  //the folder may not exist (e.g. the child failed before creating it): nothing to delete
  if (!path_exists(vm_path)) {
    return 0;
  }

  //first everything inside, then the folder itself (rmdir only removes empty folders)
  if (delete_path_recursive(vm_path) != 0) {
    return 1;
  }
  if (rmdir(vm_path) != 0) {
    perror("rmdir vm folder error");
    return 1;
  }

  return 0;
}

int remove_reservation_folder(const char *reservation_id) {
  char reservation_path[MAX_PATH_SIZE];
  snprintf(reservation_path, sizeof(reservation_path), "%s/%s", CLOUDIST_ROOT, reservation_id);

  //ENOENT: it was never created; ENOTEMPTY/EEXIST: a VM folder is still there (e.g. a VM that never started)
  if (rmdir(reservation_path) != 0 && errno != ENOENT && errno != ENOTEMPTY && errno != EEXIST) {
    perror("rmdir reservation folder error");
    return 1;
  }

  return 0;
}
