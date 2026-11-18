/*
 * kvm-arm64-probe: figure out why the smoke test never exits.
 *
 * Runs several stages, each with an alarm so a wedged vCPU is reported
 * instead of hanging:
 *   1. read back pc/pstate after KVM_SET_ONE_REG
 *   2. execute a store to an unmapped IPA (expect KVM_EXIT_MMIO)
 *   3. then PSCI SYSTEM_OFF with the PSCI_0_2 vcpu feature
 *
 *   cc -O2 -Wall -o kvm-arm64-probe kvm-arm64-probe.c
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/kvm.h>
#include <signal.h>
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
#define MMIO_ADDR  0x10000000UL

static int vcpu_fd = -1;
static struct kvm_run *run;

#define CORE_ID(name) \
    (KVM_REG_ARM64 | KVM_REG_SIZE_U64 | KVM_REG_ARM_CORE | \
     KVM_REG_ARM_CORE_REG(name))

static void die(const char *what)
{
    fprintf(stderr, "%s: %s\n", what, strerror(errno));
    exit(1);
}

static void set_reg(int vcpu, unsigned long id, uint64_t value)
{
    struct kvm_one_reg reg = { .id = id, .addr = (uint64_t)(uintptr_t)&value };
    if (ioctl(vcpu, KVM_SET_ONE_REG, &reg) < 0)
        die("KVM_SET_ONE_REG");
}

static int get_reg(int vcpu, unsigned long id, uint64_t *value)
{
    struct kvm_one_reg reg = { .id = id, .addr = (uint64_t)(uintptr_t)value };
    return ioctl(vcpu, KVM_GET_ONE_REG, &reg);
}

static void dump(const char *why)
{
    uint64_t pc = 0, pstate = 0, x0 = 0;

    fprintf(stderr, "\n[probe] %s\n", why);
    if (get_reg(vcpu_fd, CORE_ID(regs.pc), &pc) == 0)
        fprintf(stderr, "[probe] pc     = 0x%016llx\n", (unsigned long long)pc);
    if (get_reg(vcpu_fd, CORE_ID(regs.pstate), &pstate) == 0)
        fprintf(stderr, "[probe] pstate = 0x%016llx\n", (unsigned long long)pstate);
    if (get_reg(vcpu_fd, CORE_ID(regs.regs[0]), &x0) == 0)
        fprintf(stderr, "[probe] x0     = 0x%016llx\n", (unsigned long long)x0);
    if (run != NULL)
        fprintf(stderr, "[probe] run exit_reason = %u (0 = still in guest)\n",
                run->exit_reason);
    fflush(stderr);
}

static void on_alarm(int sig)
{
    (void)sig;
    dump("KVM_RUN wedged (3s)");
    _exit(2);
}

int main(void)
{
    int kvm = open("/dev/kvm", O_RDWR | O_CLOEXEC);
    if (kvm < 0)
        die("open /dev/kvm");

    printf("api=%d\n", ioctl(kvm, KVM_GET_API_VERSION, 0));
    printf("KVM_CAP_ARM_PSCI_0_2=%d\n",
           ioctl(kvm, KVM_CHECK_EXTENSION, KVM_CAP_ARM_PSCI_0_2));
    printf("KVM_CAP_ARM_VM_IPA_SIZE=%d\n",
           ioctl(kvm, KVM_CHECK_EXTENSION, KVM_CAP_ARM_VM_IPA_SIZE));

    int vm = ioctl(kvm, KVM_CREATE_VM, 0);
    if (vm < 0)
        die("KVM_CREATE_VM");

    void *guest = mmap(NULL, GUEST_SIZE, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (guest == MAP_FAILED)
        die("mmap guest");

    /*
     * 0x00 movz x1, #0x1000, lsl #16   ; x1 = 0x10000000
     * 0x04 str x1, [x1]                ; MMIO store -> KVM_EXIT_MMIO
     * 0x08 ldr x0, #8                  ; x0 = PSCI SYSTEM_OFF
     * 0x0c hvc #0
     * 0x10 .quad 0x84000008
     */
    const uint32_t code[] = {
        0xd2a20001, /* movz x1, #0x1000, lsl #16 */
        0xf9000021, /* str  x1, [x1]            */
        0x58000040, /* ldr  x0, #8              */
        0xd4000002, /* hvc  #0                  */
        0x00000000, 0x00000000,
        0x84000008, 0x00000000,
    };
    memcpy(guest, code, sizeof code);

    struct kvm_userspace_memory_region memory = {
        .slot = 0,
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
    printf("target=%u features=0x%08x\n",
           init.target, init.features[0]);

    if (ioctl(kvm, KVM_CHECK_EXTENSION, KVM_CAP_ARM_PSCI_0_2) > 0) {
        init.features[0] |= (1u << KVM_ARM_VCPU_PSCI_0_2);
        printf("enabling KVM_ARM_VCPU_PSCI_0_2\n");
    }

    vcpu_fd = ioctl(vm, KVM_CREATE_VCPU, 0);
    if (vcpu_fd < 0)
        die("KVM_CREATE_VCPU");
    if (ioctl(vcpu_fd, KVM_ARM_VCPU_INIT, &init) < 0)
        die("KVM_ARM_VCPU_INIT");

    int run_size = ioctl(kvm, KVM_GET_VCPU_MMAP_SIZE, 0);
    run = mmap(NULL, run_size, PROT_READ | PROT_WRITE, MAP_SHARED, vcpu_fd, 0);
    if (run == MAP_FAILED)
        die("mmap kvm_run");

    set_reg(vcpu_fd, CORE_ID(regs.pc), GUEST_ADDR);
    set_reg(vcpu_fd, CORE_ID(regs.pstate), 0x3c5);
    dump("after setting pc/pstate");

    signal(SIGALRM, on_alarm);

    /* stage 1: the MMIO store proves the guest actually executes */
    alarm(3);
    if (ioctl(vcpu_fd, KVM_RUN, 0) < 0)
        die("KVM_RUN 1");
    alarm(0);
    printf("stage 1: exit_reason=%u\n", run->exit_reason);
    if (run->exit_reason == KVM_EXIT_MMIO)
        printf("stage 1: mmio phys=0x%llx len=%u is_write=%u data=0x%llx\n",
               (unsigned long long)run->mmio.phys_addr, run->mmio.len,
               run->mmio.is_write, (unsigned long long)run->mmio.data[0]);
    dump("after stage 1");

    /* stage 2: retry from the HVC, with x0 already loaded */
    set_reg(vcpu_fd, CORE_ID(regs.pc), GUEST_ADDR + 8);
    set_reg(vcpu_fd, CORE_ID(regs.regs[0]), 0x84000008ULL);
    alarm(3);
    if (ioctl(vcpu_fd, KVM_RUN, 0) < 0)
        die("KVM_RUN 2");
    alarm(0);
    printf("stage 2: exit_reason=%u\n", run->exit_reason);
    if (run->exit_reason == KVM_EXIT_SYSTEM_EVENT)
        printf("stage 2: system_event type=%u\n", run->system_event.type);
    dump("after stage 2");
    return 0;
}
