#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <semaphore.h>
#include <pthread.h>

#include "parser.h"
#include "datacenter.h"
#include "constants.h"
#include "filesystem.h"
#include "datacenter_utils.h"


typedef struct {
	DataCenter* dc;
	sem_t* slots;
	char path[MAX_PATH_SIZE + 256];
} ConfArgs;


static void process_conf_file(DataCenter* dc, const char* conf_path){
	int fd = open(conf_path, O_RDONLY);
	if (fd < 0){
		perror("open error");
		return;
	}

		int done = 0;

	while(!done){
		switch(get_next_command(fd)){
			case CMD_DEFINE: {
					VMType vmtype;

					if (parse_define(fd, &vmtype) != 0) {
						fprintf(stderr, "Invalid define command. See H (help) for usage.\n");
						continue;
					}

					if(datacenter_define_VM(dc, &vmtype) != 0){
						fprintf(stderr, "Failed to define VM.\n");
						continue;
					}

					printf("VM successfully defined!\n");

					break;
				}
			
			case CMD_RESERVE: {
					Reservation reservation = {0};

					size_t num_items = parse_reserve(fd, &reservation, MAX_RESERVATIONS_ITEMS);

					if (num_items == 0) {
						fprintf(stderr, "Invalid reserve command. See H (help) for usage.\n");
						continue;
					}

					if (datacenter_reserve(dc, &reservation) != 0) {
						fprintf(stderr, "Failed to reserve VMs.\n");
						continue;
					}

					printf("Reservation made successfully!\n");

					break;
				}

			case CMD_EXECUTE: {
					char id[MAX_STRING_SIZE];

					if(parse_execute(fd, id) != 0){
						fprintf(stderr, "Invalid execute command. See H (help) for usage.\n");
						continue;
					}

					
					Reservation *reservation = find_pending_reservation(dc, id);
						if (reservation == NULL) {
							continue; 
						}
					
					//se alguma copia falhar não pode executar (o enunciado diz para copiar antes de executar)
					int copy_failed = 0;

					//loop pela lista de vms da reserva
					for (size_t k = 0; k < reservation->num_vms; k++){
						VM *vm = reservation->vms[k];

						char *source_input_path = vm->type->input_folder;
						char vm_input_path[MAX_PATH_SIZE];
						snprintf(vm_input_path, MAX_PATH_SIZE, "/tmp/CloudIST/%s/%s", id, vm->id);
						// Construir o caminho de destino para a VM
						/*"/tmp/CloudIST/%s/%s" -o correto path */
						char reservation_path[MAX_PATH_SIZE];
						snprintf(reservation_path, MAX_PATH_SIZE, "/tmp/CloudIST/%s", id);

						if ((mkdir("/tmp/CloudIST", 0777) != 0 && errno != EEXIST) ||
								(mkdir(reservation_path, 0777) != 0 && errno != EEXIST) ||
								(mkdir(vm_input_path, 0777) != 0 && errno != EEXIST)) {
							perror("mkdir error");
							copy_failed = 1;
							break;
						}

						if(copy_path_recursive(source_input_path, vm_input_path) != 0){
							fprintf(stderr, "Failed to copy input folder for VM %s.\n", vm->id);
							copy_failed = 1;
							break;
						}
					}

					//o break só sai do for, por isso verifica-se aqui se a copia falhou para não executar
					if (copy_failed) {
						fprintf(stderr, "Reservation %s not executed because its inputs could not be copied.\n", id);
						continue;
					}

					if (datacenter_execute(dc, id) != 0) {
						fprintf(stderr, "Failed to execute reservation.\n");
						continue;
					}

					printf("Finished reservation execution!\n");

					break;
				}

			case CMD_LIST:
				if (datacenter_list(dc) != 0) {
						fprintf(stderr, "Failed to list VMs.\n");
						continue;
					}

				break;

			case CMD_WAIT: {
					unsigned int delay;

					if(parse_wait(fd, &delay) != 0){
						fprintf(stderr, "Invalid wait command. See H (help) for usage.\n");
						continue;
					}

					datacenter_wait(delay);
					break;
				}

			case CMD_HELP:
				printf(
					"Spaces between arguments are allowed, but not after command end.\n"
					"Available commands:\n"
					" D <VM_TYPE_ID> <INPUT_FOLDER> <EXECUTABLE_PATH> <RAM_NEEDED> <DISK_NEEDED> <VCPU_NEEDED_COUNT>\n"
					" R <RESERVATION_ID> [<VM_TYPE_ID> <COUNT> <SERVER_ID>]+\n"
					" A <RESERVATION_ID>\n"
					" L\n"
					" E <DELAY_MS>\n"
					" H\n"
				);
				break;

			case CMD_EMPTY:
				break;

			case EOC:
				done=1;
				break;
			
			case CMD_INVALID:
				fprintf(stderr, "Invalid Command. See H (help) for usage.\n");
				break;
		}
	}

	close(fd);
}


