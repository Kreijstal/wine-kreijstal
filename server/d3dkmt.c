/*
 * Server-side D3DKMT resource management
 *
 * Copyright 2025 Rémi Bernon for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "config.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

#include "ntstatus.h"
#include "windef.h"
#include "winternl.h"
#include "ddk/wdm.h"

#include "file.h"
#include "handle.h"
#include "request.h"
#include "security.h"

struct d3dkmt_object
{
    struct object       obj;            /* object header */
    enum d3dkmt_type    type;           /* object type */
    d3dkmt_handle_t     global;         /* object global handle */
    void               *runtime;        /* client runtime data */
    data_size_t         runtime_size;   /* size of client runtime data */
    struct fd          *fd;             /* fd object for unix fds */
};

static void d3dkmt_object_dump( struct object *obj, int verbose );
static struct fd *d3dkmt_object_get_fd( struct object *obj );
static void d3dkmt_object_destroy( struct object *obj );

static const struct object_ops d3dkmt_object_ops =
{
    .size    = sizeof(struct d3dkmt_object),
    .type    = &no_type,
    .dump    = d3dkmt_object_dump,
    .get_fd  = d3dkmt_object_get_fd,
    .destroy = d3dkmt_object_destroy,
};

static enum server_fd_type d3dkmt_get_fd_type( struct fd *fd )
{
    return FD_TYPE_INVALID;
}

static const struct fd_ops d3dkmt_fd_ops =
{
    .get_fd_type = d3dkmt_get_fd_type,
};

struct keyed_wait
{
    struct list     entry;
    int             key;
    int             waiters;
    struct object  *sync;
};

struct d3dkmt_mutex
{
    struct d3dkmt_object base;
    unsigned int         key_value;      /* last released key value */
    unsigned __int64     fence_value;    /* last released fence value */
    bool                 abandoned;      /* mutex has been abandonned */
    struct thread       *owner;          /* current owner thread */
    struct list          waits;          /* list of pending keyed_waits */
    struct list          entry;          /* entry in owner d3dkmt_mutexes */
};

static void d3dkmt_mutex_dump( struct object *obj, int verbose );
static void d3dkmt_mutex_destroy( struct object *obj );

static const struct object_ops d3dkmt_mutex_ops =
{
    .size    = sizeof(struct d3dkmt_mutex),
    .type    = &no_type,
    .dump    = d3dkmt_mutex_dump,
    .destroy = d3dkmt_mutex_destroy,
};

#define DXGK_SHARED_SYNC_QUERY_STATE  0x0001
#define DXGK_SHARED_SYNC_MODIFY_STATE 0x0002
#define DXGK_SHARED_SYNC_ALL_ACCESS   (STANDARD_RIGHTS_REQUIRED|SYNCHRONIZE|0x3)

static const WCHAR dxgk_shared_sync_name[] = {'D','x','g','k','S','h','a','r','e','d','S','y','n','c','O','b','j','e','c','t'};

struct type_descr dxgk_shared_sync_type =
{
    { dxgk_shared_sync_name, sizeof(dxgk_shared_sync_name) },           /* name */
    DXGK_SHARED_SYNC_ALL_ACCESS,                                        /* valid_access */
    {                                                                   /* mapping */
        STANDARD_RIGHTS_READ | DXGK_SHARED_SYNC_QUERY_STATE,
        STANDARD_RIGHTS_WRITE | DXGK_SHARED_SYNC_MODIFY_STATE,
        STANDARD_RIGHTS_EXECUTE | SYNCHRONIZE,
        DXGK_SHARED_SYNC_ALL_ACCESS,
    },
};

struct dxgk_shared_sync
{
    struct object   obj;    /* object header */
    struct object  *sync;   /* shared sync object */
};

static void dxgk_shared_sync_dump( struct object *obj, int verbose );
static void dxgk_shared_sync_destroy( struct object *obj );

static const struct object_ops dxgk_shared_sync_ops =
{
    .size    = sizeof(struct dxgk_shared_sync),
    .type    = &dxgk_shared_sync_type,
    .dump    = dxgk_shared_sync_dump,
    .destroy = dxgk_shared_sync_destroy,
};

static void dxgk_shared_sync_dump( struct object *obj, int verbose )
{
    struct dxgk_shared_sync *shared = (struct dxgk_shared_sync *)obj;
    assert( obj->ops == &dxgk_shared_sync_ops );
    fprintf( stderr, "DxgkSync sync=%p\n", shared->sync );
}

static void dxgk_shared_sync_destroy( struct object *obj )
{
    struct dxgk_shared_sync *shared = (struct dxgk_shared_sync *)obj;
    assert( obj->ops == &dxgk_shared_sync_ops );
    release_object( shared->sync );
}

#define DXGK_SHARED_RESOURCE_MODIFY_STATE 0x0001
#define DXGK_SHARED_RESOURCE_ALL_ACCESS   (STANDARD_RIGHTS_REQUIRED|SYNCHRONIZE|0x1)

static const WCHAR dxgk_shared_resource_name[] = {'D','x','g','k','S','h','a','r','e','d','R','e','s','o','u','r','c','e'};

struct type_descr dxgk_shared_resource_type =
{
    { dxgk_shared_resource_name, sizeof(dxgk_shared_resource_name) },   /* name */
    DXGK_SHARED_RESOURCE_ALL_ACCESS,                                    /* valid_access */
    {                                                                   /* mapping */
        STANDARD_RIGHTS_READ,
        STANDARD_RIGHTS_WRITE | DXGK_SHARED_RESOURCE_MODIFY_STATE,
        STANDARD_RIGHTS_EXECUTE,
        STANDARD_RIGHTS_REQUIRED | DXGK_SHARED_RESOURCE_MODIFY_STATE,
    },
};

struct dxgk_shared_resource
{
    struct object   obj;        /* object header */
    struct object  *resource;   /* shared resource object */
    struct object  *mutex;      /* shared keyed mutex object */
    struct object  *sync;       /* shared sync object */
};

static void dxgk_shared_resource_dump( struct object *obj, int verbose );
static void dxgk_shared_resource_destroy( struct object *obj );

static const struct object_ops dxgk_shared_resource_ops =
{
    .size    = sizeof(struct dxgk_shared_resource),
    .type    = &dxgk_shared_resource_type,
    .dump    = dxgk_shared_resource_dump,
    .destroy = dxgk_shared_resource_destroy,
};

