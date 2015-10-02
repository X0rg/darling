#ifndef IPC_RIGHT_H
#define IPC_RIGHT_H
#include "mach_includes.h"
#include <linux/list.h>
#include "ipc_types.h"
#include "ipc_port.h"
#include "ipc_space.h"

struct mach_port_right
{
	darling_mach_port_t* port;
	mach_port_right_t type;
	int num_refs;
	
	/* Linked list in ipc_space */
	struct list_head reflist;
};

struct mach_port_right* ipc_right_new(darling_mach_port_t* port, mach_port_right_t type);

/**
 * Changes the reference count of a right.
 * The space in which the right resides must be locked.
 * If right->num_refs is 0 after this call, ipc_space_right_put() must be called.
 */
kern_return_t ipc_right_mod_refs(struct mach_port_right* right, mach_port_right_t type, int refchange);

/**
 * right->port should be locked
 * @param right
 */
void ipc_right_put(struct mach_port_right* right);

/**
 * Deallocates the right, removes the right name and unlocks the port
 * if refcount is zero.
 * Does nothing if refcount > 0.
 * 
 * right->port should be locked. Namespace should be locked.
 * @param right
 * @return true if right was destroyed
 */
bool ipc_right_put_if_noref(struct mach_port_right* right,
		ipc_namespace_t* space, mach_port_name_t name);

/**
 * Converts incoming port disposition to what the recipient will see.
 */
mach_msg_type_name_t ipc_right_copyin_type(mach_msg_type_name_t type);

#define PORT_NULL ((darling_mach_port_t*) 0)
#define PORT_DEAD ((darling_mach_port_t*) ~0)
#define PORT_IS_VALID(port) (port != PORT_NULL && port != PORT_DEAD)

#endif
