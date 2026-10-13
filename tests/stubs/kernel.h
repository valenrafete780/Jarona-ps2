#include "tamtypes.h"
typedef struct { int status; void *func; void *stack; int stack_size; void *gp_reg; int initial_priority; int current_priority; u32 attr; u32 option; u32 wait_type; u32 wait_id; } ee_thread_t;
int CreateThread(ee_thread_t *); int StartThread(int, void *); int GetThreadId(void); int ChangeThreadPriority(int,int);