static void dxgk_shared_resource_dump( struct object *obj, int verbose )
{
    struct dxgk_shared_resource *shared = (struct dxgk_shared_resource *)obj;
    assert( obj->ops == &dxgk_shared_resource_ops );
    fprintf( stderr, "DxgkResource resource=%p mutex=%p sync=%p\n", shared->resource,
             shared->mutex, shared->sync );
}

static void dxgk_shared_resource_destroy( struct object *obj )
{
    struct dxgk_shared_resource *shared = (struct dxgk_shared_resource *)obj;
    assert( obj->ops == &dxgk_shared_resource_ops );
    release_object( shared->resource );
    if (shared->mutex) release_object( shared->mutex );
    if (shared->sync) release_object( shared->sync );
}

static struct d3dkmt_object **objects, **objects_end, **objects_next;

#define D3DKMT_HANDLE_BIT  0x40000000

static d3dkmt_handle_t index_to_handle( int index )
{
    return (index << 6) | D3DKMT_HANDLE_BIT | 2;
}

static int handle_to_index( d3dkmt_handle_t handle )
{
    return (handle & ~0xc000003f) >> 6;
}

static bool init_handle_table(void)
{
    static const size_t initial_capacity = 1024;

    if (!(objects = mem_alloc( initial_capacity * sizeof(*objects) ))) return false;
    memset( objects, 0, initial_capacity * sizeof(*objects) );
    objects_end = objects + initial_capacity;
    objects_next = objects;

    return true;
}

static struct d3dkmt_object **grow_handle_table(void)
{
    size_t old_capacity = objects_end - objects, max_capacity = handle_to_index( D3DKMT_HANDLE_BIT - 1 );
    unsigned int new_capacity = old_capacity * 3 / 2;
    struct d3dkmt_object **tmp;

    if (new_capacity > max_capacity) new_capacity = max_capacity;
    if (new_capacity <= old_capacity) return NULL; /* exhausted handle capacity */

    if (!(tmp = realloc( objects, new_capacity * sizeof(*objects) ))) return NULL;
    memset( tmp + old_capacity, 0, (new_capacity - old_capacity) * sizeof(*tmp) );

    objects = tmp;
    objects_end = tmp + new_capacity;
    objects_next = tmp + old_capacity;

    return objects_next;
}

/* allocate a d3dkmt object with a global handle */
static d3dkmt_handle_t alloc_object_handle( struct d3dkmt_object *object )
{
    struct d3dkmt_object **entry;
    d3dkmt_handle_t handle = 0;

    if (!objects && !init_handle_table()) goto done;

    for (entry = objects_next; entry < objects_end; entry++) if (!*entry) break;
    if (entry == objects_end)
    {
        for (entry = objects; entry < objects_next; entry++) if (!*entry) break;
        if (entry == objects_next && !(entry = grow_handle_table())) goto done;
    }

    handle = index_to_handle( entry - objects );
    objects_next = entry + 1;
    *entry = object;

done:
    if (!handle) set_error( STATUS_NO_MEMORY );
    return handle;
}

/* free a d3dkmt global object handle */
static void free_object_handle( d3dkmt_handle_t global )
{
    unsigned int index = handle_to_index( global );
    assert( objects + index < objects_end );
    objects[index] = NULL;
}

/* return a pointer to a d3dkmt object from its global handle */
static void *get_d3dkmt_object( d3dkmt_handle_t global, enum d3dkmt_type type )
{
    unsigned int index = handle_to_index( global );
    struct d3dkmt_object *object;

    if (objects + index >= objects_end) object = NULL;
    else object = objects[index];

    if (!object || object->global != global || object->type != type) return NULL;
    return object;
}

static void d3dkmt_object_dump( struct object *obj, int verbose )
{
    struct d3dkmt_object *object = (struct d3dkmt_object *)obj;
    assert( obj->ops == &d3dkmt_object_ops );

    fprintf( stderr, "type=%#x global=%#x\n", object->type, object->global );
}

static struct fd *d3dkmt_object_get_fd( struct object *obj )
{
    struct d3dkmt_object *object = (struct d3dkmt_object *)obj;
    assert( obj->ops == &d3dkmt_object_ops );

    if (object->fd) return (struct fd *)grab_object( object->fd );

    set_error( STATUS_NO_SUCH_FILE );
    return NULL;
}

static void d3dkmt_object_destroy( struct object *obj )
{
    struct d3dkmt_object *object = (struct d3dkmt_object *)obj;
    assert( obj->ops == &d3dkmt_object_ops );

    if (object->global) free_object_handle( object->global );
    if (object->fd) release_object( object->fd );
    free( object->runtime );
}

static struct d3dkmt_object *d3dkmt_object_create( enum d3dkmt_type type, data_size_t runtime_size, const void *runtime )
{
    struct d3dkmt_object *object;

    if (!(object = alloc_object( &d3dkmt_object_ops ))) return NULL;
    object->type            = type;
    object->global          = 0;
    object->runtime_size    = runtime_size;
    object->fd              = NULL;

    if (!(object->runtime = memdup( runtime, runtime_size )) ||
        !(object->global = alloc_object_handle( object )))
    {
        release_object( object );
        return NULL;
    }

    return object;
}

static void d3dkmt_mutex_dump( struct object *obj, int verbose )
{
    struct d3dkmt_mutex *mutex = (struct d3dkmt_mutex *)obj;
    assert( obj->ops == &d3dkmt_mutex_ops );

    fprintf( stderr, "d3dkmt mutex global=%#x\n", mutex->base.global );
}

static void d3dkmt_mutex_destroy( struct object *obj )
{
    struct d3dkmt_mutex *mutex = (struct d3dkmt_mutex *)obj;
    struct keyed_wait *wait, *next;

    assert( obj->ops == &d3dkmt_mutex_ops );

    LIST_FOR_EACH_ENTRY_SAFE( wait, next, &mutex->waits, struct keyed_wait, entry )
    {
        release_object( wait->sync );
        list_remove( &wait->entry );
        free( wait );
    }

    if (mutex->base.global) free_object_handle( mutex->base.global );
    free( mutex->base.runtime );
}

static struct d3dkmt_object *d3dkmt_mutex_create( unsigned int key_value, data_size_t runtime_size, const void *runtime )
{
    struct d3dkmt_mutex *object;

