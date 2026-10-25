#include "datacenter.h"
#include "datacenter_utils.h"

#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/wait.h>

//only one thread prints at a time (datacenter_printf and datacenter_list), so lines from different threads don't get mixed
static pthread_mutex_t print_lock = PTHREAD_MUTEX_INITIALIZER;

void datacenter_printf(const char *format, ...) {
  va_list args;

  pthread_mutex_lock(&print_lock);

  va_start(args, format);
  vprintf(format, args);
  va_end(args);

  //flush right away: the output goes out in the same order the threads printed it
  fflush(stdout);

  pthread_mutex_unlock(&print_lock);
}

void datacenter_init(DataCenter *dc) {
  dc->servers = NULL;
  dc->num_servers = 0;

  dc->num_vm_types = 0;
  pthread_rwlock_init(&dc->vm_types_lock, NULL);

  dc->num_reservations = 0;
  pthread_mutex_init(&dc->reservations_lock, NULL);

  dc->configured = 0;

  dc->local_cores = 0;//local machine cores given to CloudIST (teacher's FAQ, for the cpulimit)

  dc->reaper_started = 0;
  dc->reaper_running = 0;
  dc->live_vms = 0;
  pthread_cond_init(&dc->vms_cond, NULL);

  pthread_mutex_init(&dc->resources_lock, NULL);
  pthread_cond_init(&dc->resources_cond, NULL);
  dc->resources_version = 0;
}

/**
 * Body of the VM reaper thread. Sleeps on vms_cond while there are no VM
 * processes; otherwise blocks in waitpid() and destroys each VM as soon as
 * its process ends. Leaves when the program is ending (reaper_running = 0)
 * and every VM has already been reaped.
 *
 * @param arg Pointer to the Data Center.
 *
 * @return NULL.
 */
static void *reaper_thread(void *arg) {
  DataCenter *dc = arg;

  while (1) {
    //no vm processes: wait here (without spending CPU) until a vm starts or the program ends
    pthread_mutex_lock(&dc->reservations_lock);
    while (dc->live_vms == 0 && dc->reaper_running) {
      pthread_cond_wait(&dc->vms_cond, &dc->reservations_lock);
    }
    //the while only ends with live_vms == 0 if the program is ending -> nothing left to wait for
    int done = (dc->live_vms == 0);
    pthread_mutex_unlock(&dc->reservations_lock);

    if (done) {
      break;
    }

    //blocks until ANY vm process ends (-1 = any child, 0 = block), without holding any lock
    int status;
    pid_t pid = waitpid(-1, &status, 0);

    if (pid > 0) {
      //destroy that vm right away: resources back to its server, delete its folder (and the reservation if it was the last one)
      destroy_vm_by_pid(dc, pid);

      pthread_mutex_lock(&dc->reservations_lock);
      dc->live_vms--;
      pthread_mutex_unlock(&dc->reservations_lock);
    } else if (errno == ECHILD) {
      //no children at all (should not happen while live_vms > 0): reset the counter so we don't spin
      pthread_mutex_lock(&dc->reservations_lock);
      dc->live_vms = 0;
      pthread_mutex_unlock(&dc->reservations_lock);
    }
    //EINTR (interrupted by a signal): just try again
  }

  return NULL;
}

int datacenter_start_reaper(DataCenter *dc) {
  dc->reaper_running = 1;

  if (pthread_create(&dc->reaper_tid, NULL, reaper_thread, dc) != 0) {
    fprintf(stderr, "Failed to create the VM reaper thread.\n");
    dc->reaper_running = 0;
    return 1;
  }

  dc->reaper_started = 1;
  return 0;
}

