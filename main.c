#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>//scandir and struct dirent (list the .conf files)
#include <fcntl.h>//needed for open
#include <semaphore.h>
#include <pthread.h>

#include "parser.h"
#include "datacenter.h"
#include "constants.h"
#include "filesystem.h"//path_exists, absolute_path and filter_conf_only
#include "datacenter_utils.h"


//arguments of each thread: a thread only receives one void*, so everything it needs goes into a struct
typedef struct {
	DataCenter* dc;
	sem_t* slots;
	char path[MAX_PATH_SIZE + 256];
} ConfArgs;


//processes the commands of one .conf file (this is what each thread runs)
static void process_conf_file(DataCenter* dc, const char* conf_path){
	//open the .conf for reading; O_CLOEXEC closes it automatically in the child processes (on exec)
	int fd = open(conf_path, O_RDONLY | O_CLOEXEC);
	if (fd < 0){
		perror("open error");
		return;
	}

	int done = 0;
	//only one R per .conf file, and an A needs that R before it
	int reservation_made = 0;

	while(!done){
		switch(get_next_command(fd)){
			case CMD_DEFINE: {
				VMType vmtype;

				if (parse_define(fd, &vmtype) != 0) {
					fprintf(stderr, "Invalid define command. See H (help) for usage.\n");
					continue;
				}

				//if the D fails, the rest of this .conf would depend on it: skip the rest of the file
				if(datacenter_define_VM(dc, &vmtype) != 0){
					fprintf(stderr, "Failed to define VM. Skipping the rest of %s.\n", conf_path);
					done = 1;
					break;
				}

				datacenter_printf("VM successfully defined!\n");

				break;
			}

			case CMD_RESERVE: {
				Reservation reservation = {0};

				//parse first, even if it is going to be refused, so the line is consumed
				size_t num_items = parse_reserve(fd, &reservation, MAX_RESERVATIONS_ITEMS);

				if (reservation_made) {
					fprintf(stderr, "Only one reservation is allowed per .conf file.\n");
					continue;
				}

				if (num_items == 0) {
					fprintf(stderr, "Invalid reserve command. See H (help) for usage.\n");
					continue;
				}

				//if the R fails, the A that follows has nothing to run: skip the rest of the file
				//(if there are not enough free resources right now, datacenter_reserve waits instead of failing)
				if (datacenter_reserve(dc, &reservation) != 0) {
					fprintf(stderr, "Failed to reserve VMs. Skipping the rest of %s.\n", conf_path);
					done = 1;
					break;
				}

				reservation_made = 1;
				datacenter_printf("Reservation made successfully!\n");

				break;
			}

			case CMD_EXECUTE: {
				char id[MAX_STRING_SIZE];

				//parse first, even if it is going to be refused, so the line is consumed
				if(parse_execute(fd, id) != 0){
					fprintf(stderr, "Invalid execute command. See H (help) for usage.\n");
					continue;
				}

				if (!reservation_made) {
					fprintf(stderr, "No reservation was made in this .conf file before the A command.\n");
					continue;
				}

				//the inputs are copied in each child process, before the exec (spawn_vm_child)
				if (datacenter_execute(dc, id) != 0) {
					fprintf(stderr, "Failed to execute reservation.\n");
					continue;
				}

				datacenter_printf("Finished reservation execution!\n");

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
				datacenter_printf(
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
	//after processing the file, free one slot in the semaphore
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

	if (argc != 8) {// local_cores (teacher's FAQ), input_dir (ex1) and max_threads (ex3)
    fprintf(stderr, "Usage: %s <servers> <ram> <disk> <cpus> <local_cores> <input_dir> <max_threads>\n", argv[0]);
    datacenter_destroy(&dc);
    return 1;
  }

	size_t servers;
	size_t ram;
	size_t disk;
	double cpu;
	size_t local_cores;//cores of the local machine given to CloudIST (teacher's FAQ, for the cpulimit)
	char input_dir[MAX_PATH_SIZE];//folder with the .conf files
	size_t max_threads;

  	// Checks
	if (parse_size_t_arg(argv[1], &servers) != 0 ||
			parse_size_t_arg(argv[2], &ram) != 0 ||
			parse_size_t_arg(argv[3], &disk) != 0 ||
			parse_double_arg(argv[4], &cpu) != 0 ||
			parse_size_t_arg(argv[5], &local_cores) != 0 || local_cores == 0 ||
			(argv[6] == NULL || argv[6][0] == '\0') ||
			parse_size_t_arg(argv[7], &max_threads) != 0 ||
			max_threads == 0) {
		fprintf(stderr, "Invalid command line arguments.\n");
		datacenter_destroy(&dc);
		return 1;
	}


	Resources resources = {
    .ram = ram,
    .disk = disk,
    .cpu = cpu
	};

	if(datacenter_configure(&dc, servers, &resources) != 0){
		fprintf(stderr, "Failed to configure Data Center.\n");
		datacenter_destroy(&dc);
		return 1;
	}
	dc.local_cores = local_cores;

	//base-code function in filesystem.c: converts input_dir to an absolute path
	if (absolute_path(argv[6], input_dir, MAX_PATH_SIZE) != 0) {
		fprintf(stderr, "Failed to convert path to absolute path.\n");
		datacenter_destroy(&dc);
		return 1;
	}

	//base-code helper, just to check the path exists and is a folder
	if (!path_exists(input_dir)) {
		fprintf(stderr, "Path %s doesn't exist or isn't a directory!\n", input_dir);
		datacenter_destroy(&dc);
		return 1;
	}


	struct dirent **conf_file_list;

	/*scandir (dirent.h) does a malloc, so everything has to be freed at the end
	*input_dir-> the input path.
	*conf_file_list-> output: array with the filtered entries (separate from the original folder).
	*filter_conf_only-> filter function that keeps only the .conf files.
	*alphasort-> alphabetical order*/
	int n_conf_files = scandir(input_dir, &conf_file_list, filter_conf_only, alphasort);
	if (n_conf_files < 0) {
        perror("scandir error");
        datacenter_destroy(&dc);
        return 1;
    }

	if(n_conf_files == 0){
		free(conf_file_list);
		datacenter_destroy(&dc);
		return 0;
	}

	//semaphore with max_threads slots: there are never more than max_threads worker threads at the same time
	sem_t slots;

	if(sem_init(&slots, 0, (unsigned int) max_threads) != 0){
		perror("sem_init error");
		free_conf_list(conf_file_list, n_conf_files);
		datacenter_destroy(&dc);
		return 1;
	}

	//each thread needs its own arguments struct, otherwise the loop would overwrite them (race condition)
	ConfArgs *args = calloc((size_t) n_conf_files, sizeof(ConfArgs));

	if(args == NULL){
		fprintf(stderr, "Failed to allocate thread arguments.\n");
		sem_destroy(&slots);
		free_conf_list(conf_file_list, n_conf_files);
		datacenter_destroy(&dc);
		return 1;
	}

	//extra thread (not a worker) that destroys each vm as soon as its process ends
	if (datacenter_start_reaper(&dc) != 0) {
		free(args);
		sem_destroy(&slots);
		free_conf_list(conf_file_list, n_conf_files);
		datacenter_destroy(&dc);
		return 1;
	}

	pthread_t tid;

	//one thread per .conf (in alphabetical order), at most max_threads at the same time
	for(int i=0; i < n_conf_files; i++){
		args[i].dc = &dc;
		args[i].slots = &slots;
		//build the full path of the .conf file (input_dir + file name)
		snprintf(args[i].path, sizeof(args[i].path), "%s/%s", input_dir, conf_file_list[i]->d_name);
		free(conf_file_list[i]);

		sem_wait(&slots); //try to take a slot (blocks if the semaphore is 0)

		if(pthread_create(&tid, NULL, run_conf, &args[i]) != 0) {//try to create the thread
			fprintf(stderr, "Failed to create thread for %s.\n", args[i].path);
			sem_post(&slots); //give back the slot that was never used
			continue;
		}

		pthread_detach(tid); //nobody joins this thread, its resources are freed when it ends
	}

	for(size_t i=0; i<max_threads; i++){ //take every slot back (= wait until every thread has finished), so the semaphore can be destroyed
		sem_wait(&slots);
	}

	sem_destroy(&slots);
	free(args);
	free(conf_file_list);
	//after every thread has finished: datacenter_destroy stops the reaper, which first waits for every vm (child process) to end
	datacenter_destroy(&dc);
	return 0;
}