    if (!(object = alloc_object( &d3dkmt_mutex_ops ))) return NULL;
    object->base.type            = D3DKMT_MUTEX;
    object->base.global          = 0;
    object->base.runtime_size    = runtime_size;
    object->base.fd              = NULL;
    object->key_value            = key_value;
    object->fence_value          = 0;
    object->abandoned            = false;
    object->owner                = NULL;
    list_init( &object->waits );

    if (!(object->base.runtime = memdup( runtime, runtime_size )) ||
        !(object->base.global = alloc_object_handle( &object->base )))
    {
        release_object( object );
        return NULL;
    }

    return &object->base;
}

static struct object *keyed_wait_grab( struct d3dkmt_mutex *mutex, int key )
{
    struct keyed_wait *wait;

    LIST_FOR_EACH_ENTRY( wait, &mutex->waits, struct keyed_wait, entry )
    {
        if (wait->key != key) continue;
        wait->waiters++;
        return grab_object( wait->sync );
    }

    if (!(wait = mem_alloc( sizeof(*wait) ))) return NULL;
    wait->key       = key;
    wait->waiters   = 1;
    if (!(wait->sync = create_internal_sync( 0, 0 )))
    {
        free( wait );
        return NULL;
    }

    list_add_tail( &mutex->waits, &wait->entry );
    return grab_object( wait->sync );
}

static void keyed_wait_release( struct d3dkmt_mutex *mutex, int key )
{
    struct keyed_wait *wait;

    LIST_FOR_EACH_ENTRY( wait, &mutex->waits, struct keyed_wait, entry )
    {
        if (wait->key == key && !--wait->waiters)
        {
            release_object( wait->sync );
            list_remove( &wait->entry );
            free( wait );
            break;
        }
    }
}

static void mutex_grab( struct d3dkmt_mutex *mutex )
{
    grab_object( mutex );
    list_add_tail( &current->d3dkmt_mutexes, &mutex->entry );
    mutex->owner = current;
}

static void mutex_release( struct d3dkmt_mutex *mutex, bool abandon )
{
    struct keyed_wait *wait;

    LIST_FOR_EACH_ENTRY( wait, &mutex->waits, struct keyed_wait, entry )
    {
        if (abandon || wait->key == mutex->key_value)
        {
            signal_sync( wait->sync );
            if (!abandon) break;
        }
    }
    if (abandon) mutex->abandoned = true;

    mutex->owner = NULL;
    list_remove( &mutex->entry );
    release_object( mutex );
}

void abandon_d3dkmt_mutexes( struct thread *thread )
{
    struct d3dkmt_mutex *mutex, *next;

    LIST_FOR_EACH_ENTRY_SAFE( mutex, next, &thread->d3dkmt_mutexes, struct d3dkmt_mutex, entry )
        mutex_release( mutex, true );
}

/* return a pointer to a d3dkmt object from its global handle */
static void *d3dkmt_object_open( d3dkmt_handle_t global, enum d3dkmt_type type )
{
    struct d3dkmt_object *object;

    if (!(object = get_d3dkmt_object( global, type )))
    {
        set_error( STATUS_INVALID_PARAMETER );
        return NULL;
    }
    return grab_object( object );
}

static struct d3dkmt_object *d3dkmt_object_open_shared( obj_handle_t handle, enum d3dkmt_type type )
{
    struct object *obj, *ret = NULL;

    if ((obj = get_handle_obj( current->process, handle, 0, &dxgk_shared_resource_ops )))
    {
        struct dxgk_shared_resource *shared = (struct dxgk_shared_resource *)obj;
        if (type == D3DKMT_RESOURCE) ret = grab_object( shared->resource );
        else if (type == D3DKMT_MUTEX && shared->mutex) ret = grab_object( shared->mutex );
        else if (type == D3DKMT_SYNC && shared->sync) ret = grab_object( shared->sync );
        release_object( obj );
        if (!ret) set_error( STATUS_INVALID_PARAMETER );
        return (struct d3dkmt_object *)ret;
    }

    if (type != D3DKMT_SYNC) return NULL;

    /* try again looking for a shared sync if client asked for a sync object */
    set_error( STATUS_SUCCESS );

    if ((obj = get_handle_obj( current->process, handle, 0, &dxgk_shared_sync_ops )))
    {
        struct dxgk_shared_sync *shared = (struct dxgk_shared_sync *)obj;
        ret = grab_object( shared->sync );
        release_object( obj );
    }

    return (struct d3dkmt_object *)ret;
}

/* create a global d3dkmt object */
DECL_HANDLER(d3dkmt_object_create)
{
    struct d3dkmt_object *object;
    struct fd *fd = NULL;

    if (req->fd >= 0)
    {
        int unix_fd;
        if ((unix_fd = thread_get_inflight_fd( current, req->fd )) < 0) return;
        if (!(fd = create_anonymous_fd( NULL, unix_fd, NULL, 0 ))) return;
    }

    switch (req->type)
    {
    case D3DKMT_MUTEX:
        if (!(object = d3dkmt_mutex_create( req->value, get_req_data_size(), get_req_data() ))) goto done;
        break;
    default:
        if (!(object = d3dkmt_object_create( req->type, get_req_data_size(), get_req_data() ))) goto done;
        break;
    }

    if (fd)
    {
        set_fd_user( fd, &d3dkmt_fd_ops, &object->obj );
        object->fd = (struct fd *)grab_object( fd );
    }

    reply->handle = alloc_handle( current->process, object, STANDARD_RIGHTS_ALL, OBJ_INHERIT );
    reply->global = object->global;
    release_object( object );

done:
    if (fd) release_object( fd );
}

/* update a global d3dkmt object */
DECL_HANDLER(d3dkmt_object_update)
{
    struct d3dkmt_object *object;
    void *tmp, *runtime;
    data_size_t size;

    if (!(size = get_req_data_size())) runtime = NULL;
    else if (!(runtime = memdup( get_req_data(), size ))) return;

    if (!(object = d3dkmt_object_open( req->global, req->type ))) goto done;
    tmp = object->runtime;
    object->runtime = runtime;
    object->runtime_size = size;
    runtime = tmp;
    release_object( object );

done:
    free( runtime );
}