void datacenter_destroy(DataCenter *dc) {
  //stop the reaper: it still waits for every running vm before leaving, so no vm is left behind
  if (dc->reaper_started) {
    pthread_mutex_lock(&dc->reservations_lock);
    dc->reaper_running = 0;
    pthread_cond_broadcast(&dc->vms_cond);
    pthread_mutex_unlock(&dc->reservations_lock);

    pthread_join(dc->reaper_tid, NULL);
    dc->reaper_started = 0;
  }

  for (size_t i = 0; i < dc->num_servers; i++) {
    for(size_t j = 0; j < dc->servers[i].num_hosted_vms; j++){
      free(dc->servers[i].hosted_vms[j]);
    }
    pthread_mutex_destroy(&dc->servers[i].server_lock);

  }
  free(dc->servers);
  pthread_rwlock_destroy(&dc->vm_types_lock);
  pthread_mutex_destroy(&dc->reservations_lock);
  pthread_cond_destroy(&dc->vms_cond);
  pthread_mutex_destroy(&dc->resources_lock);
  pthread_cond_destroy(&dc->resources_cond);
}

int datacenter_configure(DataCenter *dc, size_t num_servers, Resources *resources){
  if(dc->configured){
    fprintf(stderr, "Data Center is already configured!\n");
    return 1;
  }

  if (num_servers == 0 || resources->ram == 0 || resources->disk == 0 || resources->cpu <= 0.0) {
    fprintf(stderr, "Invalid configuration values.\n");
    return 1;
  }

  dc->servers = (Server*)malloc(num_servers*sizeof(Server));

  if(dc->servers == NULL){
    fprintf(stderr, "Failed to allocate memory for servers.\n");
    return 1;
  }

  for (size_t i = 0; i < num_servers; i++) {
    dc->servers[i].id = i+1;
    dc->servers[i].total = *resources;
    dc->servers[i].available = *resources;
    dc->servers[i].num_hosted_vms = 0;
    pthread_mutex_init(&dc->servers[i].server_lock, NULL);
  }

  dc->num_servers = num_servers;
  dc->configured = 1;

  return 0;
}

int datacenter_define_VM(DataCenter *dc, VMType *type){
  if (!dc->configured) {
    fprintf(stderr, "Data Center needs to be configured!\n");
    return 1;
  }

  //these checks only look at the disk, not at the dc, so they stay outside the lock
  if (!file_exists(type->exec_path)) {
    fprintf(stderr, "Invalid executable path.\n");
    return 1;
  }

  if (!path_exists(type->input_folder)) {
    fprintf(stderr, "Invalid input folder.\n");
    return 1;
  }

  //from here on the types list is read and written: write lock (the D is alone)
  //careful: every return from here on has to unlock first
  pthread_rwlock_wrlock(&dc->vm_types_lock);

  if(VMType_exists(dc, type->id) != NULL){
    fprintf(stderr, "VM type with id %s already exists.\n", type->id);
    pthread_rwlock_unlock(&dc->vm_types_lock);
    return 1;
  }

  if(dc->num_vm_types >= MAX_VM_TYPES){
    fprintf(stderr, "Maximum of VM types reached.\n");
    pthread_rwlock_unlock(&dc->vm_types_lock);
    return 1;
  }

  dc->vm_types[dc->num_vm_types++] = *type;

  pthread_rwlock_unlock(&dc->vm_types_lock);
  return 0;
}

/**
 * Blocks until resources are given back to some server after the moment
 * the caller read resources_version (seen). Reading the version BEFORE
 * checking the servers means a VM that ends in between is never missed.
 *
 * @param dc Pointer to the Data Center.
 * @param seen Value of resources_version read before the last attempt.
 */
static void wait_for_resources(DataCenter *dc, unsigned long seen) {
  pthread_mutex_lock(&dc->resources_lock);
  while (dc->resources_version == seen) {
    pthread_cond_wait(&dc->resources_cond, &dc->resources_lock);
  }
  pthread_mutex_unlock(&dc->resources_lock);
}

