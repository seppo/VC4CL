/*
 * Author: doe300
 *
 * See the file "LICENSE" for the full license governing this code. See the copyright statement below for the original
 * code:
 */
/*
Copyright (c) 2012, Broadcom Europe Ltd.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the copyright holder nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "Mailbox.h"

#include "Allocator.h"
#include "V3D.h"
#include "hal.h"

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <memory>
#include <system_error>

using namespace vc4cl;

#define MAJOR_NUM 100
#define IOCTL_MBOX_PROPERTY _IOWR(MAJOR_NUM, 0, char*)
#define DEVICE_FILE_NAME "/dev/vcio"

DeviceBuffer::DeviceBuffer(
    const std::shared_ptr<DeviceBlock>& block, DevicePointer devPtr, void* hostPtr, uint32_t size) :
    qpuPointer(devPtr),
    hostPointer(hostPtr), size(size), block(block)
{
}

DeviceBuffer::~DeviceBuffer()
{
    block->deallocateBuffer(hostPointer, size);
}

void DeviceBuffer::dumpContent() const
{
    // TODO rewrite
    for(unsigned i = 0; i < size / sizeof(unsigned); ++i)
    {
        if((i % 8) == 0)
            printf("\n[VC4CL] %08x:", static_cast<unsigned>(static_cast<unsigned>(qpuPointer) + i * sizeof(unsigned)));
        printf(" %08x", reinterpret_cast<const unsigned*>(hostPointer)[i]);
    }
}

static int mbox_open()
{
    int file_desc;

    // open a char device file used for communicating with kernel mbox driver
    file_desc = open_mailbox(DEVICE_FILE_NAME, 0);
    if(file_desc < 0)
    {
        std::cout << "[VC4CL] Can't open device file: " << DEVICE_FILE_NAME << std::endl;
        std::cout << "[VC4CL] Try creating a device file with: sudo mknod " << DEVICE_FILE_NAME << " c " << MAJOR_NUM
                  << " 0" << std::endl;
        throw std::system_error(errno, std::system_category(), "Failed to open mailbox");
    }
    DEBUG_LOG(DebugLevel::SYSCALL, std::cout << "[VC4CL] Mailbox file descriptor opened: " << file_desc << std::endl)
    return file_desc;
}

Mailbox::Mailbox() : fd(mbox_open())
{
    if(!enableQPU(true))
        throw std::runtime_error("Failed to enable QPUs!");
}

Mailbox::~Mailbox()
{
    ignoreReturnValue(enableQPU(false) ? CL_SUCCESS : CL_OUT_OF_RESOURCES, __FILE__, __LINE__,
        "There is no way of handling an error here");
    close_mailbox(fd);
    DEBUG_LOG(DebugLevel::SYSCALL, std::cout << "[VC4CL] Mailbox file descriptor closed: " << fd << std::endl)
}

std::pair<void*, std::shared_ptr<DeviceBlock>> Mailbox::allocateBlock(
    unsigned sizeInBytes, unsigned alignmentInBytes, MemoryFlag flags)
{
    // munmap requires an alignment of the system page size (4096), so we need to enforce it here
    unsigned handle =
        memAlloc(sizeInBytes, std::max(static_cast<unsigned>(DEVICE_PAGE_ALIGNMENT), alignmentInBytes), flags);
    if(handle != 0)
    {
        DevicePointer qpuPointer = memLock(handle);
        void* hostPointer = mapmem(V3D::busAddressToPhysicalAddress(static_cast<unsigned>(qpuPointer)), sizeInBytes);
        DEBUG_LOG(DebugLevel::DEVICE_MEMORY,
            std::cout << "Allocated " << sizeInBytes << " bytes of device memory: handle " << handle
                      << ", device address " << std::hex << "0x" << qpuPointer << ", host address " << hostPointer
                      << std::dec << std::endl)
        return std::make_pair(hostPointer,
            std::make_shared<DeviceBlock>(
                shared_from_this(), handle, DevicePointer{qpuPointer}, hostPointer, sizeInBytes, flags));
    }
    return std::make_pair(nullptr, nullptr);
}

std::unique_ptr<DeviceBuffer> Mailbox::allocateKernelBuffer(
    unsigned sizeInBytes, unsigned alignmentInBytes, MemoryFlag flags)
{
    // Kernels always use a new device block, since A) they might use different flags in the future and B) they likely
    // use more than (half of) a page anyway
    // For the same reasons, also do not add kernel buffers to the cache
    std::lock_guard<std::mutex> guard(allocationLock);
    auto newBlock = allocateBlock(sizeInBytes, alignmentInBytes, flags).second;
    return newBlock->allocateBuffer(sizeInBytes, alignmentInBytes);
}

std::shared_ptr<DeviceBuffer> Mailbox::allocateDataBuffer(
    unsigned sizeInBytes, unsigned alignmentInBytes, MemoryFlag flags)
{
    std::lock_guard<std::mutex> guard(allocationLock);
    if(sizeInBytes >= DEVICE_PAGE_ALIGNMENT)
    {
        // uses more then single block, allocate own block
        auto newBlock = allocateBlock(sizeInBytes, alignmentInBytes, flags).second;
        return newBlock->allocateBuffer(sizeInBytes, alignmentInBytes);
    }
    // Check if we can reuse any previously allocated device blocks
    for(auto& weakBlock : cachedBlocks)
    {
        auto block = weakBlock.second.lock();
        if(!block || block->flags != flags)
            continue;
        if(auto buffer = block->allocateBuffer(sizeInBytes, alignmentInBytes))
            return buffer;
    }
    // allocate new block and add to cache
    auto newBlock = allocateBlock(DEVICE_PAGE_ALIGNMENT, alignmentInBytes, flags);
    cachedBlocks.emplace(newBlock.first, newBlock.second);
    return newBlock.second->allocateBuffer(sizeInBytes, alignmentInBytes);
}

bool Mailbox::deallocateBlock(unsigned memHandle, void* hostPointer, DevicePointer qpuPointer, uint32_t size)
{
    {
        std::lock_guard<std::mutex> guard(allocationLock);
        // remove from cache
        cachedBlocks.erase(hostPointer);
    }
    if(hostPointer)
        unmapmem(hostPointer, size);
    if(memHandle != 0)
    {
        if(!memUnlock(memHandle))
            return false;
        if(!memFree(memHandle))
            return false;
        DEBUG_LOG(DebugLevel::DEVICE_MEMORY,
            std::cout << "Deallocated " << size << " bytes of device memory: handle " << memHandle
                      << ", device address " << std::hex << "0x" << qpuPointer << ", host address " << hostPointer
                      << std::dec << std::endl)
    }
    return true;
}

ExecutionHandle Mailbox::executeCode(uint32_t codeAddress, unsigned valueR0, unsigned valueR1, unsigned valueR2,
    unsigned valueR3, unsigned valueR4, unsigned valueR5) const
{
    MailboxMessage<MailboxTag::EXECUTE_CODE, 7, 1> msg(
        {codeAddress, valueR0, valueR1, valueR2, valueR3, valueR4, valueR5});
    if(mailboxCall(msg.buffer.data()) < 0)
        return ExecutionHandle{false};
    return ExecutionHandle{msg.getContent(0) == 0};
}

ExecutionHandle Mailbox::executeQPU(unsigned numQPUs, std::pair<uint32_t*, uint32_t> controlAddress, bool flushBuffer,
    std::chrono::milliseconds timeout) const
{
    if(timeout.count() > 0xFFFFFFFF)
    {
        DEBUG_LOG(DebugLevel::SYSCALL,
            std::cout << "Timeout is too big, needs fit into a 32-bit integer: " << timeout.count() << std::endl)
        return ExecutionHandle{false};
    }
    /*
     * "By default the qpu_execute call does a GPU side L1 and L2 data cache flush before executing the qpu code. If you
     * are happy it is safe not to do this, setting noflush=1 will be a little quicker." see:
     * https://github.com/raspberrypi/firmware/issues/747
     */
    MailboxMessage<MailboxTag::EXECUTE_QPU, 4, 1> msg(
        {numQPUs, controlAddress.second, static_cast<unsigned>(!flushBuffer), static_cast<unsigned>(timeout.count())});
    if(mailboxCall(msg.buffer.data()) < 0)
        return ExecutionHandle{false};
    return ExecutionHandle{msg.getContent(0) == 0};
}

