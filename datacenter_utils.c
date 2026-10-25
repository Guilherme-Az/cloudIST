#include "datacenter_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <fcntl.h>
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

int reservation_exists(DataCenter *dc, const char* reservation_id){
	for(size_t i = 0; i < dc->num_reservations; i++){
		if(strncmp(reservation_id, dc->reservations[i].id, MAX_STRING_SIZE) == 0){
			return 1;
		}
	}
	return 0;
}

int reservation_validate(DataCenter *dc, Reservation *reservation) {
	//(the repeated id check is in datacenter_reserve, because it reads the reservations
	//list and needs the reservations lock, which is not held here)

	/* Simulate the allocation on each server's TOTAL resources: if it doesn't fit even on
	 * empty servers it can never fit (fail now); if it does, the R may wait for free resources.
	 * total never changes after datacenter_configure, so it can be read without the server locks. */
	Resources total_left[dc->num_servers];
	size_t hosted_count[dc->num_servers];
	size_t vms_count = 0;

	for (size_t i = 0; i < dc->num_servers; i++) {
		total_left[i] = dc->servers[i].total;
		hosted_count[i] = 0;
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
				        "Server \"%zu\" can't host that many VMs.\n",
				        item->server_id);
				return 1;
			}

			if (!resources_can_fit(total_left[item->server_id-1], vm_type->required)) {
				fprintf(stderr, "Server \"%zu\" does not have enough total resources for this reservation.\n", item->server_id);
				return 1;
			}

			resources_sub(&total_left[item->server_id-1], vm_type->required);
			hosted_count[item->server_id-1]++;
			vms_count++; // One VM "reserved"
		}

		// Attach vm_type to item for later use (types are never moved or deleted, so the pointer stays valid).
		item->vm_type = vm_type;
	}

	return 0;
}

int reservation_fits_now(DataCenter *dc, Reservation *reservation) {
	Resources available[dc->num_servers];
	size_t hosted_count[dc->num_servers];

	/* Copy the current available resources and hosted vms, so we can simulate the allocation.
	 * Only the servers used by the reservation: they are the only ones locked
	 * (lock_reservation_servers); reading the others would be a data race. */
	for (size_t i = 0; i < reservation->items_count; i++) {
		size_t id = reservation->items[i].server_id;
		available[id - 1] = dc->servers[id - 1].available;
		hosted_count[id - 1] = dc->servers[id - 1].num_hosted_vms;
	}

	for (size_t i = 0; i < reservation->items_count; i++) {
		ReservationItem *item = &reservation->items[i];
		size_t idx = item->server_id - 1;

		for (size_t j = 0; j < item->amount; j++) {
			if (hosted_count[idx] >= MAX_HOSTED_VMS || !resources_can_fit(available[idx], item->vm_type->required)) {
				return 0;
			}
			resources_sub(&available[idx], item->vm_type->required);
			hosted_count[idx]++;
		}
	}

	return 1;
}

void notify_resources_freed(DataCenter *dc) {
	//new version -> every R waiting in datacenter_reserve wakes up and tries again
	pthread_mutex_lock(&dc->resources_lock);
	dc->resources_version++;
	pthread_cond_broadcast(&dc->resources_cond);
	pthread_mutex_unlock(&dc->resources_lock);
}

