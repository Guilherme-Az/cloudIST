#ifndef DATACENTER_UTILS__H
#define DATACENTER_UTILS__H

#include "datacenter.h"

/**
 * Checks if a VMType with the given id exists.
 * Caller must hold dc->vm_types_lock (read or write).
 *
 * @param dc Pointer to Data Center.
 * @param type_id Id of VM type being checked.
 *
 * @return Pointer to VMType if it exists.
 * @return NULL if VMType does not exist.
 */
VMType *VMType_exists(DataCenter *dc, const char* type_id);

/**
 * Checks if a reservation with the given id already exists in the Data Center.
 * Caller must hold dc->reservations_lock.
 *
 * @param dc Pointer to Data Center.
 * @param reservation_id Id of the reservation being checked.
 *
 * @return 1 if the reservation exists.
 * @return 0 if the reservation does not exist.
 */
int reservation_exists(DataCenter *dc, const char* reservation_id);

/**
 * Checks whether the reservation could EVER be allocated: every VM type
 * exists, every server id is valid, the amounts are within the limits, and
 * all the requested VMs fit in each server's TOTAL resources (as if the
 * servers were empty). Attaches each item's VMType to item->vm_type.
 * The Data Center state is left unchanged regardless of the outcome.
 *
 * Caller must hold dc->vm_types_lock (read). The servers' total resources
 * never change, so no server lock is needed.
 *
 * @param dc Data Center where the reservation will be validated.
 * @param reservation Reservation to validate.
 *
 * @return 0 if the reservation can be allocated (now or later).
 * @return 1 if it can never be allocated (the error is printed).
 */
int reservation_validate(DataCenter *dc, Reservation *reservation);

/**
 * Simulates the placement of every VM requested in the reservation using a
 * temporary copy of each server's AVAILABLE resources, to know whether it
 * fits right now. Nothing is printed and the Data Center is not changed.
 *
 * The reservation must have passed reservation_validate (item->vm_type set)
 * and the caller must hold the reservation's server locks
 * (lock_reservation_servers).
 *
 * @param dc Pointer to the Data Center.
 * @param reservation Reservation to check.
 *
 * @return 1 if it fits right now.
 * @return 0 if the servers don't have enough free resources at the moment.
 */
int reservation_fits_now(DataCenter *dc, Reservation *reservation);

/**
 * Creates all VMs requested by the reservation, assigns them to their
 * designated servers, updates the server resource availability, and stores
 * references to the created VMs in both the reservation and the corresponding
 * server hosted VM list.
 *
 * The reservation is expected to have already passed validation before this
 * function is called. If an error occurs during the commit process, the
 * function performs a rollback of the changes made by this operation.
 * Caller must hold the reservation's server locks.
 *
 * @param dc Data Center where the reservation will be committed.
 * @param reservation Reservation to allocate.
 *
 * @return 0 if the reservation was successfully committed
 * @return 1 if an error occurred and the changes were rolled back.
 */
int reservation_commit(DataCenter *dc, Reservation *reservation);

/**
 * Rolls back the changes made by reservation_commit: frees the VMs created
 * after the first initial_vms and gives their resources back to the servers.
 * Caller must hold the locks of the reservation's servers.
 *
 * @param reservation Pointer to the reservation being rolled back.
 * @param initial_vms Number of VMs that existed before the operation.
 */
void reservation_rollback(Reservation *reservation, size_t initial_vms);

/**
 * Tells the reservations waiting for free resources (datacenter_reserve)
 * that some resources were given back, so they try again.
 *
 * @param dc Pointer to the Data Center.
 */
void notify_resources_freed(DataCenter *dc);

/**
 * Looks up a reservation by id and checks that it is pending.
 * Caller must hold dc->reservations_lock (the returned pointer points inside
 * the reservations list, which moves when a reservation is deleted).
 *
 * @param dc Pointer to a Data Center.
 * @param reservation_id ID of the reservation to look up.
 *
 * @return Pointer to the Reservation if found and RES_STATE_PENDING.
 * @return NULL if not found or not in RES_STATE_PENDING (error is printed).
 */
Reservation *find_pending_reservation(DataCenter *dc, const char *reservation_id);

/**
 * Frees every VM allocated to the reservation, returns their resources to the
 * corresponding hosting servers, removes them from each server's hosted VM
 * list, and finally removes the reservation from the Data Center reservation
 * list.
 *
 * @param dc Pointer to the Data Center containing the reservation.
 * @param reservation Pointer to the reservation to destroy.
 */
void reservation_destroy(DataCenter *dc, Reservation *reservation);

/**
 * Locks the servers used by the reservation's items, always in increasing
 * server id order (so two reservations can never deadlock).
 *
 * @param dc Pointer to the Data Center.
 * @param res Reservation whose servers are locked.
 */
void lock_reservation_servers(DataCenter *dc, Reservation *res);

/**
 * Unlocks the servers locked by lock_reservation_servers.
 *
 * @param dc Pointer to the Data Center.
 * @param res Reservation whose servers are unlocked.
 */
void unlock_reservation_servers(DataCenter *dc, Reservation *res);

/**
 * Destroys the VM whose process has the given pid (called by the reaper
 * thread after waitpid() reports that it finished): returns its resources to
 * the hosting server, wakes up the reservations waiting for resources,
 * deletes the VM folder, removes it from the server and from its reservation,
 * and frees it. If it was the last VM of the reservation, the reservation
 * (and its folder) is removed too. Does nothing if no VM has that pid.
 *
 * @param dc Pointer to the Data Center.
 * @param pid Pid of the finished VM process.
 */
void destroy_vm_by_pid(DataCenter *dc, pid_t pid);

/**
 * Runs in the child process after fork(): prepares the VM folder
 * (/tmp/CloudIST-114255/<reservation>/<vm>) with a copy of the inputs, sets
 * resource limits (RAM, disk), moves into the VM folder (chdir), sends the
 * application's stdout to /dev/null and execs the VM's workload under
 * cpulimit. Never returns (either execs successfully or calls _exit(1) on
 * failure).
 *
 * @param vm Pointer to the VM being spawned.
 * @param reservation_id Id of the VM's reservation (for the folder path).
 * @param cpu_limit CPU limit for cpulimit -l (may be above 100).
 */
void spawn_vm_child(VM *vm, const char *reservation_id, long cpu_limit);

/**
 * Forks one child per VM in the reservation, calling spawn_vm_child()
 * in each child and recording the pid/state in the parent (and waking up
 * the reaper thread). Caller must hold dc->reservations_lock.
 *
 * @param dc Pointer to the Data Center (number of servers and local cores,
 *           for the cpulimit percentage).
 * @param res Pointer to the Reservation whose VMs should be spawned.
 *
 * @return 0 on success.
 * @return 1 if any fork() call fails.
 */
int spawn_all_vms(DataCenter *dc, Reservation *res);

/**
 * Base-code stub (not used anymore: the reaper thread waits for the VMs).
 * Marks every VM of the reservation as VM_STATE_TERMINATED.
 *
 * @param res Pointer to the Reservation whose VMs should be waited on.
 */
void wait_for_all_vms(Reservation *res);

/**
 * Converts a VM state to a human-readable string. Used by datacenter_list().
 *
 * @param state The VMState to convert.
 *
 * @return Static, human-readable string describing the state.
 */
const char *vm_state_to_string(VMState state);

/**
 * Converts a reservation state to a human-readable string. Used by
 * datacenter_list().
 *
 * @param state The ReservationState to convert.
 *
 * @return Static, human-readable string describing the state.
 */
const char *res_state_to_string(ReservationState state);

#endif // DATACENTER_UTILS__H
