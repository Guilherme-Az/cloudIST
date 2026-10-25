#include "datacenter_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/resource.h>

VMType *VMType_exists(DataCenter *dc, const char* type_id){
	for(size_t i = 0; i < dc->num_vm_types; i++){
		if(strncmp(type_id, dc->vm_types[i].id, MAX_STRING_SIZE) == 0){
			return &dc->vm_types[i];
		}
	}
	return NULL;
}

/**
 * Checks if a reservation with the given id already exists in the Data Center.
 * 
 * @param dc Pointer to Data Center.
 * @param reservation_id Id of the reservation being checked.
 * 
 * @return 1 if the reservation exists.
 * @return 0 if the reservation does not exist.
 */	
int reservation_exists(DataCenter *dc, const char* reservation_id){
	for(size_t i = 0; i < dc->num_reservations; i++){
		if(strncmp(reservation_id, dc->reservations[i].id, MAX_STRING_SIZE) == 0){
			return 1;
		}
	}
	return 0;
}

int reservation_validate(DataCenter *dc, Reservation *reservation) {
	//(a verificação do id repetido passou para o datacenter_reserve, porque lê o array
	//das reservas e precisa do lock das reservas, que não se tranca aqui)

	Resources available[dc->num_servers];
	size_t hosted_count[dc->num_servers];
	size_t vms_count = 0;

	/* Copy current server resources and hosted_vms so we can simulate the allocation.
	 * Só dos servidores usados pela reserva: são os únicos trancados (lock_reservation_servers),
	 * ler os outros seria uma data race (e o valor nem é usado). */
	for (size_t i = 0; i < reservation->items_count; i++) {
		size_t id = reservation->items[i].server_id;
		if (id >= 1 && id <= dc->num_servers) {
			available[id - 1] = dc->servers[id - 1].available;
			hosted_count[id - 1] = dc->servers[id - 1].num_hosted_vms;
		}
	}

	for (size_t i = 0; i < reservation->items_count; i++) {
		ReservationItem *item = &reservation->items[i];

		if (item->amount == 0) {
			fprintf(stderr, "Reservation item must request at least one VM.\n");
			return 1;
		}

		VMType *vm_type = VMType_exists(dc, item->vm_type_id);

		if(vm_type == NULL){
			fprintf(stderr, "VM of type %s does not exist\n", item->vm_type_id);
			return 1;
		}

		if (item->server_id == 0 || item->server_id > dc->num_servers) {
			fprintf(stderr, "Invalid server in reservation.\n");
			return 1;
		}

		for (size_t j = 0; j < item->amount; j++) {
			if(vms_count >= MAX_RESERVATION_VMS){
				fprintf(stderr, "Reached max of VMS for a reservation.\n");
				return 1;
			}
			
			if (hosted_count[item->server_id - 1] >= MAX_HOSTED_VMS) {
        fprintf(stderr,
                "Server \"%zu\" reached the maximum number of hosted VMs.\n",
                item->server_id);
        return 1;
    	}

			if (!resources_can_fit(available[item->server_id-1], vm_type->required)) {
				fprintf(stderr, "Server \"%zu\" does not have enough resources.\n", item->server_id);
				return 1;
			}

			resources_sub(&available[item->server_id-1], vm_type->required);
			hosted_count[item->server_id-1]++;
			vms_count++; // One VM "reserved"
		}

		// Attach vm_type to item for later use.
		item->vm_type = vm_type;
	}

	return 0;
}

/**
 * Rolls back the changes made by a reservation operation.
 * 
 * @param reservation Pointer to the reservation being rolled back.
 * @param initial_vms Number of VMs that existed before the operation.
 */
void reservation_rollback(Reservation *reservation, size_t initial_vms){
	while (reservation->num_vms > initial_vms) {
		VM *vm = reservation->vms[reservation->num_vms - 1];

		Server *server = vm->server;

		// Restore resources
		resources_add(&server->available, vm->type->required);

		// Remove VM from server hosted list
		server->num_hosted_vms--;

		server->hosted_vms[server->num_hosted_vms] = NULL;

		// Remove VM from reservation
		reservation->num_vms--;

		reservation->vms[reservation->num_vms] = NULL;

		free(vm);
	}
}

