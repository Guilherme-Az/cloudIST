#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>//é mesmo preciso usar isto perguntei ao stor

#include "parser.h"
#include "datacenter.h"
#include "constants.h"
#include "filesystem.h"//tenho de ver se posso aidiconar isto mas ya PERGUNTAR

int main(int argc, char **argv){
	DataCenter dc;
	datacenter_init(&dc);

	if (argc != 5) {// Adding input_dir 
    fprintf(stderr, "Usage: %s <servers> <ram> <disk> <cpus> <input_dir>\n", argv[0]);
    return 1;
  }

	size_t servers;
	size_t ram;
	size_t disk;
	double cpu;
	char input_dir[MAX_STRING_SIZE];//Adding input_dir

	if (parse_size_t_arg(argv[1], &servers) != 0 ||
			parse_size_t_arg(argv[2], &ram) != 0 ||
			parse_size_t_arg(argv[3], &disk) != 0 ||
			parse_double_arg(argv[4], &cpu) != 0 ||
			parse_string_arg(argv[5], input_dir, sizeof(input_dir)) != 0) {
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

	//Uso uma função auxiliar do stor só para ver se o path existe
	if (!path_exists(input_dir)) {
		fprintf(stderr, "Path %s doesn't exist or isn't a directory!\n", input_dir);
		return 1;
	}
	
	//A blibioteca dirent.h é posix , mas tenho de o stor disse que era para usar isto 
	struct dirent **conf_file_list;

	/*uso candir que é da dirent.h. input_dir-> path do input. 
	*conf_file_list-> output de um array de ficheiros filtrados(separado da diretoria orginal). 
	*filter_conf_only-> função que filtra e deixa so os ficheiros .conf. 
	*alphasort-> ordena alfabeticamente*/
	int n_conf_files = scandir(input_dir, &conf_file_list, filter_conf_only, alphasort); // 

	//novo loop para ler os ficheiros .conf do conf_file_list
	for (int i = 0; i < n_conf_files; i++) {
		char path_buffer[MAX_PATH_SIZE];
		char absolute_path_buffer[MAX_PATH_SIZE];	

		//PERGUNTAR ao stor se posso usar snprintf(acho que sim) é preciso para juntar o conf_file_list de já filtrado ao path do input_dir
		/*path_buffer->novo buffer para guardar o caminho do ficheiro. 
		*max_path_size-> tamanho maximo do path(o linux tem um defenido). 
		*input_dir-> caminho da diretoria de input(primeiro s). 
		*conf_file_list[i]->d_name-> nome do ficheiro .
		*conf filtrado(segundo s)*/ 
		snprintf(path_buffer, MAX_PATH_SIZE, "%s/%s", input_dir, conf_file_list[i]->d_name); // Prepend input_dir to filename

		if (absolute_path(path_buffer, absolute_path_buffer, MAX_PATH_SIZE) != 0) { // Convert to absolute path
			fprintf(stderr, "Failed to convert path to absolute path.\n");
			return 1;
		}

		int fd = open(absolute_path_buffer, O_RDONLY);

		if (fd < 0) {
        	perror("open error");
        	return 1;
    	}

		while(1) {
			
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

					size_t num_items = parse_reserve(STDIN_FILENO, &reservation, MAX_RESERVATIONS_ITEMS);

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

				case CMD_EXECUTE:
					char id[MAX_STRING_SIZE];


					//Copiar atodo o conteundo das diretioria de inputs (comandos D)para o caminho da maquina virtual e chamar recursivamente antes da execução
					
					Reservation *reservation = &dc.reservations[0]; //só há uma reseva por conf file
					//loop pelos items de reserva
					for (size_t i = 0; i < reservation->items_count; i++){
						ReservationItem *reservation_item = &reservation->items[i];
						//loop pelos vms de cada item de reserva 
						for (size_t j = 0; j < reservation_item->amount; j++) {
							
							char *source_input_path = reservation_item->vm_type->input;
						
							char vm_input_path[MAX_PATH_SIZE];

							// Construir o caminho de destino para a VM
							snprintf(vm_input_path, MAX_PATH_SIZE, "/tmp/CloudIST/%s/%s",reservation_item->server_id, vm->id);

					
							if(copy_path_recursive(source_input_path, vm_input_path) != 0){
								fprintf(stderr, "Failed to copy input folder for VM %s.\n", vm->id);
								continue;
							}
						}

					}



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

				case CMD_LIST:
					if (datacenter_list(&dc) != 0) {
						fprintf(stderr, "Failed to list VMs.\n");
						continue;
					}

					break;

				case CMD_WAIT:
					unsigned int delay;

					if(parse_wait(STDIN_FILENO, &delay) != 0){
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
					datacenter_destroy(&dc);
					return 0;
			}

			
		}
	}
}