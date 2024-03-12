// SPDX-FileCopyrightText: Copyright 2022 sudachi Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/core.h"
#include "core/hle/kernel/k_scoped_resource_reservation.h"
#include "core/hle/kernel/k_system_resource.h"

namespace Kernel {

Result KSecureSystemResource::Initialize(size_t size, KResourceLimit* resource_limit,
                                         KMemoryManager::Pool pool) {
    // Set members.
    m_resource_limit = resource_limit;
    m_resource_size = size;
    m_resource_pool = pool;

    // Determine required size for our secure resource.
    const size_t secure_size = this->CalculateRequiredSecureMemorySize();

    // Reserve memory for our secure resource.
    KScopedResourceReservation memory_reservation(
        m_resource_limit, Svc::LimitableResource::PhysicalMemoryMax, secure_size);
    R_UNLESS(memory_reservation.Succeeded(), ResultLimitReached);

    // Allocate secure memory.
    R_TRY(KSystemControl::AllocateSecureMemory(m_kernel, std::addressof(m_resource_address),
                                               m_resource_size, static_cast<u32>(m_resource_pool)));
    ASSERT(m_resource_address != 0);

    // Ensure we clean up the secure memory, if we fail past this point.
    ON_RESULT_FAILURE {
        KSystemControl::FreeSecureMemory(m_kernel, m_resource_address, m_resource_size,
                                         static_cast<u32>(m_resource_pool));
    };

    // Check that our allocation is bigger than the reference counts needed for it.
    const size_t rc_size =
        Common::AlignUp(KPageTableSlabHeap::CalculateReferenceCountSize(m_resource_size), PageSize);
    R_UNLESS(m_resource_size > rc_size, ResultOutOfMemory);

    // Get resource pointer.
    KPhysicalAddress resource_paddr =
        KPageTable::GetHeapPhysicalAddress(m_kernel, m_resource_address);
    auto* resource =
        m_kernel.System().DeviceMemory().GetPointer<KPageTableManager::RefCount>(resource_paddr);

    // Initialize slab heaps.
    m_dynamic_page_manager.Initialize(m_resource_address + rc_size, m_resource_size - rc_size,
                                      PageSize);
    m_page_table_heap.Initialize(std::addressof(m_dynamic_page_manager), 0, resource);
    m_memory_block_heap.Initialize(std::addressof(m_dynamic_page_manager), 0);
    m_block_info_heap.Initialize(std::addressof(m_dynamic_page_manager), 0);

    // Initialize managers.
    m_page_table_manager.Initialize(std::addressof(m_dynamic_page_manager),
                                    std::addressof(m_page_table_heap));
    m_memory_block_slab_manager.Initialize(std::addressof(m_dynamic_page_manager),
                                           std::addressof(m_memory_block_heap));
    m_block_info_manager.Initialize(std::addressof(m_dynamic_page_manager),
                                    std::addressof(m_block_info_heap));

    // Set our managers.
    this->SetManagers(m_memory_block_slab_manager, m_block_info_manager, m_page_table_manager);

    // Commit the memory reservation.
    memory_reservation.Commit();

    // Open reference to our resource limit.
    m_resource_limit->Open();

    // Set ourselves as initialized.
    m_is_initialized = true;

    R_SUCCEED();
}

void KSecureSystemResource::Finalize() {
    // Check that we have no outstanding allocations.
    ASSERT(m_memory_block_slab_manager.GetUsed() == 0);
    ASSERT(m_block_info_manager.GetUsed() == 0);
    ASSERT(m_page_table_manager.GetUsed() == 0);

    // Free our secure memory.
    KSystemControl::FreeSecureMemory(m_kernel, m_resource_address, m_resource_size,
                                     static_cast<u32>(m_resource_pool));

    // Release the memory reservation.
    m_resource_limit->Release(Svc::LimitableResource::PhysicalMemoryMax,
                              this->CalculateRequiredSecureMemorySize());

    // Close reference to our resource limit.
    m_resource_limit->Close();
}

size_t KSecureSystemResource::CalculateRequiredSecureMemorySize(size_t size,
                                                                KMemoryManager::Pool pool) {
    return KSystemControl::CalculateRequiredSecureMemorySize(size, static_cast<u32>(pool));
}

} // namespace Kernel