void reservation_rollback(Reservation *reservation, size_t initial_vms){
	while (reservation->num_vms > initial_vms) {
		VM *vm = reservation->vms[reservation->num_vms - 1];

		Server *server = vm->server;

		// Restore resources
		resources_add(&server->available, vm->type->required);

		// Remove VM from server hosted list.
		// Look for the vm instead of assuming it is the last one: with threads, between the commit and the
		// rollback (phase 3 of datacenter_reserve) another reservation may have added vms to this server
		for (size_t j = 0; j < server->num_hosted_vms; j++) {
			if (server->hosted_vms[j] == vm) {
				memmove(&server->hosted_vms[j],
								&server->hosted_vms[j + 1],
								(server->num_hosted_vms - j - 1) * sizeof(VM *));
				server->num_hosted_vms--;
				server->hosted_vms[server->num_hosted_vms] = NULL;
				break;
			}
		}

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

/**
 * CPU limit (cpulimit -l) of a VM, teacher's FAQ formula:
 *   CPULIMIT = 100 * VM vCPUs * local cores / (CPUs per server * number of servers)
 * Rounded to the nearest integer (cpulimit only takes integers), at least 1
 * (very small VMs would get 0) and at most 100 * local cores (the whole share
 * of the local machine given to CloudIST). It may be above 100.
 *
 * @param dc Pointer to the Data Center.
 * @param vm VM being launched.
 *
 * @return The value for cpulimit -l.
 */
static long vm_cpu_limit(DataCenter *dc, VM *vm) {
	double limit = 100.0 * vm->type->required.cpu * (double)dc->local_cores
	               / (vm->server->total.cpu * (double)dc->num_servers);
	long max_limit = 100 * (long)dc->local_cores;
	long rounded = lround(limit);

	if (rounded < 1) {
		return 1;
	}
	if (rounded > max_limit) {
		return max_limit;
	}
	return rounded;
}

void spawn_vm_child(VM *vm, const char *reservation_id, long cpu_limit) {

	//prepare the vm folder (create, clean, copy inputs) before the limits, so the copy isn't limited
	//if it fails, the child leaves without running the app (the vm ends right away and is destroyed like the others)
	char vm_dir[MAX_PATH_SIZE];
	if (make_vm_folder_path(vm->type->input_folder, reservation_id, vm->id, vm_dir, sizeof(vm_dir)) != 0) {
		fprintf(stderr, "Failed to prepare input folder for VM %s.\n", vm->id);
		_exit(1);
	}

	//the app runs inside the vm folder (chdir below), so a relative exec_path from the D command
	//would stop working: turn it into an absolute path first, while we are still in the original folder
	char exec_path[MAX_PATH_SIZE];
	if (absolute_path(vm->type->exec_path, exec_path, sizeof(exec_path)) != 0) {
		fprintf(stderr, "Failed to get the absolute path of the executable of VM %s.\n", vm->id);
		_exit(1);
	}

	/*the rlimit struct is the one from the project statement,
	it is used (through the kernel) to set limits on the resources a process can use*/
	struct rlimit mem, disk;

	//the vm ram is in GB: shift by 30 (2^30) to get bytes, and cast to rlim_t, the type the rlimit struct expects
	mem.rlim_cur = (rlim_t)vm->type->required.ram << 30;
	mem.rlim_max = mem.rlim_cur;

	disk.rlim_cur = (rlim_t)vm->type->required.disk << 30;
	disk.rlim_max = disk.rlim_cur;

	if (setrlimit(RLIMIT_AS, &mem) == -1) {
		perror("setrlimit mem");
		_exit(1); //without the memory limit the vm doesn't start
	}
	if (setrlimit(RLIMIT_FSIZE, &disk) == -1) {
		perror("setrlimit disk");
		_exit(1); //without the file size limit the vm doesn't start
	}

	//the app's working directory becomes its own vm folder (/tmp/CloudIST-114255/<reservation>/<vm>),
	//so it finds its input files with relative paths
	if (chdir(vm_dir) != 0) {
		perror("chdir to the vm folder");
		_exit(1);
	}

	//the app's normal output (stdout) goes to /dev/null, so it doesn't mix with CloudIST's output;
	//dup2 makes fd 1 (stdout) point to /dev/null. stderr stays, so the app's errors are still visible
	int null_fd = open("/dev/null", O_WRONLY);
	if (null_fd < 0) {
		perror("open /dev/null");
		_exit(1);
	}
	if (dup2(null_fd, STDOUT_FILENO) < 0) {
		perror("dup2 stdout");
		_exit(1);
	}
	close(null_fd);

	//exec only takes strings, so the number is converted to text
	char cpu_limit_str[32];
	snprintf(cpu_limit_str, sizeof(cpu_limit_str), "%ld", cpu_limit);

	//run the vm's exec_path through cpulimit (statement: cpulimit -q -f -l <CPULIMIT> -- <EXEC_PATH>)
	//execlp with p because it looks for cpulimit in the PATH; the app goes after the --
	execlp("cpulimit", "cpulimit", "-q", "-f", "-l", cpu_limit_str, "--", exec_path, (char *)NULL);
	perror("exec cpulimit");
	_exit(1);

}

int spawn_all_vms(DataCenter *dc, Reservation *res) {
	pid_t child_pid;

	//loop through every vm of the reservation and create a child process for each one
	for (size_t i = 0; i < res->num_vms; i++) {
		VM *vm = res->vms[i];

		long cpu_limit = vm_cpu_limit(dc, vm);

		fflush(stdout);//makes sure what is in the stdout buffer is written before creating the child process: the child inherits the parent's buffer, and without the flush it would write it again (duplicated output)
		//fork: -1 error, 0 child, default parent process (with the child's pid)
		switch(child_pid = fork()){
			case -1:
				perror("fork error");
				return 1;
			//child process: call the function that runs the vm
			case 0:
				spawn_vm_child(vm, res->id, cpu_limit);
				_exit(1); //_exit instead of exit: exit cleans everything up and flushes the buffers, _exit just leaves the process, which is what you want in a child process
			//only the parent keeps running the loop to create the other children
			default:
				vm->pid = child_pid;
				vm->state = VM_STATE_RUNNING;
				//one more vm process: wake the reaper up if it was waiting for vms
				//(we are holding the reservations lock, from datacenter_execute)
				dc->live_vms++;
				pthread_cond_signal(&dc->vms_cond);
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
 * Marks in used[] (with 1) the servers used by the reservation's items.
 * Invalid server ids are ignored (reservation_validate rejects them).
 *
 * @param dc Pointer to the Data Center.
 * @param res Reservation whose servers are marked.
 * @param used Array with dc->num_servers positions.
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
	//the for goes from server 1 upwards -> always locks in increasing id order (avoids deadlocks)
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
	//reservations lock: we search the list and may delete a reservation (memmove)
	//(it also guarantees the spawn already stored this vm's pid: the spawn holds this lock during the fork)
	pthread_mutex_lock(&dc->reservations_lock);

	//look through every reservation for the vm with this pid (several can be running in parallel)
	for (size_t i = 0; i < dc->num_reservations; i++) {
		Reservation *res = &dc->reservations[i];

		for (size_t k = 0; k < res->num_vms; k++) {
			VM *vm = res->vms[k];
			if (vm->pid != pid) {
				continue;
			}

			Server *server = vm->server;

			//lock of this vm's server (order: reservations -> server)
			pthread_mutex_lock(&server->server_lock);

			//same as reservation_destroy: the resources removed by resources_sub in reservation_commit (R command) go back to the server
			resources_add(&server->available, vm->type->required);

			//remove the vm from the server's list (same as reservation_destroy)
			for (size_t j = 0; j < server->num_hosted_vms; j++) {
				if (server->hosted_vms[j] == vm) {
					/*same as reservation_destroy: it's an array, so pull the following vms one position back,
					starting at the vm we want to remove
					*memmove(destination, source, number of elements after position j * element size)*/
					memmove(&server->hosted_vms[j],
									&server->hosted_vms[j + 1],
									(server->num_hosted_vms - j - 1) * sizeof(VM *));
					server->num_hosted_vms--;
					break;
				}
			}

			pthread_mutex_unlock(&server->server_lock);

			//resources were given back: wake up any R waiting for free resources
			notify_resources_freed(dc);

			//the vm ended: delete its folder (/tmp/CloudIST-114255/<reservation>/<vm>) with everything inside
			remove_vm_folder(res->id, vm->id);

			//remove the vm from the reservation's list (pull the following ones one position back)
			memmove(&res->vms[k],
							&res->vms[k + 1],
							(res->num_vms - k - 1) * sizeof(VM *));
			res->num_vms--;

			free(vm);

			//if it was the last vm of the reservation, delete the reservation and its (now empty) folder
			//(with 0 vms reservation_destroy only removes the reservation from the dc list)
			if (res->num_vms == 0) {
				char reservation_id[MAX_STRING_SIZE];
				//copy the id first: after reservation_destroy, res points to another reservation
				strncpy(reservation_id, res->id, MAX_STRING_SIZE);
				reservation_id[MAX_STRING_SIZE - 1] = '\0';

				reservation_destroy(dc, res);
				remove_reservation_folder(reservation_id);
			}

			//we found the vm; after reservation_destroy the res pointer is no longer valid
			pthread_mutex_unlock(&dc->reservations_lock);
			return;
		}
	}

	pthread_mutex_unlock(&dc->reservations_lock);
}