uint32_t Mailbox::getTotalGPUMemory() const
{
    SimpleQueryMessage<MailboxTag::VC_MEMORY> msg;
    if(!readMailboxMessage(msg))
        return 0;
    return msg.getContent(1);
}

/*
 * use ioctl to send mbox property message
 */
int Mailbox::mailboxCall(void* buffer) const
{
    unsigned* p = reinterpret_cast<unsigned*>(buffer);
    unsigned size = *p;
    DEBUG_LOG(DebugLevel::SYSCALL, {
        std::cout << "Mailbox buffer before:";
        for(unsigned i = 0; i < size / 4; ++i)
            std::cout << ' ' << std::hex << std::setfill('0') << std::setw(8) << p[i] << std::dec;
        std::cout << std::endl;
    })

    int ret_val = ioctl_mailbox(fd, IOCTL_MBOX_PROPERTY, buffer);
    if(ret_val < 0)
    {
        DEBUG_LOG(DebugLevel::SYSCALL, std::cout << "ioctl_set_msg failed: " << ret_val << std::endl)
        perror("[VC4CL] Error in mbox_property");
        throw std::system_error(errno, std::system_category(), "Failed to set mailbox property");
    }

    DEBUG_LOG(DebugLevel::SYSCALL, {
        std::cout << "Mailbox buffer after:";
        for(unsigned i = 0; i < size / 4; ++i)
            std::cout << ' ' << std::hex << std::setfill('0') << std::setw(8) << p[i] << std::dec;
        std::cout << std::endl;
    })
    return ret_val;
}