int datacenter_reserve(DataCenter *dc, Reservation *reservation) {
  if (!dc->configured) {
    fprintf(stderr, "Data Center needs to be configured!\n");
    return 1;
  }

  //phase 1: can it EVER fit? Read lock of the types (validate looks up the types) and the servers'
  //total resources (they never change, so no server lock is needed). If not, fail right away (no waiting)
  pthread_rwlock_rdlock(&dc->vm_types_lock);
  int impossible = reservation_validate(dc, reservation) != 0;
  pthread_rwlock_unlock(&dc->vm_types_lock);

  if (impossible) {
    return 1;
  }

  //phase 2: take the resources. Lock only this reservation's servers -> reservations on different servers run in parallel.
  //Check and commit inside the same lock, otherwise two reservations could both see "it fits" at the same time.
  //If it doesn't fit right now (other vms are using the resources), wait until some vm gives resources back and try again
  int warned = 0;
  while (1) {
    pthread_mutex_lock(&dc->resources_lock);
    unsigned long seen = dc->resources_version;
    pthread_mutex_unlock(&dc->resources_lock);

    lock_reservation_servers(dc, reservation);

    if (reservation_fits_now(dc, reservation)) {
      int failed = reservation_commit(dc, reservation) != 0;
      unlock_reservation_servers(dc, reservation);
      if (failed) {
        return 1;
      }
      break;
    }

    unlock_reservation_servers(dc, reservation);

    if (!warned) {
      fprintf(stderr, "Not enough free resources for reservation %s right now. Waiting...\n", reservation->id);
      warned = 1;
    }
    wait_for_resources(dc, seen);
  }

  //phase 3: add it to the reservations list (reservations lock)
  pthread_mutex_lock(&dc->reservations_lock);

  int duplicated = reservation_exists(dc, reservation->id);
  int full = dc->num_reservations >= MAX_RESERVATIONS;

  if (!duplicated && !full) {
    dc->reservations[dc->num_reservations] = *reservation;
    dc->num_reservations++;
  }

  pthread_mutex_unlock(&dc->reservations_lock);

  if (duplicated || full) {
    if (duplicated) {
      fprintf(stderr, "Reservation with id %s already exists.\n", reservation->id);
    } else {
      fprintf(stderr, "Max number of reservations reached\n");
    }
    //undo phase 2: give the resources back and delete the vms created (with the servers' locks)
    lock_reservation_servers(dc, reservation);
    reservation_rollback(reservation, 0);
    unlock_reservation_servers(dc, reservation);
    //resources were given back: wake up any R that is waiting for them
    notify_resources_freed(dc);
    return 1;
  }

  return 0;
}

int datacenter_execute(DataCenter *dc, const char *reservation_id) {
  if(!dc->configured){
    fprintf(stderr, "Data Center needs to be configured!\n");
    return 1;
  }

  //reservations lock from the find until the end: res points inside the list, which can move (memmove)
  //if another thread deletes a reservation; and the spawn has to store the pids before anyone tries to
  //destroy these vms (destroy_vm_by_pid uses the same lock)
  pthread_mutex_lock(&dc->reservations_lock);

  Reservation *res = find_pending_reservation(dc, reservation_id);
  if (!res) {
    pthread_mutex_unlock(&dc->reservations_lock);
    return 1;
  }

  if (spawn_all_vms(dc, res) != 0) {
    pthread_mutex_unlock(&dc->reservations_lock);
    return 1;
  }

  res->state = RES_STATE_RUNNING;

  pthread_mutex_unlock(&dc->reservations_lock);
  return 0;
}

