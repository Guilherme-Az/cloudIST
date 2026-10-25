#ifndef DATACENTER__H
#define DATACENTER__H

#include <stdio.h>
#include <sys/types.h>
#include <pthread.h>

#include "resources.h"
#include "constants.h"
#include "filesystem.h"

typedef struct VMType{
  char id[MAX_STRING_SIZE];        // Id of VM type. eg: "V1"
  char input_folder[MAX_PATH_SIZE];  // Folder with input for executable
  char exec_path[MAX_PATH_SIZE];     // Path for executable
  Resources required;
} VMType;

typedef enum {
  VM_STATE_RESERVED,   // placed on a Server, process not started yet (waiting for 'A' command)
  VM_STATE_RUNNING,    // process launched (fork()/exec() called), `pid` is valid
  VM_STATE_TERMINATED  // wait() called, process finished
} VMState;

typedef struct Server Server;

typedef struct VM {
  char id[MAX_VM_ID_STRING]; // VM id. eg: "R1_V1_1"
  VMType *type;   // VM type.
  Server *server; // Server where the VM is placed.
  pid_t pid;      // PID of executable file.
  VMState state;  // State of the VM.
} VM;

typedef struct Server {
	size_t id;		     // Server ID (1, 2, 3, ...)
	Resources total;     // Resources that Server has.
	Resources available; // Resources still available.

	VM *hosted_vms[MAX_HOSTED_VMS];
    size_t num_hosted_vms;

    pthread_mutex_t server_lock; // Protects only this server (available, hosted_vms), so reservations on different servers run in parallel.
} Server;

typedef enum {
    RES_STATE_PENDING,   // Reserved via 'R', waiting for 'A'
    RES_STATE_RUNNING,   // 'A' was called
    RES_STATE_FINISHED   // All VMs in this reservation have terminated
} ReservationState;

typedef struct ReservationPair {
    char vm_type_id[MAX_STRING_SIZE]; // VM type. e.g: "V1"
    size_t amount;                    // Amount of VMs with the given type being reserved.
    size_t server_id;                 // Server where each VM will be running on.

    VMType *vm_type;                   // Used for search optimization.
} ReservationItem;

typedef struct Reservation {
	char id[MAX_STRING_SIZE];                       // id of reservation.

	ReservationItem items[MAX_RESERVATIONS_ITEMS];  // List of reservations items
    size_t items_count;

	VM *vms[MAX_RESERVATION_VMS];                   // List of VMs in this reservation.
	size_t num_vms;                                 // Number of VMs in this reservation.
	ReservationState state;                         // State of the reservation.
} Reservation;

/*
 * Lock order (always the same, so there are no deadlocks):
 *   vm_types_lock -> reservations_lock -> server_lock (increasing server id) -> resources_lock
 */
typedef struct DataCenter {
	Server *servers;			     // List of Servers.
	size_t num_servers;              // Number of Servers.

	VMType vm_types[MAX_VM_TYPES];
    size_t num_vm_types;
    pthread_rwlock_t vm_types_lock; // Protects the VM types list (D writes, R and L only read).

	Reservation reservations[MAX_RESERVATIONS];
    size_t num_reservations;
    pthread_mutex_t reservations_lock; // Protects the reservations list (and each VM's pid/state), live_vms and reaper_running.

	int configured;		      // Wheter the datacenter is configured or not.

	size_t local_cores;       // Cores of the local machine given to CloudIST (for the cpulimit %).

	// VM reaper thread: destroys each VM as soon as its process ends (see datacenter_start_reaper).
	pthread_t reaper_tid;
	int reaper_started;       // 1 after datacenter_start_reaper (so datacenter_destroy knows it has to stop it).
	int reaper_running;       // 0 = the program is ending: the reaper leaves when there are no VMs left.
	size_t live_vms;          // VM processes started and not reaped yet.
	pthread_cond_t vms_cond;  // Wakes the reaper up when a VM is started or when the program is ending.

	// Waiting reservations: an R that doesn't fit right now waits here until some VM gives its resources back.
	pthread_mutex_t resources_lock;
	pthread_cond_t resources_cond;
	unsigned long resources_version; // Incremented every time resources are given back to a server.
} DataCenter;

/**
 * Initializes a Data Center (empty lists and all the locks).
 *
 * @param dc DataCenter being initialized.
 */
void datacenter_init(DataCenter *dc);

/**
 * Destroys a Data Center: stops the VM reaper (if it was started), which
 * first waits for every running VM to finish, and then frees the servers,
 * the remaining VMs and the locks.
 *
 * @param dc DatacCenter being destroyed.
 */
void datacenter_destroy(DataCenter *dc);

/**
 * Configures a given Data Center.
 *
 * @param dc Pointer to Datacenter being configured.
 * @param resources Pointer to resources of every PC on the Datacenter.
 *
 * @return 0 on success.
 * @return 1 on error.
 */
int datacenter_configure(DataCenter *dc, size_t num_pcs, Resources *resources);

/**
 * Starts the VM reaper thread. It blocks in waitpid() and destroys each VM
 * (giving its resources back and deleting its folder) as soon as its process
 * ends. It is not one of the MAX_THREADS worker threads.
 *
 * @param dc Pointer to a configured Data Center.
 *
 * @return 0 on success.
 * @return 1 on error.
 */
int datacenter_start_reaper(DataCenter *dc);

/**
 * Defines a VM for the Data Center
 *
 * @param dc Pointer to a Data Center.
 * @param type Pointer to VM type.
 *
 * @return 0 on success.
 * @return 1 on error.
 */
int datacenter_define_VM(DataCenter *dc, struct VMType *type);

/**
 * Reserves a set of VMs for a given reservation.
 *
 * If the reservation fits in the servers (total resources) but there are not
 * enough free resources right now, it waits until running VMs give resources
 * back and tries again. If it can never fit, it fails right away.
 *
 * @param dc Pointer to a Data Center.
 * @param reservation Pointer to a Reservation.
 *
 * @return 0 on success.
 * @return 1 on error.
 */
int datacenter_reserve(DataCenter *dc, Reservation *reservation);

/**
 * Lists the current state of the Data Center.
 *
 * @param dc Pointer to a Data Center.
 *
 * @return 0 on success.
 * @return 1 on error.
 */
int datacenter_list(DataCenter *dc);

/**
 * Executes a reservation, launching the VMs. It does not wait for them:
 * each VM is destroyed by the reaper thread when its process ends.
 *
 * @param dc Pointer to a Data Center.
 * @param reservation_id ID of the reservation to execute.
 *
 * @return 0 on success.
 * @return 1 on error.
 */
int datacenter_execute(DataCenter *dc, const char *reservation_id);

/**
 * Executes a delay command.
 *
 * @param delay_ms Delay in ms.
 */
void datacenter_wait(unsigned int delay_ms);

/**
 * Thread-safe printf: prints to stdout while holding the print lock and
 * flushes stdout before returning, so lines printed by different threads
 * never get mixed (datacenter_list() holds the same lock while printing).
 *
 * Must not be called by a child process after fork().
 *
 * @param format printf-style format string.
 */
void datacenter_printf(const char *format, ...) __attribute__((format(printf, 1, 2)));

#endif // DATACENTER__H
