/*
 * Minimal ARM64 KVM smoke test.
 *
 * Creates a VM and one vCPU, puts one page of code at 0x40000000, runs
 * PSCI SYSTEM_OFF, and expects KVM_EXIT_SYSTEM_EVENT/SHUTDOWN.
 *
 * Build on the ARM64 host:
 *   cc -O2 -Wall -o kvm-arm64-smoke kvm-arm64-smoke.c
 *   ./kvm-arm64-smoke
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/kvm.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define GUEST_ADDR 0x40000000UL
#define GUEST_SIZE 0x1000UL

#define CORE_REG(name) \
    (KVM_REG_ARM64 | KVM_REG_SIZE_U64 | KVM_REG_ARM_CORE | \
     KVM_REG_ARM_CORE_REG(name))

static void die(const char *what)
{
    fprintf(stderr, "%s: %s\n", what, strerror(errno));
    exit(1);
}

static void set_reg(int vcpu, unsigned long id, uint64_t value)
{
    struct kvm_one_reg reg = {
        .id = id,
        .addr = (uint64_t)(uintptr_t)&value,
    };
    if (ioctl(vcpu, KVM_SET_ONE_REG, &reg) < 0)
        die("KVM_SET_ONE_REG");
}

int main(void)
{
    int kvm = open("/dev/kvm", O_RDWR | O_CLOEXEC);
    if (kvm < 0)
        die("open /dev/kvm");

    int version = ioctl(kvm, KVM_GET_API_VERSION, 0);
    if (version != KVM_API_VERSION) {
        fprintf(stderr, "unexpected KVM API version %d\n", version);
        return 1;
    }

    int vm = ioctl(kvm, KVM_CREATE_VM, 0);
    if (vm < 0)
        die("KVM_CREATE_VM");

    void *guest = mmap(NULL, GUEST_SIZE, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (guest == MAP_FAILED)
        die("mmap guest");

    /*
     * 0x00: ldr x0, #8       ; load PSCI_SYSTEM_OFF
     * 0x04: hvc #0           ; PSCI call
     * 0x08: .quad 0x84000008
     */
    const uint32_t code[] = {
        0x58000040, 0xd4000002, 0x00000000, 0x00000000,
        0x84000008, 0x00000000,
    };
    memcpy(guest, code, sizeof code);

    struct kvm_userspace_memory_region memory = {
        .slot = 0,
        .flags = 0,
        .guest_phys_addr = GUEST_ADDR,
        .memory_size = GUEST_SIZE,
        .userspace_addr = (uint64_t)(uintptr_t)guest,
    };
    if (ioctl(vm, KVM_SET_USER_MEMORY_REGION, &memory) < 0)
        die("KVM_SET_USER_MEMORY_REGION");

    struct kvm_vcpu_init init;
    memset(&init, 0, sizeof init);
    if (ioctl(vm, KVM_ARM_PREFERRED_TARGET, &init) < 0)
        die("KVM_ARM_PREFERRED_TARGET");

    int vcpu = ioctl(vm, KVM_CREATE_VCPU, 0);
    if (vcpu < 0)
        die("KVM_CREATE_VCPU");
    if (ioctl(vcpu, KVM_ARM_VCPU_INIT, &init) < 0)
        die("KVM_ARM_VCPU_INIT");

    int run_size = ioctl(kvm, KVM_GET_VCPU_MMAP_SIZE, 0);
    if (run_size <= 0)
        die("KVM_GET_VCPU_MMAP_SIZE");
    struct kvm_run *run = mmap(NULL, run_size, PROT_READ | PROT_WRITE,
                               MAP_SHARED, vcpu, 0);
    if (run == MAP_FAILED)
        die("mmap kvm_run");

    set_reg(vcpu, CORE_REG(regs.pc), GUEST_ADDR);
    set_reg(vcpu, CORE_REG(regs.pstate), 0x3c5); /* EL1h, DAIF masked */

    if (ioctl(vcpu, KVM_RUN, 0) < 0)
        die("KVM_RUN");

    printf("KVM ARM64 smoke: exit_reason=%u\n", run->exit_reason);
    if (run->exit_reason != KVM_EXIT_SYSTEM_EVENT) {
        fprintf(stderr, "expected KVM_EXIT_SYSTEM_EVENT, got %u\n",
                run->exit_reason);
        return 1;
    }
    printf("KVM ARM64 smoke: system_event.type=%u\n",
           run->system_event.type);
    if (run->system_event.type != KVM_SYSTEM_EVENT_SHUTDOWN) {
        fprintf(stderr, "expected SHUTDOWN, got %u\n",
                run->system_event.type);
        return 1;
    }
    puts("KVM ARM64 smoke: OK");
    return 0;
}
