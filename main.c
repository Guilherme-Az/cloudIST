#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>//é mesmo preciso usar isto perguntei ao stor
#include <fcntl.h>//é preciso para o open
#include <errno.h>//para o EEXIST do mkdir
#include <sys/stat.h>//para o mkdir

#include "parser.h"
#include "datacenter.h"
#include "constants.h"
#include "filesystem.h"//tenho de ver se posso aidiconar isto mas ya PERGUNTAR
#include "datacenter_utils.h"// para ter a função do find_pending_reservation(tenho de ver se isto é boa arquitetura tho)

int main(int argc, char **argv){
	DataCenter dc;
	datacenter_init(&dc);

	if (argc != 6) {// Adding input_dir 
    fprintf(stderr, "Usage: %s <servers> <ram> <disk> <cpus> <input_dir>\n", argv[0]);
    return 1;
  }

	size_t servers;
	size_t ram;
	size_t disk;
	double cpu;
	char input_dir[MAX_PATH_SIZE];//Adding input_dir

	if (parse_size_t_arg(argv[1], &servers) != 0 ||
			parse_size_t_arg(argv[2], &ram) != 0 ||
			parse_size_t_arg(argv[3], &disk) != 0 ||
			parse_double_arg(argv[4], &cpu) != 0 ||
			(argv[5] == NULL || argv[5][0] == '\0')) {//adicionar aqui esta verificação
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

	//função do stor no filesystem- converte para absulute path
	if (absolute_path(argv[5], input_dir, MAX_PATH_SIZE) != 0) { // Convert to absolute path and put it in input_dir
		fprintf(stderr, "Failed to convert path to absolute path.\n");
		datacenter_destroy(&dc);
		return 1;
	}
	printf("absulute path %s\n", input_dir);
	//Uso uma função auxiliar do stor só para ver se o path existe
	if (!path_exists(input_dir)) {
		fprintf(stderr, "Path %s doesn't exist or isn't a directory!\n", input_dir);
		datacenter_destroy(&dc);
		return 1;
	}
	
	//A blibioteca dirent.h é posix , mas tenho de o stor disse que era para usar isto 
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
	

	//novo loop para ler os ficheiros .conf do conf_file_list
	for (int i = 0; i < n_conf_files; i++) {
		char conf_path[MAX_PATH_SIZE +256]; //ele não deixa compilar se não adicionar um bocado 	

		//PERGUNTAR ao stor se posso usar snprintf(acho que sim) é preciso para juntar o conf_file_list de já filtrado ao path do input_dir
		/*conf_path->novo buffer para guardar o caminho do ficheiro. 
		*max_path_size-> tamanho maximo do path(o linux tem um definido). 
		*input_dir-> caminho da diretoria de input(primeiro s). 
		*conf_file_list[i]->d_name-> nome do ficheiro .conf filtrado(segundo s)*/ 
		snprintf(conf_path, sizeof(conf_path), "%s/%s", input_dir, conf_file_list[i]->d_name); // Prepend input_dir to filename

		//abrir o ficheiro .conf para ler e para fechar automaticamente quando o processo filho for criado (O_CLOEXEC)
		int fd = open(conf_path, O_RDONLY | O_CLOEXEC);

		if (fd < 0) {
        	perror("open error");
			//as entradas antes de i já foram libertadas no loop, falta a atual e as seguintes
			for (int j = i; j < n_conf_files; j++) {
				free(conf_file_list[j]);
			}
			free(conf_file_list);
			datacenter_collect_finished_vms(&dc, 1);//esperar pelas vms de .confs anteriores que ainda estejam a correr
			datacenter_destroy(&dc);
        	return 1;
    	}
		int done = 0;
		while(!done){ 
			datacenter_collect_finished_vms(&dc, 0);//verifica se há vms que acabaram de correr e destroi-as, 0 para não bloquear o parent process
			switch (get_next_command(fd)){
				case CMD_DEFINE: {
					VMType vmtype;

					if (parse_define(fd, &vmtype) != 0) {
						fprintf(stderr, "Invalid define command. See H (help) for usage.\n");
						continue;
					}

					if(datacenter_define_VM(&dc, &vmtype) != 0){
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

					if (datacenter_reserve(&dc, &reservation) != 0) {
						fprintf(stderr, "Failed to reserve VMs.\n");
						continue;
					}

					printf("Reservation made successfully!\n");

					break;
				}

				case CMD_EXECUTE:{
					char id[MAX_STRING_SIZE];

					// esta parte era do codigo do stor vou meter antes do meu para ter o id da reserva
					if(parse_execute(fd, id) != 0){
						fprintf(stderr, "Invalid execute command. See H (help) for usage.\n");
						continue;
					}


					if (datacenter_execute(&dc, id) != 0) {
						fprintf(stderr, "Failed to execute reservation.\n");
						continue;
					}

					printf("Finished reservation execution!\n");

					break;
				}
				case CMD_LIST:
					if (datacenter_list(&dc) != 0) {
						fprintf(stderr, "Failed to list VMs.\n");
						continue;
					}

					break;

				case CMD_WAIT:
					unsigned int delay;

					if(parse_wait(fd, &delay) != 0){
						fprintf(stderr, "Invalid wait command. See H (help) for usage.\n");
						continue;
					}

					datacenter_wait(delay);
					break;

				case CMD_INVALID:
					fprintf(stderr, "Invalid Command. See H (help) for usage.\n");
					break;

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
			}

			
		}
		close(fd);
		free(conf_file_list[i]); //libertar a memoria alocada pelo scandir
	}
	//depois de todos os .conf esperar (1 = bloquear) que todas as vms acabem e destrui-las, antes de sair
	datacenter_collect_finished_vms(&dc, 1);
	datacenter_destroy(&dc);
	free(conf_file_list);
	return 0; //libertar a memoria alocada pelo scandir
}