/* query a global d3dkmt object */
DECL_HANDLER(d3dkmt_object_query)
{
    struct d3dkmt_object *object;

    if (req->global) object = d3dkmt_object_open( req->global, req->type );
    else object = d3dkmt_object_open_shared( req->handle, req->type );
    if (!object) return;

    reply->runtime_size = object->runtime_size;
    release_object( object );
}

/* open a global d3dkmt object */
DECL_HANDLER(d3dkmt_object_open)
{
    data_size_t runtime_size = get_reply_max_size();
    struct d3dkmt_object *object;
    obj_handle_t handle;

    if (req->global) object = d3dkmt_object_open( req->global, req->type );
    else object = d3dkmt_object_open_shared( req->handle, req->type );
    if (!object) return;

    /* only resource objects require exact runtime buffer size match */
    if (object->type != D3DKMT_RESOURCE && runtime_size > object->runtime_size) runtime_size = object->runtime_size;

    if (runtime_size && object->runtime_size != runtime_size) set_error( STATUS_INVALID_PARAMETER );
    else if ((handle = alloc_handle( current->process, object, STANDARD_RIGHTS_ALL, OBJ_INHERIT )))
    {
        reply->handle = handle;
        reply->global = object->global;
        reply->runtime_size = object->runtime_size;
        if (runtime_size) set_reply_data( object->runtime, object->runtime_size );
    }

    release_object( object );
}

/* share global d3dkmt objects together */
DECL_HANDLER(d3dkmt_share_objects)
{
    struct object *resource = NULL, *mutex = NULL, *sync = NULL;
    struct object_params params;

    if (!get_req_object_attributes( &params )) return;
    params.attr |= OBJ_CASE_INSENSITIVE;

    if (req->resource)
    {
        struct dxgk_shared_resource *shared;

        if (!(resource = d3dkmt_object_open( req->resource, D3DKMT_RESOURCE ))) goto done;
        if (req->mutex && !(mutex = d3dkmt_object_open( req->mutex, D3DKMT_MUTEX ))) goto done;
        if (req->sync && !(sync = d3dkmt_object_open( req->sync, D3DKMT_SYNC ))) goto done;

        params.ops = &dxgk_shared_resource_ops;
        if (!(shared = create_named_object( &params ))) goto done;
        shared->resource = grab_object( resource );
        if ((shared->mutex = mutex)) grab_object( mutex );
        if ((shared->sync = sync)) grab_object( sync );
        reply->handle = alloc_handle( current->process, shared, req->access, OBJ_INHERIT );
        release_object( shared );
    }
    else
    {
        struct dxgk_shared_sync *shared;

        if (!(sync = d3dkmt_object_open( req->sync, D3DKMT_SYNC ))) goto done;

        params.ops = &dxgk_shared_sync_ops;
        if (!(shared = create_named_object( &params ))) goto done;
        shared->sync = grab_object( sync );
        reply->handle = alloc_handle( current->process, shared, req->access, OBJ_INHERIT );
        release_object( shared );
    }

done:
    if (params.root) release_object( params.root );
    if (resource) release_object( resource );
    if (mutex) release_object( mutex );
    if (sync) release_object( sync );
}

/* open a shared d3dkmt object from its name */
DECL_HANDLER(d3dkmt_object_open_name)
{
    struct unicode_str name = get_req_unicode_str();

    switch (req->type)
    {
    case D3DKMT_SYNC:
        reply->handle = open_object( current->process, req->rootdir, req->access, &dxgk_shared_sync_ops,
                                     name, req->attributes | OBJ_CASE_INSENSITIVE );
        break;
    case D3DKMT_RESOURCE:
        reply->handle = open_object( current->process, req->rootdir, req->access, &dxgk_shared_resource_ops,
                                     name, req->attributes | OBJ_CASE_INSENSITIVE );
        break;
    default:
        set_error( STATUS_INVALID_PARAMETER );
        break;
    }
}

/* Acquire a global d3dkmt keyed mutex */
DECL_HANDLER(d3dkmt_mutex_acquire)
{
    struct d3dkmt_mutex *mutex;
    struct object *sync;

    if (!(mutex = d3dkmt_object_open( req->mutex, D3DKMT_MUTEX ))) return;

    if (req->wait_status) set_error( req->wait_status );
    else if (mutex->abandoned) set_error( STATUS_ABANDONED );
    else if (mutex->key_value == req->key_value && !mutex->owner)
    {
        reply->fence_value = mutex->fence_value;
        mutex_grab( mutex );
    }
    else if ((reply->wait_handle = req->wait_handle)) set_error( STATUS_PENDING );
    else if ((sync = keyed_wait_grab( mutex, req->key_value )))
    {
        if ((reply->wait_handle = alloc_handle( current->process, sync, SYNCHRONIZE, 0 ))) set_error( STATUS_PENDING );
        release_object( sync );
    }

    release_object( mutex );

    if (get_error() != STATUS_PENDING && req->wait_handle) keyed_wait_release( mutex, req->key_value );
}

/* Release a global d3dkmt keyed mutex */
DECL_HANDLER(d3dkmt_mutex_release)
{
    struct d3dkmt_mutex *mutex;

    if (!(mutex = d3dkmt_object_open( req->mutex, D3DKMT_MUTEX ))) return;

    if (mutex->abandoned) set_error( STATUS_ABANDONED );
    else if (mutex->owner != current) set_error( STATUS_INVALID_PARAMETER );
    else
    {
        mutex->key_value = req->key_value;
        mutex->fence_value = req->fence_value;
        mutex_release( mutex, req->abandon );
    }

    release_object( mutex );
}

#define COMPOSITION_QUERY_STATE  0x0001
#define COMPOSITION_MODIFY_STATE 0x0002
#define COMPOSITION_ALL_ACCESS   (STANDARD_RIGHTS_REQUIRED | COMPOSITION_QUERY_STATE | COMPOSITION_MODIFY_STATE)

static const WCHAR composition_name[] = {'C','o','m','p','o','s','i','t','i','o','n'};

struct type_descr composition_type =
{
    { composition_name, sizeof(composition_name) },
    COMPOSITION_ALL_ACCESS,
    {
        STANDARD_RIGHTS_READ | COMPOSITION_QUERY_STATE,
        STANDARD_RIGHTS_WRITE | COMPOSITION_MODIFY_STATE,
        STANDARD_RIGHTS_EXECUTE,
        COMPOSITION_ALL_ACCESS,
    },
};