int reservation_commit(DataCenter *dc, Reservation *reservation){
	// Keep track of how many vms existed before.
	size_t initial_vms = reservation->num_vms;

	for (size_t i = 0; i < reservation->items_count; i++) {
		ReservationItem *item = &reservation->items[i];
		Server *server = &dc->servers[item->server_id - 1];

		for (size_t j = 0; j < item->amount; j++) {
			VM *new_vm = calloc(1, sizeof(VM));
			if (new_vm == NULL) {
				fprintf(stderr, "Failed to allocate VM.\n");
				reservation_rollback(reservation, initial_vms);
				return 1;
			}

			snprintf(new_vm->id,
				MAX_VM_ID_STRING,
				"%s_%zu",
				reservation->id,
				reservation->num_vms + 1
			);

			new_vm->type = item->vm_type;
			new_vm->server = server;
			new_vm->state = VM_STATE_RESERVED;

			resources_sub(&server->available, item->vm_type->required);

			server->hosted_vms[server->num_hosted_vms++] = new_vm;
			reservation->vms[reservation->num_vms++] = new_vm;
		}
	}

	reservation->state = RES_STATE_PENDING;

	return 0;
}

Reservation *find_pending_reservation(DataCenter *dc, const char *reservation_id) {
	Reservation *res = NULL;

	for (size_t i = 0; i < dc->num_reservations; i++) {
		if (strcmp(dc->reservations[i].id, reservation_id) == 0) {
			res = &dc->reservations[i];
			break;
		}
	}

	if (!res) {
		fprintf(stderr, "Reservation ID %s not found.\n", reservation_id);
		return NULL;
	}

	if (res->state != RES_STATE_PENDING) {
		fprintf(stderr, "Reservation ID %s is not in a pending state.\n", reservation_id);
		return NULL;
	}

	return res;
}

void reservation_destroy(DataCenter *dc, Reservation *reservation) {
	// Free every VM belonging to the reservation.
	for (size_t i = 0; i < reservation->num_vms; i++) {
		VM *vm = reservation->vms[i];
		Server *server = vm->server;

		// Return the VM resources back to the hosting server.
		resources_add(&server->available, vm->type->required);

		// Remove the VM from the server's hosted VM list.
		for (size_t j = 0; j < server->num_hosted_vms; j++) {
			if (server->hosted_vms[j] == vm) {
				// Shift the remaining VMs one position to the left.
				memmove(&server->hosted_vms[j],
								&server->hosted_vms[j + 1],
								(server->num_hosted_vms - j - 1) * sizeof(VM *));

				server->num_hosted_vms--;
				break;
			}
		}

		free(vm);
		reservation->vms[i] = NULL;
	}

	reservation->num_vms = 0;

	// Find the reservation inside the Data Center reservation list.
	size_t idx;
	for (idx = 0; idx < dc->num_reservations; idx++) {
			if (&dc->reservations[idx] == reservation)
				break;
	}

	// Reservation not found (should never happen).
	if (idx == dc->num_reservations)
		return;

	// Remove the reservation by shifting the remaining ones.
	memmove(&dc->reservations[idx],
					&dc->reservations[idx + 1],
					(dc->num_reservations - idx - 1) * sizeof(Reservation));

	dc->num_reservations--;
}

