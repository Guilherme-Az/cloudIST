#define MAX_STRING_SIZE 40
#define MAX_PATH_SIZE 1024

#define MAX_VM_TYPES           128
#define MAX_VM_ID_STRING       MAX_STRING_SIZE + 4 // 4 = "_" + 3 digits
#define MAX_HOSTED_VMS         128

#define MAX_RESERVATIONS       256
#define MAX_RESERVATIONS_ITEMS 32
#define MAX_RESERVATION_VMS    128
// Root folder of every VM folder: CLOUDIST_ROOT/<reservation>/<vm>.
// It has the student number in it: on a shared machine (the cluster) /tmp is
// common to every user, and a /tmp/CloudIST created by another user would give
// "Permission denied" (teacher's FAQ).
#define CLOUDIST_ROOT "/tmp/CloudIST-114255"