struct dcomp_surface
{
    struct object obj;
    struct dcomp_binding *binding;
    struct list subscribers;
    unsigned int generation;
};

struct dcomp_subscription;

struct dcomp_subscription_entry
{
    struct list surface_entry;
    struct dcomp_subscription *subscription;
    struct dcomp_surface *surface;
    unsigned int generation;
};

struct dcomp_subscription
{
    struct object obj;
    struct event *event;
    unsigned int count;
    struct dcomp_subscription_entry *entries;
};

struct dcomp_binding
{
    struct object obj;
    struct dcomp_surface *surface;
    unsigned int width;
    unsigned int height;
    unsigned int format;
    unsigned int alpha_mode;
    struct luid adapter_luid;
    unsigned int device_uuid[4];
    unsigned int memory_type_index;
    unsigned int buffer_count;
    unsigned int front_buffer;
    unsigned int generation;
    int has_front;
    unsigned int lease_counts[16];
    struct object **buffers;
    struct object **syncs;
    struct event *available_event;
};

struct dcomp_buffer_lease
{
    struct object obj;
    struct dcomp_binding *binding;
    unsigned int buffer;
};

static void dcomp_surface_dump( struct object *obj, int verbose );
static void dcomp_subscription_dump( struct object *obj, int verbose );
static void dcomp_subscription_destroy( struct object *obj );

static const struct object_ops dcomp_surface_ops =
{
    .size = sizeof(struct dcomp_surface),
    .type = &composition_type,
    .dump = dcomp_surface_dump,
};

static const WCHAR composition_subscription_name[] =
    {'C','o','m','p','o','s','i','t','i','o','n','S','u','b','s','c','r','i','p','t','i','o','n'};

static struct type_descr composition_subscription_type =
{
    { composition_subscription_name, sizeof(composition_subscription_name) },
    STANDARD_RIGHTS_REQUIRED,
    {
        STANDARD_RIGHTS_READ,
        STANDARD_RIGHTS_WRITE,
        STANDARD_RIGHTS_EXECUTE,
        STANDARD_RIGHTS_REQUIRED,
    },
};

static const struct object_ops dcomp_subscription_ops =
{
    .size = sizeof(struct dcomp_subscription),
    .type = &composition_subscription_type,
    .dump = dcomp_subscription_dump,
    .destroy = dcomp_subscription_destroy,
};

static void dcomp_subscription_dump( struct object *obj, int verbose )
{
    struct dcomp_subscription *subscription = (struct dcomp_subscription *)obj;

    assert( obj->ops == &dcomp_subscription_ops );
    fprintf( stderr, "DirectComposition subscription event=%p surfaces=%u\n",
             subscription->event, subscription->count );
}

static void dcomp_subscription_destroy( struct object *obj )
{
    struct dcomp_subscription *subscription = (struct dcomp_subscription *)obj;
    unsigned int i;

    assert( obj->ops == &dcomp_subscription_ops );
    for (i = 0; i < subscription->count; ++i)
    {
        list_remove( &subscription->entries[i].surface_entry );
        release_object( subscription->entries[i].surface );
    }
    if (subscription->event) release_object( subscription->event );
    free( subscription->entries );
}

static void dcomp_signal_subscribers( struct dcomp_surface *surface, unsigned int generation )
{
    struct dcomp_subscription_entry *entry;

    LIST_FOR_EACH_ENTRY( entry, &surface->subscribers,
            struct dcomp_subscription_entry, surface_entry )
    {
        if (entry->generation == generation) continue;
        entry->generation = generation;
        set_event( entry->subscription->event );
    }
}

static void dcomp_surface_dump( struct object *obj, int verbose )
{
    struct dcomp_surface *surface = (struct dcomp_surface *)obj;

    assert( obj->ops == &dcomp_surface_ops );
    fprintf( stderr, "DirectComposition surface binding=%p\n", surface->binding );
}

static const WCHAR composition_binding_name[] =
    {'C','o','m','p','o','s','i','t','i','o','n','B','i','n','d','i','n','g'};

static struct type_descr composition_binding_type =
{
    { composition_binding_name, sizeof(composition_binding_name) },
    STANDARD_RIGHTS_REQUIRED,
    {
        STANDARD_RIGHTS_READ,
        STANDARD_RIGHTS_WRITE,
        STANDARD_RIGHTS_EXECUTE,
        STANDARD_RIGHTS_REQUIRED,
    },
};

static void dcomp_binding_dump( struct object *obj, int verbose );
static void dcomp_binding_destroy( struct object *obj );
static void dcomp_buffer_lease_dump( struct object *obj, int verbose );
static void dcomp_buffer_lease_destroy( struct object *obj );

static const struct object_ops dcomp_binding_ops =
{
    .size = sizeof(struct dcomp_binding),
    .type = &composition_binding_type,
    .dump = dcomp_binding_dump,
    .destroy = dcomp_binding_destroy,
};

static const WCHAR composition_buffer_lease_name[] =
    {'C','o','m','p','o','s','i','t','i','o','n','B','u','f','f','e','r','L','e','a','s','e'};

static struct type_descr composition_buffer_lease_type =
{
    { composition_buffer_lease_name, sizeof(composition_buffer_lease_name) },
    STANDARD_RIGHTS_REQUIRED,
    {
        STANDARD_RIGHTS_READ,
        STANDARD_RIGHTS_WRITE,
        STANDARD_RIGHTS_EXECUTE,
        STANDARD_RIGHTS_REQUIRED,
    },
};

static const struct object_ops dcomp_buffer_lease_ops =
{
    .size = sizeof(struct dcomp_buffer_lease),
    .type = &composition_buffer_lease_type,
    .dump = dcomp_buffer_lease_dump,
    .destroy = dcomp_buffer_lease_destroy,
};

static void dcomp_buffer_lease_dump( struct object *obj, int verbose )
{
    struct dcomp_buffer_lease *lease = (struct dcomp_buffer_lease *)obj;

    assert( obj->ops == &dcomp_buffer_lease_ops );
    fprintf( stderr, "DirectComposition buffer lease binding=%p buffer=%u\n",
             lease->binding, lease->buffer );
}