void spawn_vm_child(VM *vm, const char *reservation_id, double cpu_percentage) {

	//prepara a pasta da vm (criar, limpar, copiar inputs) antes dos limites, para a copia não ficar limitada
	//se falhar, o filho sai sem correr a app (a vm acaba logo e é destruida como as outras)
	if (make_vm_folder_path(vm->type->input_folder, reservation_id, vm->id) != 0) {
		fprintf(stderr, "Failed to prepare input folder for VM %s.\n", vm->id);
		_exit(1);
	}

	/*está no enunciado usarmos esta estrutura do rlimit,
	serve para(através do kernerl) definir limites aos recursos que um processo pode usar*/
	struct rlimit mem, disk;

	//multiplica a ram da vm que está em GB por 2^30 para converter para bytes, e converte para o tipo rlim_t que é o tipo que a struct rlimit espera
	mem.rlim_cur = (rlim_t)vm->type->required.ram << 30;
	mem.rlim_max = mem.rlim_cur;

	disk.rlim_cur = (rlim_t)vm->type->required.disk << 30;
	disk.rlim_max = disk.rlim_cur;

	if (setrlimit(RLIMIT_AS, &mem) == -1) {
		perror("setrlimit mem");
		_exit(1); //sem limit de memoria nao arranca a vm
	}
	if (setrlimit(RLIMIT_FSIZE, &disk) == -1) {
		perror("setrlimit disk");
		_exit(1); //sem limit de tamanho de ficheiro nao arranca a vm
	}

	//o cpulimit -l só aceita inteiros: truncar (8.33 -> 8) e no minimo 1 (VMs muito pequenas davam 0)
	int cpu_limit = (int)cpu_percentage;
	if (cpu_limit < 1) {
		cpu_limit = 1;
	}
	//o exec só recebe strings, por isso converte-se o numero para texto
	char cpu_limit_str[32];
	snprintf(cpu_limit_str, sizeof(cpu_limit_str), "%d", cpu_limit);

	//corre o exec_path do vm através do cpulimit (enunciado: cpulimit -q -f -l <CPULIMIT> -- <EXEC_PATH>)
	//execlp com p porque procura o cpulimit no PATH; a app vai depois do --
	execlp("cpulimit", "cpulimit", "-q", "-f", "-l", cpu_limit_str, "--", vm->type->exec_path, (char *)NULL);
	perror("exec cpulimit");
	_exit(1);

}

int spawn_all_vms(DataCenter *dc, Reservation *res) {
	pid_t child_pid;

	//loop por cada vm da reserva e cria um child process para cada uma
	for (size_t i = 0; i < res->num_vms; i++) {
		VM *vm = res->vms[i];

		//% de cpu para o cpulimit de cada processo child
		//100 * vCPUs da vm * cores locais / (CPUs por servidor * nº de servidores)
		double cpu_percentage = 100.0 * vm->type->required.cpu * (double)dc->local_cores
		                        / (vm->server->total.cpu * (double)dc->num_servers);

		fflush(stdout);//não é necessário agora, mas básicamente garante que o que está no buffer de stdout é escrito antes de criar o child process, porque o child process herda o buffer do pai e se não se fizer flush, o que estava no buffer do pai vai ser escrito pelo child process também, duplicando a escrita
		//fork: -1 erro, 0 child, default >= parent process (pid do child),
		switch(child_pid = fork()){
			case -1:
				perror("fork error");
				return 1;
			//child process, chama a função para executar a vm
			case 0:
				spawn_vm_child(vm, res->id, cpu_percentage);
				_exit(1); //usar o _exit em vez de exit porque o exit limpa tudo e flushes buffers  enquanto o _exit apenas sai do processo sem limpar, que é o que queres de um child process
			//só o pai é que continua a correr o loop para criar outras children
			default:
				vm->pid = child_pid;
				vm->state = VM_STATE_RUNNING;
				break;
		}

	}

	return 0;
}

void wait_for_all_vms(Reservation *res) {
	for (size_t i = 0; i < res->num_vms; i++) {
		// TODO: IMPLEMENT WAITING FOR VM

		res->vms[i]->state = VM_STATE_TERMINATED;
	}
}

const char *vm_state_to_string(VMState state) {
    switch (state) {
        case VM_STATE_RESERVED:   return "RESERVED";
        case VM_STATE_RUNNING:    return "RUNNING";
        case VM_STATE_TERMINATED: return "TERMINATED";
        default:                  return "UNKNOWN";
    }
}