bool Mailbox::enableQPU(bool enable) const
{
    QueryMessage<MailboxTag::ENABLE_QPU> msg({static_cast<unsigned>(enable)});
    if(mailboxCall(msg.buffer.data()) < 0)
        return false;
    /*
     * If the mailbox is already running/being used, 0x80000000 is returned (see #16).
     * This seems to be also true for shutting down the mailbox,
     * which hints to some kind of reference-counter within the VC4 hard-/firmware
     * only returning 0 for the first open/last close and 0x80000000 otherwise.
     */
    return msg.getContent(0) == 0 || msg.getContent(0) == 0x80000000;
}

unsigned Mailbox::memAlloc(unsigned sizeInBytes, unsigned alignmentInBytes, MemoryFlag flags) const
{
    MailboxMessage<MailboxTag::ALLOCATE_MEMORY, 3, 1> msg({sizeInBytes, alignmentInBytes, flags});
    if(mailboxCall(msg.buffer.data()) < 0)
        return 0;
    return msg.getContent(0);
}

DevicePointer Mailbox::memLock(unsigned handle) const
{
    QueryMessage<MailboxTag::LOCK_MEMORY> msg({handle});
    if(mailboxCall(msg.buffer.data()) < 0)
        return DevicePointer(0);
    return DevicePointer(msg.getContent(0));
}

bool Mailbox::memUnlock(unsigned handle) const
{
    QueryMessage<MailboxTag::UNLOCK_MEMORY> msg({handle});
    if(mailboxCall(msg.buffer.data()) < 0)
        return false;
    return msg.getContent(0) == 0;
}

bool Mailbox::memFree(unsigned handle) const
{
    QueryMessage<MailboxTag::RELEASE_MEMORY> msg({handle});
    if(mailboxCall(msg.buffer.data()) < 0)
        return false;
    return msg.getContent(0) == 0;
}

CHECK_RETURN bool Mailbox::checkReturnValue(unsigned value) const
{
    if((value >> 31) == 1) // 0x8000000x
    {
        // 0x80000000 on success
        // 0x80000001 on failure
        DEBUG_LOG(DebugLevel::SYSCALL,
            std::cout << "Mailbox request: " << (((value & 0x1) == 0x1) ? "failed" : "succeeded") << std::endl)
        return value == 0x80000000;
    }
    else
    {
        DEBUG_LOG(DebugLevel::SYSCALL, std::cout << "Unknown return code: " << value << std::endl)
        return false;
    }
}

std::shared_ptr<Mailbox>& vc4cl::mailbox()
{
    static std::shared_ptr<Mailbox> mb(new Mailbox());
    return mb;
}
