// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <magenta/compiler.h>
#include <magenta/syscalls.h>
#include <magenta/syscalls/port.h>
#include <magenta/threads.h>
#include <mx/event.h>
#include <mx/object.h>
#include <mx/port.h>
#include <mxalloc/new.h>
#include <mxio/debug.h>
#include <mxio/dispatcher.h>
#include <mxtl/algorithm.h>
#include <mxtl/auto_lock.h>
#include <fs/vfs-dispatcher.h>

#define MXDEBUG 0

// ** NOTE -- this prototype multithreaded dispatcher is only used by
// ** minfs as part of ongoing multithread development. Not yet safe
// ** for general consumption

namespace fs {

Handler::~Handler() {
    Close();
}

mx_status_t Handler::SetAsyncCallback(const mx::port& dispatch_port) {
    // queue a message on port whenever h_ is readable or closed
    return h_.wait_async(dispatch_port, (uint64_t)(uintptr_t)this,
                         MX_CHANNEL_READABLE|MX_CHANNEL_PEER_CLOSED,
                         MX_WAIT_ASYNC_ONCE);
}

mx_status_t Handler::CancelAsyncCallback(const mx::port& dispatch_port) {
    return dispatch_port.cancel(h_.get(), (uint64_t)(uintptr_t)this);
}

void Handler::Close() {
    h_.reset();
}

VfsDispatcher::VfsDispatcher(mxio_dispatcher_cb_t cb, uint32_t pool_size) :
    cb_(cb), pool_size_(pool_size), n_threads_(0) {
    mtx_init(&lock_, mtx_plain);
}

VfsDispatcher::~VfsDispatcher() {
    mx_status_t status;

    // *up to clients to lock to prevent add/run activity during destructor*

    // kill off worker threads, so no new cb activity
    // - send suicide events -- existing queue clears then workers exit
    // - join all threads
    // close port so no queue new activity; no new handlers can be added,
    // - remaining messages discarded
    // clean up and delete remaining handlers

    // suicide: cause worker threads to wake and die
    // (ideally, we could close the port and the threads would die on their own)
    // shut down existing handlers (to prevent races)
    if ((status = shutdown_event_.signal(0u, MX_EVENT_SIGNALED)) != MX_OK) {
        FS_TRACE_ERROR("couldn't send kill signal to thread\n");
    }

    // reap worker threads
    for (unsigned i = 0; i < n_threads_; i++) {
        int rc;
        int r = thrd_join(t_[i], &rc);
        if (r != thrd_success) {
            printf("mxio_dispatcher_destroy: join failure %d\n", r);
        }
    }
}

static void GetThreadName(char* name, size_t namelen) {
    mx_status_t r = mx_object_get_property(thrd_get_mx_handle(thrd_current()),
                                           MX_PROP_NAME, name, namelen);
    if (r != MX_OK)
        strncpy(name, "???", namelen);
}

void VfsDispatcher::DisconnectHandler(Handler* handler, bool need_close_cb) {
    // close handle, so we get no further messages
    handler->Close();

    if (need_close_cb) {
        handler->ExecuteCloseCallback(cb_);
    }
}

int VfsDispatcher::Loop() {
    mx_status_t r;

    // when draining queue, limit the number of messages you take
    // at once, so you don't dominate the cpu
    constexpr unsigned kMaxMessageBatchSize = 4;
    char tname[128];
    GetThreadName(tname, sizeof(tname));

    for (;;) {
        mx_port_packet_t packet;

        if ((r = port_.wait(MX_TIME_INFINITE, &packet, 0u)) < 0) {
            xprintf("mxio_dispatcher: port wait failed %d, worker exiting\n", r);
            return MX_OK;
        }

        xprintf("port_wait: thread %s \n", tname);

        if ((packet.signal.observed & MX_EVENT_SIGNALED) != 0) {
            // reset for the next thread
            r = shutdown_event_.wait_async(port_, 0u, MX_EVENT_SIGNALED,
                                           MX_WAIT_ASYNC_ONCE);
            if (r != MX_OK) {
                FS_TRACE_ERROR("vfs-dispatcher: error, couldn't reset thread event\n");
            }
            // exit thread
            xprintf("%s: suicide\n", tname);
            return r;
        }

        xprintf("thrd_: port_wait: returns key %p effective:%#x \n",
                (void*)packet.key, packet.signal.observed);

        Handler* handler = (Handler*)(uintptr_t)packet.key;

        if (packet.signal.observed & MX_CHANNEL_READABLE) {
            // hit cb multiple times if we know multi packets available
            for (unsigned ix = 0; ix < mxtl::min(kMaxMessageBatchSize, (unsigned)packet.signal.count); ++ix) {
                if ((r = handler->ExecuteCallback(cb_)) != MX_OK) {
                    // error or close: invoke callback in case of error
                    DisconnectHandler(handler, r != ERR_DISPATCHER_DONE);
                    goto free_handler;
                }
            }
            // maybe more work to do: re-arm handler to fire again
            if ((r = handler->SetAsyncCallback(port_))!= MX_OK){
                DisconnectHandler(handler, true);
                goto free_handler;
            }
        } else if (packet.signal.observed & MX_CHANNEL_PEER_CLOSED) {
            DisconnectHandler(handler, true);
        free_handler:
            {
                mxtl::AutoLock md_lock(&lock_);
                handlers_.erase(*handler);
            }
        }

    }

    // fatal error -- exiting thread
    return MX_OK;
}

mx_status_t VfsDispatcher::Create(mxio_dispatcher_cb_t cb, uint32_t pool_size,
                                  mxtl::unique_ptr<fs::Dispatcher>* out) {
    AllocChecker ac;
    mxtl::unique_ptr<fs::VfsDispatcher> dispatcher(new (&ac) fs::VfsDispatcher(cb, pool_size));
    if (!ac.check()) {
        return MX_ERR_NO_MEMORY;
    }

    thrd_t* t = new (&ac) thrd_t[pool_size];
    if (!ac.check()) {
        return MX_ERR_NO_MEMORY;
    }
    dispatcher->t_.reset(t, pool_size);

    mx_status_t status;
    if ((status = mx::port::create(0, &dispatcher->port_)) != MX_OK) {
        return status;
    }
    if ((status = mx::event::create(0u, &dispatcher->shutdown_event_)) != MX_OK) {
        return status;
    }
    status = dispatcher->shutdown_event_.wait_async(dispatcher->port_, 0u,
                                                    MX_EVENT_SIGNALED,
                                                    MX_WAIT_ASYNC_ONCE);
    if (status != MX_OK) {
        return status;
    }

    if ((status = dispatcher->Start("VFS Dispatcher")) != MX_OK) {
        return status;
    }
    *out = mxtl::move(dispatcher);
    return MX_OK;
}

static int mxio_dispatcher_thread(void* arg) {
    VfsDispatcher* md = reinterpret_cast<VfsDispatcher*>(arg);
    return md->Loop();
}

mx_status_t VfsDispatcher::Start(const char* name) {
    char namebuf[NAME_MAX];

    mxtl::AutoLock md_lock(&lock_);
    mx_status_t r;

    if (n_threads_ != 0) {
        // already initialized
        return MX_ERR_BAD_STATE;
    }

    xprintf("starting dispatcher with %d threads\n", pool_size_);
    for (uint32_t i = 0; i < pool_size_; i++) {
        if (pool_size_ > 1) {
            snprintf(namebuf, sizeof(namebuf), "%s-%u", name, n_threads_);
        } else {
            snprintf(namebuf, sizeof(namebuf), "%s", name);
        }

        xprintf("start thread %s\n", namebuf);
        if ((r = thrd_create_with_name(&t_[n_threads_], mxio_dispatcher_thread,
                                       this, namebuf)) != thrd_success) {
            return MX_ERR_NO_RESOURCES;
        } else {
            n_threads_++;
        }
    }

    return MX_OK;
}

mx_status_t VfsDispatcher::AddVFSHandler(mx_handle_t h, vfs_dispatcher_cb_t cb, void* cookie) {
    AllocChecker ac;
    mxtl::unique_ptr<Handler> handler(new (&ac) Handler(h, cb, cookie));
    if (!ac.check()) {
        return MX_ERR_NO_MEMORY;
    }

    mxtl::AutoLock md_lock(&lock_);

    // set us up to receive read/close callbacks from handler on port_
    mx_status_t status;
    if ((status = handler->SetAsyncCallback(port_)) != MX_OK) {
        return status;
    } else {
        handlers_.push_back(mxtl::move(handler));
    }

    return MX_OK;
}

} // namespace fs