static void dcomp_buffer_lease_destroy( struct object *obj )
{
    struct dcomp_buffer_lease *lease = (struct dcomp_buffer_lease *)obj;

    assert( obj->ops == &dcomp_buffer_lease_ops );
    assert( lease->binding->lease_counts[lease->buffer] );
    --lease->binding->lease_counts[lease->buffer];
    set_event( lease->binding->available_event );
    release_object( lease->binding );
}

static obj_handle_t dcomp_create_buffer_lease( struct dcomp_binding *binding,
        unsigned int buffer )
{
    struct dcomp_buffer_lease *lease;
    obj_handle_t handle = 0;

    if (!(lease = alloc_object( &dcomp_buffer_lease_ops ))) return 0;
    lease->binding = (struct dcomp_binding *)grab_object( binding );
    lease->buffer = buffer;
    ++binding->lease_counts[buffer];
    handle = alloc_handle_no_access_check( current->process, lease,
            STANDARD_RIGHTS_REQUIRED, 0 );
    release_object( lease );
    return handle;
}

static void dcomp_binding_dump( struct object *obj, int verbose )
{
    struct dcomp_binding *binding = (struct dcomp_binding *)obj;

    assert( obj->ops == &dcomp_binding_ops );
    fprintf( stderr, "DirectComposition binding surface=%p size=%ux%u format=%u alpha=%u buffers=%u front=%u generation=%u has_front=%u\n",
             binding->surface, binding->width, binding->height, binding->format,
             binding->alpha_mode, binding->buffer_count, binding->front_buffer,
             binding->generation, binding->has_front );
}

static void dcomp_binding_destroy( struct object *obj )
{
    struct dcomp_binding *binding = (struct dcomp_binding *)obj;
    unsigned int i;

    assert( obj->ops == &dcomp_binding_ops );
    if (binding->surface)
    {
        assert( binding->surface->binding == binding );
        binding->surface->binding = NULL;
        dcomp_signal_subscribers( binding->surface, ++binding->surface->generation );
        release_object( binding->surface );
    }
    for (i = 0; i < binding->buffer_count; ++i)
    {
        if (binding->buffers[i]) release_object( binding->buffers[i] );
        if (binding->syncs[i]) release_object( binding->syncs[i] );
    }
    if (binding->available_event) release_object( binding->available_event );
    free( binding->buffers );
    free( binding->syncs );
}

static struct object **dcomp_get_buffers( unsigned int count, const obj_handle_t *handles )
{
    struct object **buffers;
    unsigned int i;

    if (!count || count > 16)
    {
        set_error( STATUS_INVALID_PARAMETER );
        return NULL;
    }
    if (!(buffers = mem_alloc( count * sizeof(*buffers) ))) return NULL;
    memset( buffers, 0, count * sizeof(*buffers) );
    for (i = 0; i < count; ++i)
    {
        if (!(buffers[i] = get_handle_obj( current->process, handles[i], 0,
                &dxgk_shared_resource_ops )))
            break;
    }
    if (i != count)
    {
        while (i--) release_object( buffers[i] );
        free( buffers );
        return NULL;
    }
    return buffers;
}

static struct object *dcomp_get_sync( obj_handle_t handle )
{
    if (!handle)
    {
        set_error( STATUS_INVALID_PARAMETER );
        return NULL;
    }
    return get_handle_obj( current->process, handle, 0, &dxgk_shared_sync_ops );
}

DECL_HANDLER(dcomp_create_surface)
{
    struct dcomp_surface *surface;

    if ((surface = alloc_object( &dcomp_surface_ops )))
    {
        surface->binding = NULL;
        list_init( &surface->subscribers );
        surface->generation = 0;
        reply->handle = alloc_handle_no_access_check( current->process, surface,
                req->access, req->attributes );
        release_object( surface );
    }
}

DECL_HANDLER(dcomp_open_surface)
{
    struct dcomp_surface *surface;

    if ((surface = (struct dcomp_surface *)get_handle_obj(current->process, req->handle,
            COMPOSITION_QUERY_STATE, &dcomp_surface_ops)))
    {
        reply->handle = alloc_handle_no_access_check(current->process, surface,
                COMPOSITION_QUERY_STATE, 0);
        release_object(surface);
    }
}

DECL_HANDLER(dcomp_bind_surface)
{
    const struct dcomp_surface_info *info = get_req_data();
    const obj_handle_t *handles;
    struct dcomp_surface *surface;
    struct dcomp_binding *binding;
    struct object **buffers;
    struct object **syncs;
    unsigned int i;

    if (get_req_data_size() < sizeof(*info) || get_req_data_size() != req->payload_size
            || !info->buffer_count || info->buffer_count > 16
            || get_req_data_size() != sizeof(*info)
            + info->buffer_count * sizeof(*handles)
            || !info->width || !info->height || info->width > 16384 || info->height > 16384
            || (info->alpha_mode != 1 && info->alpha_mode != 3)
            || info->memory_type_index >= 32 || info->resize || info->sync_resource)
    {
        set_error( STATUS_INVALID_PARAMETER );
        return;
    }

    handles = (const obj_handle_t *)(info + 1);
    if (!(buffers = dcomp_get_buffers( info->buffer_count, handles ))) return;
    if (!(syncs = mem_alloc( info->buffer_count * sizeof(*syncs) )))
    {
        for (i = 0; i < info->buffer_count; ++i) release_object( buffers[i] );
        free( buffers );
        return;
    }
    memset( syncs, 0, info->buffer_count * sizeof(*syncs) );
    if (!(surface = (struct dcomp_surface *)get_handle_obj( current->process, req->surface,
            COMPOSITION_MODIFY_STATE, &dcomp_surface_ops )))
    {
        for (i = 0; i < info->buffer_count; ++i) release_object( buffers[i] );
        free( buffers );
        free( syncs );
        return;
    }

    if (surface->binding)
    {
        set_error( STATUS_DEVICE_BUSY );
        release_object( surface );
        for (i = 0; i < info->buffer_count; ++i) release_object( buffers[i] );
        free( buffers );
        free( syncs );
        return;
    }

    if ((binding = alloc_object( &dcomp_binding_ops )))
    {
        binding->surface = surface;
        binding->width = info->width;
        binding->height = info->height;
        binding->format = info->format;
        binding->alpha_mode = info->alpha_mode;
        binding->adapter_luid = info->adapter_luid;
        binding->device_uuid[0] = info->device_uuid0;
        binding->device_uuid[1] = info->device_uuid1;
        binding->device_uuid[2] = info->device_uuid2;
        binding->device_uuid[3] = info->device_uuid3;
        binding->memory_type_index = info->memory_type_index;
        binding->buffer_count = info->buffer_count;
        binding->front_buffer = 0;
        binding->generation = 0;
        binding->has_front = 0;
        memset( binding->lease_counts, 0, sizeof(binding->lease_counts) );
        binding->buffers = buffers;
        binding->syncs = syncs;
        binding->available_event = create_event( NULL, empty_str, 0, 1, 1, NULL );
        if (!binding->available_event)
        {
            binding->surface = NULL;
            release_object( surface );
            release_object( binding );
            return;
        }
        surface->binding = binding;
        dcomp_signal_subscribers( surface, ++surface->generation );
        if (!(reply->binding = alloc_handle_no_access_check( current->process, binding,
                STANDARD_RIGHTS_REQUIRED, 0 ))
                || !(reply->available_event = alloc_handle( current->process,
                binding->available_event, SYNCHRONIZE, 0 )))
        {
            if (reply->binding) close_handle( current->process, reply->binding );
            reply->binding = 0;
            surface->binding = NULL;
            binding->surface = NULL;
            release_object( surface );
        }
        release_object( binding );
        return;
    }

    release_object( surface );
    for (i = 0; i < info->buffer_count; ++i) release_object( buffers[i] );
    free( buffers );
    free( syncs );
}

