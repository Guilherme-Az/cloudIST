#define _XOPEN_SOURCE 700

#include "filesystem.h"

#include <sys/stat.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <dirent.h>//tenho de ver se posso usar isto mas acho que sim

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

//PERGUNTAR ao stor se posso usar esta função ... e se fica melhor aqui ou na main
int filter_conf_only(const struct dirent *ficheiro) {
  if (ficheiro->d_type!= DT_REG) {
    return 1;
  }

  int *ficheiro_type = strrchr(ficheiro->d_name, ".");
  if(ficheiro_type == NULL || strcmp(ficheiro_type, ".conf") != 0){
    return 1;
  }

  return 0;
}

int copy_path_recursive(const char *source_path, const char *vm_path){

  //a função posix para abrir diretorias
	Dir *dir = opendir(source_path);

  if (dir == NULL) {
    perror("opendir error");
    return 1;
  }
  
  struct dirent *entry;

  //se o entre é o ficheir/pasta atual se for null acaba o loop
  while ((entry = readdir(dir)) != NULL) {

    //isto foi o chat que disse é para não entrar em loops infinitos
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }

    char source_entry_path[MAX_PATH_SIZE];
    char vm_entry_path[MAX_PATH_SIZE];

    //copiar o caminho todo tipo pronto se houver pastas dentro de pastas
    snprintf(source_entry_path, MAX_PATH_SIZE, "%s/%s", source_path, entry->d_name);
    snprintf(vm_entry_path, MAX_PATH_SIZE, "%s/%s", vm_path, entry->d_name);

    //uso aqui as funções axiliares do stor 
    if (path_exists(source_entry_path)) {
      //este é o comando para fazer mm a diretoria
      mkdir(vm_entry_path, 0777);
      //a parte recurciva para as pastas dentro de pastas
      copy_path_recursive(source_entry_path, vm_entry_path);

    //se chegarmos a um ficheiro copiar
    } else if (file_exists(source_entry_path)) {
      copy_file(source_entry_path, vm_entry_path);
    }
  }
  
	return 0;
}

int copy_file(const char *source_file, const char *vm_file) {

  //abres o ficheiro de origem para leitura
  int fd_source = open(source_file, O_RDONLY);
  if (fd_source < 0) {
    perror("open source file error");
    return 1;
  }

  //abres o ficheiro de destino para escrever, ali o numero o chat disse que era mm preciso
  int fd_vm = open(vm_file, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (fd_vm < 0) {
    perror("open vm file error");
    close(fd_source);
    return 1;
  }

  char buffer[4096];
  ssize_t bytes_read;

  //loop basico para ler em chunks do buffer e escrever no ficheiro de destino
  while ((bytes_read = read(fd_source, buffer, sizeof(buffer))) > 0) {
    write(fd_vm, buffer, bytes_read);
  }

  close(fd_source);
  close(fd_vm);

  return 0;
}