static void *run_conf(void *arg){
	ConfArgs *a = arg;

	process_conf_file(a->dc, a->path);
	// Depois de processar o ficheiro libertamos uma vaga no semáforo
	sem_post(a->slots);
	return NULL;
}

static void free_conf_list(struct dirent **list, int n){
	for(int i=0; i<n; i++){
		free(list[i]);
	}
	free(list);
}


int main(int argc, char **argv){
	DataCenter dc;
	datacenter_init(&dc);

	if (argc != 7) {// Adding input_dir && MAX_THREADS
    fprintf(stderr, "Usage: %s <servers> <ram> <disk> <cpus> <input_dir> <max_threads>\n", argv[0]);
    return 1;
  }

	size_t servers;
	size_t ram;
	size_t disk;
	double cpu;
	char input_dir[MAX_PATH_SIZE];//Adding input_dir
	size_t max_threads;

  	// Verificações
	if (parse_size_t_arg(argv[1], &servers) != 0 ||
			parse_size_t_arg(argv[2], &ram) != 0 ||
			parse_size_t_arg(argv[3], &disk) != 0 ||
			parse_double_arg(argv[4], &cpu) != 0 ||
			(argv[5] == NULL || argv[5][0] == '\0') ||
			parse_size_t_arg(argv[6], &max_threads) != 0 ||
			max_threads == 0) {
		fprintf(stderr, "Invalid command line arguments.\n");
		return 1;
	}


	Resources resources = {
    .ram = ram,
    .disk = disk,
    .cpu = cpu
	};

	if(datacenter_configure(&dc, servers, &resources) != 0){
		fprintf(stderr, "Failed to configure Data Center.\n");
		return 1;
	}

	//função no filesystem- converte para absulute path
	if (absolute_path(argv[5], input_dir, MAX_PATH_SIZE) != 0) { // Convert to absolute path and put it in input_dir
		fprintf(stderr, "Failed to convert path to absolute path.\n");
		datacenter_destroy(&dc);
		return 1;
	}

	//função auxiliar só para ver se o path existe
	if (!path_exists(input_dir)) {
		fprintf(stderr, "Path %s doesn't exist or isn't a directory!\n", input_dir);
		datacenter_destroy(&dc);
		return 1;
	}
	
	
	struct dirent **conf_file_list;

	/*uso scandir que é da dirent.h. - ele faz um malloc então tenho de fazer free no final
	*input_dir-> path do input. 
	*conf_file_list-> output de um array de ficheiros filtrados(separado da diretoria orginal). 
	*filter_conf_only-> função que filtra e deixa so os ficheiros .conf. 
	*alphasort-> ordena alfabeticamente*/
	int n_conf_files = scandir(input_dir, &conf_file_list, filter_conf_only, alphasort);
	if (n_conf_files < 0) {
        fprintf(stderr, "Failed to use scandir.\n");
        datacenter_destroy(&dc);
        return 1;
    }

	if(n_conf_files == 0){
		free(conf_file_list);
		datacenter_destroy(&dc);
		return 0;
	}

	sem_t slots;

	if(sem_init(&slots, 0, (unsigned int) max_threads) != 0){
		perror("sem_init error");
		free_conf_list(conf_file_list, n_conf_files);
		data_center_destroy(&dc);
		return 1;
	}

	ConfArgs *args = calloc((size_t) n_conf_files, sizeof(ConfArgs));

	if(args == NULL){
		fprintf(stderr, "Failed to allocate thread arguments.\n");
		sem_destroy(&slots);
		free_conf_list(conf_file_list, n_conf_files);
		datacenter_destroy(&dc);
		return 1;
	}

	pthread_t tid;

	for(int i=0; i < n_conf_files; i++){
		args[i].dc = &dc;
		args[i].slots = &slots;
		snprintf(args[i].path, sizeof(args[i].path), "%s/%s", input_dir, conf_file_list[i]->d_name);
		free(conf_file_list[i]);

		sem_wait(&slots); //Tentar reservar um lugar (bloqueia se o semáforo for 0)

		if(pthread_create(&tid, NULL, run_conf, &args[i]) != 0) {//Tentar criar a thread
			fprintf(stderr, "Failed to create thread for %s.\n", args[i].path);
			sem_post(&slots); //Devolver a vaga que não chegou a ser usada
			continue;
		}

		pthread_detach(tid); //Libertar a thread
	}

	for(size_t i=0; i<max_threads; i++){ //Meter o semáforo a 0 para poder destrui-lo
		sem_wait(&slots);
	}

	sem_destroy(&slots);
	free(args);
	free(conf_file_list);
	data_center_destroy(&dc);
	return 0;
}