DECL_HANDLER(dcomp_update_surface)
{
    const struct dcomp_surface_info *info = get_req_data();
    const obj_handle_t *handles;
    struct dcomp_binding *binding;
    struct object **buffers = NULL, **syncs = NULL, **old_buffers, **old_syncs;
    struct object *sync = NULL;
    unsigned int old_count, next_buffer = ~0u, i;

    if (get_req_data_size() < sizeof(*info) || get_req_data_size() != req->payload_size
            || !info->buffer_count || info->buffer_count > 16
            || get_req_data_size() != sizeof(*info)
            + (info->resize ? info->buffer_count * sizeof(*handles) : 0))
    {
        set_error( STATUS_INVALID_PARAMETER );
        return;
    }
    handles = (const obj_handle_t *)(info + 1);
    if (info->resize && !(buffers = dcomp_get_buffers( info->buffer_count, handles ))) return;
    if (info->resize)
    {
        if (info->sync_resource || !(syncs = mem_alloc( info->buffer_count * sizeof(*syncs) )))
        {
            if (info->sync_resource) set_error( STATUS_INVALID_PARAMETER );
            goto done;
        }
        memset( syncs, 0, info->buffer_count * sizeof(*syncs) );
    }
    /* A producer whose device cannot export a shareable timeline semaphore
     * publishes without a sync object; it has waited for its own device to go
     * idle before getting here instead. */
    else if (info->sync_resource && !(sync = dcomp_get_sync( info->sync_resource ))) goto done;

    if (!(binding = (struct dcomp_binding *)get_handle_obj( current->process, req->binding,
            0, &dcomp_binding_ops ))) goto done;

    if (!info->width || !info->height || info->width > 16384 || info->height > 16384
            || (info->alpha_mode != 1 && info->alpha_mode != 3)
            || info->memory_type_index >= 32
            || info->front_buffer >= info->buffer_count)
        set_error( STATUS_INVALID_PARAMETER );
    else if (!info->resize && (info->width != binding->width || info->height != binding->height
            || info->format != binding->format || info->alpha_mode != binding->alpha_mode
            || memcmp( &info->adapter_luid, &binding->adapter_luid, sizeof(binding->adapter_luid) )
            || info->device_uuid0 != binding->device_uuid[0]
            || info->device_uuid1 != binding->device_uuid[1]
            || info->device_uuid2 != binding->device_uuid[2]
            || info->device_uuid3 != binding->device_uuid[3]
            || info->memory_type_index != binding->memory_type_index
            || info->buffer_count != binding->buffer_count))
        set_error( STATUS_INVALID_PARAMETER );
    else if (info->resize)
    {
        for (i = 0; i < binding->buffer_count; ++i)
            if (binding->lease_counts[i]) break;
        if (i != binding->buffer_count) set_error( STATUS_DEVICE_BUSY );
        else next_buffer = 0;
    }
    else
    {
        if (binding->lease_counts[info->front_buffer])
            set_error( STATUS_DEVICE_BUSY );
        else if (binding->buffer_count == 1)
            next_buffer = info->front_buffer;
        else for (i = 1; i < binding->buffer_count; ++i)
        {
            unsigned int candidate = (info->front_buffer + i) % binding->buffer_count;
            if (!binding->lease_counts[candidate])
            {
                next_buffer = candidate;
                break;
            }
        }
        if (next_buffer == ~0u) set_error( STATUS_DEVICE_BUSY );
    }

    if (get_error() == STATUS_DEVICE_BUSY) reset_event( binding->available_event );

    if (!get_error())
    {
        old_buffers = binding->buffers;
        old_syncs = binding->syncs;
        old_count = binding->buffer_count;
        binding->width = info->width;
        binding->height = info->height;
        binding->format = info->format;
        binding->alpha_mode = info->alpha_mode;
        binding->adapter_luid = info->adapter_luid;
        binding->device_uuid[0] = info->device_uuid0;
        binding->device_uuid[1] = info->device_uuid1;
        binding->device_uuid[2] = info->device_uuid2;
        binding->device_uuid[3] = info->device_uuid3;
        binding->memory_type_index = info->memory_type_index;
        binding->buffer_count = info->buffer_count;
        binding->front_buffer = info->front_buffer;
        binding->has_front = !info->resize;
        if (info->resize)
        {
            binding->buffers = buffers;
            binding->syncs = syncs;
        }
        else
        {
            if (binding->syncs[info->front_buffer])
                release_object( binding->syncs[info->front_buffer] );
            binding->syncs[info->front_buffer] = sync;
            sync = NULL;
        }
        if (info->resize) memset( binding->lease_counts, 0, sizeof(binding->lease_counts) );
        ++binding->generation;
        dcomp_signal_subscribers( binding->surface, ++binding->surface->generation );
        if (info->resize)
        {
            buffers = NULL;
            syncs = NULL;
            for (i = 0; i < old_count; ++i) release_object( old_buffers[i] );
            for (i = 0; i < old_count; ++i)
                if (old_syncs[i]) release_object( old_syncs[i] );
            free( old_buffers );
            free( old_syncs );
        }
        reply->next_buffer = next_buffer;
        set_event( binding->available_event );
    }

    release_object( binding );
done:
    if (sync) release_object( sync );
    if (buffers)
    {
        for (i = 0; i < info->buffer_count; ++i) release_object( buffers[i] );
        free( buffers );
    }
    free( syncs );
}

