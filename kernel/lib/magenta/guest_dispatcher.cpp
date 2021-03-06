// Copyright 2017 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <arch/hypervisor.h>
#include <kernel/vm/vm_object.h>
#include <magenta/fifo_dispatcher.h>
#include <magenta/guest_dispatcher.h>
#include <magenta/rights.h>
#include <mxalloc/new.h>

// static
mx_status_t GuestDispatcher::Create(mxtl::RefPtr<VmObject> physmem,
                                    mxtl::RefPtr<Dispatcher>* dispatcher,
                                    mx_rights_t* rights) {
    mxtl::unique_ptr<Guest> guest;
    status_t status = arch_guest_create(physmem, &guest);
    if (status != MX_OK)
        return status;

    AllocChecker ac;
    auto disp = new (&ac) GuestDispatcher(mxtl::move(guest));
    if (!ac.check())
        return MX_ERR_NO_MEMORY;

    *rights = MX_DEFAULT_GUEST_RIGHTS;
    *dispatcher = mxtl::AdoptRef<Dispatcher>(disp);
    return MX_OK;
}

GuestDispatcher::GuestDispatcher(mxtl::unique_ptr<Guest> guest)
    : guest_(mxtl::move(guest)) {}

GuestDispatcher::~GuestDispatcher() {}

mx_status_t GuestDispatcher::SetTrap(mx_trap_address_space_t aspace, mx_vaddr_t addr, size_t len,
                                     mxtl::RefPtr<FifoDispatcher> fifo) {
    canary_.Assert();

    return arch_guest_set_trap(guest_.get(), aspace, addr, len, fifo);
}
