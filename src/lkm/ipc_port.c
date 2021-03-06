/*
 * Darling Mach Linux Kernel Module
 * Copyright (C) 2015 Lubos Dolezel
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include "ipc_port.h"
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/atomic.h>
#include <linux/sched.h>
#include "debug.h"
#include "darling_task.h"
#include "ipc_right.h"
#include "ipc_space.h"
#include "traps.h"

static atomic_t port_count = ATOMIC_INIT(0);

mach_msg_return_t ipc_port_new(darling_mach_port_t** port_out)
{
	darling_mach_port_t* port;
	
	*port_out = NULL;
	
	port = (darling_mach_port_t*) kmalloc(sizeof(darling_mach_port_t), GFP_KERNEL);
	if (port == NULL)
		return KERN_RESOURCE_SHORTAGE;
	
	mutex_init(&port->mutex);
	port->num_srights = 0;
	port->num_sorights = 0;
	port->is_server_port = false;
	port->is_port_set = false;
	port->queue_size = 0;
	
	INIT_LIST_HEAD(&port->refs);
	INIT_LIST_HEAD(&port->messages);
	hash_init(port->sets);
	init_waitqueue_head(&port->queue_send);
	init_waitqueue_head(&port->queue_recv);
	
	debug_msg("Allocated new port: %p\n", port);
	
	atomic_inc(&port_count);
	*port_out = port;
	return KERN_SUCCESS;
}

mach_msg_return_t ipc_port_set_new(darling_mach_port_t** port_out)
{
	darling_mach_port_t* port;
	
	*port_out = NULL;
	
	port = (darling_mach_port_t*) kmalloc(sizeof(darling_mach_port_t), GFP_KERNEL);
	if (port == NULL)
		return KERN_RESOURCE_SHORTAGE;
	
	mutex_init(&port->mutex);
	port->num_srights = 0;
	port->num_sorights = 0;
	port->is_server_port = false;
	port->is_port_set = true;

	INIT_LIST_HEAD(&port->refs);
	hash_init(port->members);

	atomic_inc(&port_count);
	*port_out = port;
	return KERN_SUCCESS;
}

static void ipc_portset_delete_member(darling_mach_port_t* pset, darling_mach_port_t* member)
{
	struct hlist_node *tmp2;
	struct darling_mach_port_le* ref2;

	hash_for_each_possible_safe(pset->members, ref2, tmp2, node, (long)member)
	{
		if (ref2->port != member)
			continue;
		hash_del(&ref2->node);
		kfree(ref2);
	}
}

static void ipc_port_delete_set(darling_mach_port_t* pset, darling_mach_port_t* member)
{
	struct hlist_node *tmp2;
	struct darling_mach_port_le* ref2;

	hash_for_each_possible_safe(member->sets, ref2, tmp2, node, (long)pset)
	{
		if (ref2->port != pset)
			continue;
		hash_del(&ref2->node);
		kfree(ref2);
	}
}

mach_msg_return_t ipc_port_put(darling_mach_port_t* port)
{
	struct list_head* p;
	debug_msg("Deallocating port: %p\n", port);
	
	if (port->is_server_port)
	{
		if (port->server_port.cb_free != NULL)
			port->server_port.cb_free(&port->server_port);
	}
	if (port->is_port_set)
	{
		int bkt;
		struct hlist_node *tmp;
		struct darling_mach_port_le* ref;
		
		// Iterate referenced ports
		hash_for_each_safe(port->members, bkt, tmp, ref, node)
		{
			ipc_port_lock(ref->port);

			// Find ref to self in port's list of sets
			ipc_port_delete_set(port, ref->port);
			
			ipc_port_unlock(ref->port);
			
			kfree(ref);
		}
	}
	else // ordinary port
	{
		int bkt;
		struct hlist_node *tmp;
		struct darling_mach_port_le* ref;
		struct ipc_delivered_msg* msg;
		
		// Remove itself from all port sets we're in
		hash_for_each_safe(port->sets, bkt, tmp, ref, node)
		{
			ipc_port_lock(ref->port);
			
			// Find ref to self in port sets's list of ports
			ipc_portset_delete_member(ref->port, port);
			
			ipc_port_unlock(ref->port);
			
			kfree(ref);
		}
		
		// set recipient_died to true for all pending msgs
		list_for_each_entry(msg, &port->messages, list)
		{
			msg->recipient_died = true;
		}
		wake_up_all(&port->queue_send);
		wake_up_all(&port->queue_recv);
	}
	
	// mark all references as dead ports
	list_for_each(p, &port->refs)
	{
		struct mach_port_right* ref;

		ref = list_entry(p, struct mach_port_right, reflist);
		
		debug_msg("Marking right %p as PORT_DEAD\n", ref);

		spin_lock(&ref->port_lock);
		ref->port = PORT_DEAD;
		ref->type = MACH_PORT_RIGHT_DEAD_NAME;
		spin_unlock(&ref->port_lock);
	}
	debug_msg("All refs to port %p are now dead\n", port);
	
	atomic_dec(&port_count);
	kfree(port);
	return KERN_SUCCESS;
}

kern_return_t ipc_portset_insert(darling_mach_port_t* pset, darling_mach_port_t* port)
{
	struct darling_mach_port_le *pset_member, *port_member;
	
	if (!pset->is_port_set || port->is_port_set)
		return KERN_INVALID_RIGHT;
	
	pset_member = (struct darling_mach_port_le*) kmalloc(sizeof(*pset_member), GFP_KERNEL);
	if (pset_member == NULL)
		return KERN_RESOURCE_SHORTAGE;
	
	port_member = (struct darling_mach_port_le*) kmalloc(sizeof(*pset_member), GFP_KERNEL);
	if (port_member == NULL)
	{
		kfree(pset_member);
		return KERN_RESOURCE_SHORTAGE;
	}
	
	pset_member->port = port;
	port_member->port = pset;
	
	hash_add(pset->members, &pset_member->node, (long)port);
	hash_add(port->sets, &port_member->node, (long)pset);
	
	return KERN_SUCCESS;
}

kern_return_t ipc_portset_move(darling_mach_port_t* pset, darling_mach_port_t* port)
{
	int bkt;
	struct hlist_node *tmp;
	struct darling_mach_port_le* ref;
	
	if (port->is_port_set)
		return KERN_INVALID_RIGHT;
	if (pset != NULL && !pset->is_port_set)
		return KERN_INVALID_RIGHT;

	// Remove port from all port sets it is in
	hash_for_each_safe(port->sets, bkt, tmp, ref, node)
	{
		// Delete ref on the pset side
		ipc_portset_delete_member(pset, port);
		
		kfree(ref);
	}
	
	// Kill all entries
	hash_init(port->sets);
	
	if (pset != NULL)
		return ipc_portset_insert(pset, port);
	
	return KERN_SUCCESS;
}

kern_return_t ipc_portset_extract(darling_mach_port_t* pset, darling_mach_port_t* port)
{
	if (!pset->is_port_set || port->is_port_set)
		return KERN_INVALID_RIGHT;
	
	ipc_port_delete_set(pset, port);
	ipc_portset_delete_member(pset, port);
	
	return KERN_SUCCESS;
}

void ipc_port_lock(darling_mach_port_t* port)
{
	if (PORT_IS_VALID(port))
		mutex_lock(&port->mutex);
}

void ipc_port_unlock(darling_mach_port_t* port)
{
	if (PORT_IS_VALID(port))
		mutex_unlock(&port->mutex);
}

int ipc_port_count(void)
{
	return atomic_read(&port_count);
}