DECL_HANDLER(dcomp_query_surface)
{
    struct dcomp_surface *surface;
    struct dcomp_binding *binding;

    if (!(surface = (struct dcomp_surface *)get_handle_obj( current->process,
            req->surface, COMPOSITION_QUERY_STATE, &dcomp_surface_ops ))) return;
    if (!(binding = surface->binding))
        set_error( STATUS_NOT_FOUND );
    else if (!binding->has_front)
        set_error( STATUS_NOT_FOUND );
    else if ((reply->resource = alloc_handle_no_access_check( current->process,
            binding->buffers[binding->front_buffer], STANDARD_RIGHTS_READ, 0 )))
    {
        if (binding->syncs[binding->front_buffer]
                && !(reply->sync_resource = alloc_handle_no_access_check( current->process,
                binding->syncs[binding->front_buffer], STANDARD_RIGHTS_READ, 0 )))
        {
            close_handle( current->process, reply->resource );
            reply->resource = 0;
            set_error( STATUS_NOT_FOUND );
            release_object( surface );
            return;
        }
        reply->width = binding->width;
        reply->height = binding->height;
        reply->format = binding->format;
        reply->alpha_mode = binding->alpha_mode;
        reply->adapter_luid = binding->adapter_luid;
        reply->buffer_count = binding->buffer_count;
        reply->front_buffer = binding->front_buffer;
        reply->generation = surface->generation;
        reply->has_front = binding->has_front;
    }
    release_object( surface );
}

DECL_HANDLER(dcomp_subscribe_surfaces)
{
    const struct dcomp_subscription_input *inputs = get_req_data();
    struct dcomp_surface_snapshot *snapshots = NULL;
    struct dcomp_subscription *subscription = NULL;
    struct event *event = NULL;
    unsigned int count, i;

    if (get_req_data_size() != req->surfaces_size
            || req->surfaces_size % sizeof(*inputs))
    {
        set_error( STATUS_INVALID_PARAMETER );
        return;
    }
    count = req->surfaces_size / sizeof(*inputs);
    if (count > 4096 || get_reply_max_size() < count * sizeof(*snapshots))
    {
        set_error( STATUS_BUFFER_TOO_SMALL );
        return;
    }
    if (!(event = get_event_obj( current->process, req->event, EVENT_MODIFY_STATE ))) return;
    if (!(subscription = alloc_object( &dcomp_subscription_ops ))) goto failed;
    subscription->event = NULL;
    subscription->count = 0;
    subscription->entries = NULL;
    if (count && (!(snapshots = mem_alloc( count * sizeof(*snapshots) ))
            || !(subscription->entries = mem_alloc( count * sizeof(*subscription->entries) ))))
        goto failed;

    memset( snapshots, 0, count * sizeof(*snapshots) );
    if (count) memset( subscription->entries, 0, count * sizeof(*subscription->entries) );
    subscription->event = (struct event *)grab_object( event );

    for (i = 0; i < count; ++i)
    {
        struct dcomp_subscription_entry *entry = &subscription->entries[i];
        struct dcomp_surface *surface;
        struct dcomp_binding *binding;

        if (!(surface = (struct dcomp_surface *)get_handle_obj( current->process,
                inputs[i].surface, COMPOSITION_QUERY_STATE, &dcomp_surface_ops )))
            goto failed;
        entry->subscription = subscription;
        entry->surface = surface;
        list_add_tail( &surface->subscribers, &entry->surface_entry );
        ++subscription->count;

        snapshots[i].generation = surface->generation;
        entry->generation = surface->generation;
        if (!(binding = surface->binding)) continue;
        snapshots[i].width = binding->width;
        snapshots[i].height = binding->height;
        snapshots[i].format = binding->format;
        snapshots[i].alpha_mode = binding->alpha_mode;
        snapshots[i].adapter_luid = binding->adapter_luid;
        snapshots[i].device_uuid0 = binding->device_uuid[0];
        snapshots[i].device_uuid1 = binding->device_uuid[1];
        snapshots[i].device_uuid2 = binding->device_uuid[2];
        snapshots[i].device_uuid3 = binding->device_uuid[3];
        snapshots[i].memory_type_index = binding->memory_type_index;
        snapshots[i].buffer_count = binding->buffer_count;
        snapshots[i].front_buffer = binding->front_buffer;
        snapshots[i].has_front = binding->has_front;
        if (binding->has_front && !(snapshots[i].resource = alloc_handle_no_access_check(
                current->process, binding->buffers[binding->front_buffer],
                STANDARD_RIGHTS_READ, 0 )))
            goto failed;
        if (binding->has_front && binding->syncs[binding->front_buffer]
                && !(snapshots[i].sync_resource = alloc_handle_no_access_check(
                current->process, binding->syncs[binding->front_buffer],
                STANDARD_RIGHTS_READ, 0)))
            goto failed;
        if (binding->has_front && !(snapshots[i].lease = dcomp_create_buffer_lease(
                binding, binding->front_buffer)))
            goto failed;
    }

    if (!(reply->subscription = alloc_handle_no_access_check( current->process,
            subscription, STANDARD_RIGHTS_REQUIRED, 0 )))
        goto failed;
    reply->snapshots_size = count * sizeof(*snapshots);
    if (count) set_reply_data( snapshots, reply->snapshots_size );
    release_object( subscription );
    release_object( event );
    free( snapshots );
    return;

failed:
    if (snapshots)
        for (i = 0; i < count; ++i)
        {
            if (snapshots[i].resource) close_handle( current->process, snapshots[i].resource );
            if (snapshots[i].sync_resource)
                close_handle( current->process, snapshots[i].sync_resource );
            if (snapshots[i].lease) close_handle( current->process, snapshots[i].lease );
        }
    if (subscription) release_object( subscription );
    if (event) release_object( event );
    free( snapshots );
}
