#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>//é mesmo preciso usar isto perguntei ao stor
#include <fcntl.h>//é preciso para o open
#include <semaphore.h>
#include <pthread.h>

#include "parser.h"
#include "datacenter.h"
#include "constants.h"
#include "filesystem.h"//tenho de ver se posso aidiconar isto mas ya PERGUNTAR
#include "datacenter_utils.h"// para ter a função do find_pending_reservation(tenho de ver se isto é boa arquitetura tho)


//argumentos de cada thread: uma thread só recebe um void*, por isso junta-se tudo o que ela precisa numa struct
typedef struct {
	DataCenter* dc;
	sem_t* slots;
	char path[MAX_PATH_SIZE + 256];
} ConfArgs;


//processa os comandos de um ficheiro .conf (é o que cada thread corre)
static void process_conf_file(DataCenter* dc, const char* conf_path){
	//abrir o ficheiro .conf para ler e para fechar automaticamente quando o processo filho for criado (O_CLOEXEC)
	int fd = open(conf_path, O_RDONLY | O_CLOEXEC);
	if (fd < 0){
		perror("open error");
		return;
	}

	int done = 0;

	while(!done){
		datacenter_collect_finished_vms(dc, 0);//verifica se há vms que acabaram de correr e destroi-as, 0 para não bloquear
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

				//a copia dos inputs é feita em cada processo filho, antes do exec (spawn_vm_child)
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

	if (argc != 8) {// local_cores (FAQ do stor), input_dir (ex1) e max_threads (ex3)
    fprintf(stderr, "Usage: %s <servers> <ram> <disk> <cpus> <local_cores> <input_dir> <max_threads>\n", argv[0]);
    return 1;
  }

	size_t servers;
	size_t ram;
	size_t disk;
	double cpu;
	size_t local_cores;//numero de cores da máquina local para o CloudIST (FAQ do stor, para o cpulimit)
	char input_dir[MAX_PATH_SIZE];//Adding input_dir
	size_t max_threads;

  	// Verificações
	if (parse_size_t_arg(argv[1], &servers) != 0 ||
			parse_size_t_arg(argv[2], &ram) != 0 ||
			parse_size_t_arg(argv[3], &disk) != 0 ||
			parse_double_arg(argv[4], &cpu) != 0 ||
			parse_size_t_arg(argv[5], &local_cores) != 0 || local_cores == 0 ||
			(argv[6] == NULL || argv[6][0] == '\0') ||
			parse_size_t_arg(argv[7], &max_threads) != 0 ||
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
	dc.local_cores = local_cores;

	//função do stor no filesystem- converte para absulute path
	if (absolute_path(argv[6], input_dir, MAX_PATH_SIZE) != 0) { // Convert to absolute path and put it in input_dir
		fprintf(stderr, "Failed to convert path to absolute path.\n");
		datacenter_destroy(&dc);
		return 1;
	}

	//Uso uma função auxiliar do stor só para ver se o path existe
	if (!path_exists(input_dir)) {
		fprintf(stderr, "Path %s doesn't exist or isn't a directory!\n", input_dir);
		datacenter_destroy(&dc);
		return 1;
	}


	struct dirent **conf_file_list;

	/*uso scandir que é da dirent.h.(o stor disse para usar) - ele faz um malloc então tenho de fazer free no final
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

	//semáforo com max_threads vagas: nunca há mais de max_threads threads ao mesmo tempo
	sem_t slots;

	if(sem_init(&slots, 0, (unsigned int) max_threads) != 0){
		perror("sem_init error");
		free_conf_list(conf_file_list, n_conf_files);
		datacenter_destroy(&dc);
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

	//uma thread por .conf (por ordem alfabética), no máximo max_threads ao mesmo tempo
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

	for(size_t i=0; i<max_threads; i++){ //Meter o semáforo a 0 para poder destrui-lo (= esperar que todas as threads acabem)
		sem_wait(&slots);
	}

	//depois de todas as threads acabarem: esperar (1 = bloquear) que todas as vms acabem e destrui-las, antes de sair
	datacenter_collect_finished_vms(&dc, 1);

	sem_destroy(&slots);
	free(args);
	free(conf_file_list);
	datacenter_destroy(&dc);
	return 0;
}