int datacenter_list(DataCenter *dc) {
  if (!dc->configured) {
    fprintf(stderr, "Data Center needs to be configured!\n");
    return 1;
  }

  //the L reads everything: lock everything, in the order types -> reservations -> servers (1, 2, 3...)
  pthread_rwlock_rdlock(&dc->vm_types_lock);
  pthread_mutex_lock(&dc->reservations_lock);
  for (size_t i = 0; i < dc->num_servers; i++) {
    pthread_mutex_lock(&dc->servers[i].server_lock);
  }
  //and the print lock, so the whole listing comes out together (no lines from other threads in the middle)
  pthread_mutex_lock(&print_lock);

  printf("=================================== DATA CENTER STATUS ===================================\n");
  printf("\n[ PHYSICAL SERVERS ] (%zu total)\n", dc->num_servers);
  printf("------------------------------------------------------------------------------------------\n");

  for (size_t i = 0; i < dc->num_servers; i++) {
    Server server = dc->servers[i];

    Resources used = server.total;
    resources_sub(&used, server.available);

    printf(" Server #%zu\n", server.id);
    printf("   ├─ Resources Total     : %6.2f vCPUs | %3zu GB RAM | %3zu GB Disk\n",
            server.total.cpu, server.total.ram, server.total.disk);
    printf("   ├─ Resources Used      : %6.2f vCPUs | %3zu GB RAM | %3zu GB Disk\n",
            used.cpu, used.ram, used.disk);
    printf("   ├─ Resources Available : %6.2f vCPUs | %3zu GB RAM | %3zu GB Disk\n",
            server.available.cpu, server.available.ram, server.available.disk);
    printf("   └─ Hosted VMs (%zu):\n", server.num_hosted_vms);

    if (server.num_hosted_vms == 0) {
      printf("        (None)\n");
    } else {
      for (size_t j = 0; j < server.num_hosted_vms; j++) {
        VM *vm = server.hosted_vms[j];
        const char *prefix = (j == server.num_hosted_vms - 1) ? "└──" : "├──";

        printf("        %s VM ID : %-16s | Type: %-10s | State: %-10s",
                prefix,
                vm->id,
                vm->type ? vm->type->id : "Unknown",
                vm_state_to_string(vm->state));

        if (vm->state == VM_STATE_RUNNING) {
          printf(" | PID: %d", (int)vm->pid);
        }
        printf("\n");
      }
    }
    printf("\n");
  }

  printf("[ DEFINED VM TYPES ] (%zu registered)\n", dc->num_vm_types);
  printf("------------------------------------------------------------------------------------------\n");
  if (dc->num_vm_types == 0) {
    printf(" (None)\n\n");
  } else {
      for (size_t i = 0; i < dc->num_vm_types; i++) {
        VMType vt = dc->vm_types[i];
        printf("  * Type ID: %-12s | Requirements: %.2f vCPU, %zu GB RAM, %zu GB Disk\n",
                vt.id, vt.required.cpu, vt.required.ram, vt.required.disk);
        printf("    ├─ Executable : %s\n", vt.exec_path);
        printf("    └─ Input Dir  : %s\n\n", vt.input_folder);
      }
      printf("\n");
  }

  printf("[ RESERVATIONS ] (%zu total)\n", dc->num_reservations);
  printf("------------------------------------------------------------------------------------------\n");
  if (dc->num_reservations == 0) {
    printf(" (None)\n\n");
  } else {
    for (size_t i = 0; i < dc->num_reservations; i++) {
      Reservation res = dc->reservations[i];
      printf("  * Reservation ID: %-12s | State: %-8s | Total VMs: %zu\n",
              res.id, res_state_to_string(res.state), res.num_vms);

      printf("    ├─ Requested Pairs:\n");
      for (size_t k = 0; k < res.items_count; k++) {
        printf("    │    • %-12s : %zu unit(s)\n", res.items[k].vm_type_id, res.items[k].amount);
      }

      printf("    └─ Allocated VMs:\n");
      if (res.num_vms == 0) {
        printf("         (None)\n");
      } else {
          for (size_t k = 0; k < res.num_vms; k++) {
            VM *vm = res.vms[k];
            const char *prefix = (k == res.num_vms - 1) ? "└──" : "├──";
            printf("         %s ID: %-15s | Host Server: #%zu\n",
                    prefix,
                    vm->id,
                    vm->server->id);
          }
      }
      printf("\n");
    }
  }

  printf("================================================================--------------------------\n\n");
  fflush(stdout);

  //unlock in reverse order
  pthread_mutex_unlock(&print_lock);
  for (size_t i = dc->num_servers; i > 0; i--) {
    pthread_mutex_unlock(&dc->servers[i - 1].server_lock);
  }
  pthread_mutex_unlock(&dc->reservations_lock);
  pthread_rwlock_unlock(&dc->vm_types_lock);
  return 0;
}

void datacenter_wait(unsigned int delay_ms){
  struct timespec delay = (struct timespec){delay_ms / 1000, (delay_ms % 1000) * 1000000};
  nanosleep(&delay, NULL);
}