const char *res_state_to_string(ReservationState state) {
    switch (state) {
			case RES_STATE_PENDING:  return "PENDING";
			case RES_STATE_RUNNING:  return "RUNNING";
			case RES_STATE_FINISHED: return "FINISHED";
			default:                 return "UNKNOWN";
    }
}

/**
 * Marca em used[] (com 1) os servidores usados pelos items da reserva.
 * Ids de servidor inválidos são ignorados (o reservation_validate rejeita-os depois).
 */
static void mark_reservation_servers(DataCenter *dc, Reservation *res, char *used) {
	memset(used, 0, dc->num_servers);
	for (size_t i = 0; i < res->items_count; i++) {
		size_t id = res->items[i].server_id;
		if (id >= 1 && id <= dc->num_servers) {
			used[id - 1] = 1;
		}
	}
}

void lock_reservation_servers(DataCenter *dc, Reservation *res) {
	char used[dc->num_servers];
	mark_reservation_servers(dc, res, used);
	//o for vai do servidor 1 para cima -> tranca sempre por ordem crescente de id (evita deadlock)
	for (size_t i = 0; i < dc->num_servers; i++) {
		if (used[i]) {
			pthread_mutex_lock(&dc->servers[i].server_lock);
		}
	}
}

void unlock_reservation_servers(DataCenter *dc, Reservation *res) {
	char used[dc->num_servers];
	mark_reservation_servers(dc, res, used);
	for (size_t i = dc->num_servers; i > 0; i--) {
		if (used[i - 1]) {
			pthread_mutex_unlock(&dc->servers[i - 1].server_lock);
		}
	}
}

void destroy_vm_by_pid(DataCenter *dc, pid_t pid) {
	//lock das reservas: vamos procurar no array e talvez apagar uma reserva (memmove)
	//(também garante que o spawn já guardou o pid desta vm: o spawn tem este lock durante o fork)
	pthread_mutex_lock(&dc->reservations_lock);

	//procurar por todas as reservas a vm que tem este pid(podem estar várias a correr em pararelo)
	for (size_t i = 0; i < dc->num_reservations; i++) {
		Reservation *res = &dc->reservations[i];

		for (size_t k = 0; k < res->num_vms; k++) {
			VM *vm = res->vms[k];
			if (vm->pid != pid) {
				continue;
			}

			Server *server = vm->server;

			//lock do servidor desta vm (ordem: reservas -> servidor)
			pthread_mutex_lock(&server->server_lock);

			//igual a reservation_destroy básicamente aqueles recursos que foram removidos pelo resources_sub no  reservation_commit, após o comando R(reserve), são devolvidos ao server
			resources_add(&server->available, vm->type->required);

			//tirar a vm da lista do servidor (igual ao reservation_destroy)
			for (size_t j = 0; j < server->num_hosted_vms; j++) {
				if (server->hosted_vms[j] == vm) {
					/*igual ao reservation_destroy, com é uma lista ligada puxar as seguintes vms uma posição para trás, 
					a partir da vm que se quer retirar
					*memmove(destino, origem, numero de elementos depois da posição j*tamanho_do_elemento)*/
					memmove(&server->hosted_vms[j],
									&server->hosted_vms[j + 1],
									(server->num_hosted_vms - j - 1) * sizeof(VM *));
					server->num_hosted_vms--;
					break;
				}
			}

			pthread_mutex_unlock(&server->server_lock);

			//tirar a vm da lista da reserva (puxar as seguintes uma posição para trás)
			memmove(&res->vms[k],
							&res->vms[k + 1],
							(res->num_vms - k - 1) * sizeof(VM *));
			res->num_vms--;

			free(vm);

			//se era a ultima vm da reserva, apaga a reserva
			//(com 0 vms o reservation_destroy só tira a reserva do array do dc)
			if (res->num_vms == 0) {
				reservation_destroy(dc, res);
			}

			//já encontrámos a vm, depois do reservation_destroy o ponteiro res já não é valido
			pthread_mutex_unlock(&dc->reservations_lock);
			return;
		}
	}

	pthread_mutex_unlock(&dc->reservations_lock);
}