#include "datacenter.h"
#include "datacenter_utils.h"

#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

void datacenter_init(DataCenter *dc) {
  dc->servers = NULL;
  dc->num_servers = 0;

  dc->num_vm_types = 0;
  pthread_rwlock_init(&dc->vm_types_lock, NULL);

  dc->num_reservations = 0;
  pthread_mutex_init(&dc->reservations_lock, NULL);

  dc->configured = 0;

  dc->local_cores = 0;//os local cores da maquina local que o stor pediu
}

void datacenter_destroy(DataCenter *dc) {
  for (size_t i = 0; i < dc->num_servers; i++) {
    for(size_t j = 0; j < dc->servers[i].num_hosted_vms; j++){
      free(dc->servers[i].hosted_vms[j]);
    }
    pthread_mutex_destroy(&dc->servers[i].server_lock);

  }
  free(dc->servers);
  pthread_rwlock_destroy(&dc->vm_types_lock);
  pthread_mutex_destroy(&dc->reservations_lock);
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

  //estas verificações só olham para o disco, não para o dc, por isso ficam fora do lock
  if (!file_exists(type->exec_path)) {
    fprintf(stderr, "Invalid executable path.\n");
    return 1;
  }

  if (!path_exists(type->input_folder)) {
    fprintf(stderr, "Invalid input folder.\n");
    return 1;
  }

  //a partir daqui lê-se e escreve-se o array de tipos: lock de escrita (o D fica sozinho)
  //atenção: cada return a partir daqui tem de fazer unlock antes
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

int datacenter_reserve(DataCenter *dc, Reservation *reservation) {
  if (!dc->configured) {
    fprintf(stderr, "Data Center needs to be configured!\n");
    return 1;
  }

  //fase 1+2: ver se cabe e tirar os recursos. Lock de leitura dos tipos (o validate procura os tipos)
  //e lock só dos servidores desta reserva -> reservas em servidores diferentes correm em paralelo.
  //validate e commit ficam dentro do mesmo lock, senão duas reservas viam "cabe" ao mesmo tempo
  pthread_rwlock_rdlock(&dc->vm_types_lock);
  lock_reservation_servers(dc, reservation);

  int failed = reservation_validate(dc, reservation) != 0 || reservation_commit(dc, reservation) != 0;

  unlock_reservation_servers(dc, reservation);
  pthread_rwlock_unlock(&dc->vm_types_lock);

  if (failed) {
    return 1;
  }

  //fase 3: acrescentar ao array das reservas (lock das reservas)
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
    //desfazer a fase 2: devolver os recursos e apagar as vms criadas (com os locks dos servidores)
    lock_reservation_servers(dc, reservation);
    reservation_rollback(reservation, 0);
    unlock_reservation_servers(dc, reservation);
    return 1;
  }

  return 0;
}

int datacenter_execute(DataCenter *dc, const char *reservation_id) {
  if(!dc->configured){
    fprintf(stderr, "Data Center needs to be configured!\n");
    return 1;
  }

  //lock das reservas desde o find até ao fim: o ponteiro res aponta para dentro do array,
  //que pode mudar (memmove) se outra thread apagar uma reserva; e o spawn tem de guardar
  //os pids antes de alguém tentar destruir estas vms (destroy_vm_by_pid usa o mesmo lock)
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

  //o L lê tudo: tranca tudo, pela ordem tipos -> reservas -> servidores (1, 2, 3...)
  pthread_rwlock_rdlock(&dc->vm_types_lock);
  pthread_mutex_lock(&dc->reservations_lock);
  for (size_t i = 0; i < dc->num_servers; i++) {
    pthread_mutex_lock(&dc->servers[i].server_lock);
  }

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

  //destrancar pela ordem inversa
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

void datacenter_collect_finished_vms(DataCenter *dc, int block) {
  int status;
  pid_t pid;
  //está guardado no sistema o estado de todas os processos das children
  //o waitpid faz ir lá buscar=-1 não tem crianças ou deu erro, 0 há children que ainda não acabaram, >0 é o pid da criança que acabou por último
  /*waitpid(WHICH_CHILD, &status, OPTIONS)
  se WHICH_CHILD=-1-> pela ordem que as children acabarem( alternativamente se fosse um pid específico é esse que acabou)
  &status é how it finished
  se OPTIONS= WNOHANG -> o parent não bloqueia- usado para, quando parent ainda está a ler comandos
  alternativamente se  OPTIONS= 0 o parent bloqueia até as crianças acabarem- usado no final da main para esperar que os processos acabem todos*/ 
  while ((pid = waitpid(-1, &status, block ? 0 : WNOHANG)) > 0)
    destroy_vm_by_pid(dc, pid);
}