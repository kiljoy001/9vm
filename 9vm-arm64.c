/*
 * 9vm-arm64: the ARM64 board layer for the same microVM as 9vm.
 *
 * The guest is the Harvey/ARM64 kernel image (a raw Linux-style Image or
 * an ELF).  This file supplies everything QEMU's "virt" machine would:
 *
 *   - guest RAM at 0x40000000 with the Image at 0x40080000
 *   - a device tree in x0 describing memory, cpu, psci, GICv3, the
 *     architected timer, PL011 and (later) virtio-mmio transports
 *   - a PL011 UART emulated here, so early console output appears
 *   - KVM's in-kernel GICv3, so timer and UART interrupts are delivered
 *
 * The addresses and interrupt numbers mirror QEMU virt on purpose: the
 * same kernel image boots under either, and its device tree is the only
 * thing that has to agree.
 *
 *   cc -O2 -Wall -Wextra -o 9vm-arm64 9vm-arm64.c
 *   ./9vm-arm64 -m 512 harvey.img
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/kvm.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define RAM_BASE        0x40000000UL
#define KERNEL_PA       0x40080000UL
#define DTB_PA          0x48000000UL
#define DTB_RESERVE     0x100000UL

#define GICD_BASE       0x08000000UL
#define GICD_SIZE       0x10000UL
#define GICR_BASE       0x080A0000UL
#define GICR_SIZE       0x20000UL       /* one vCPU: 2 * 64 KiB */

#define UART_BASE       0x09000000UL
#define UART_SIZE       0x1000UL
#define UART_SPI        1
#define UART_IRQ        (32 + UART_SPI) /* KVM_IRQ_LINE takes the GIC id */

#define VIRTIO_MMIO_BASE 0x0a000000UL
#define VIRTIO_MMIO_STRIDE 0x200UL
#define VIRTIO_MMIO_SLOTS 8
#define VIRTIO_MMIO_IRQ0 16            /* SPIs 16.. as in QEMU virt */

#define DEFAULT_RAM_MB  512
#define DEFAULT_CMDLINE "console=ttyAMA0"

/* ---- big-endian helpers (FDT is big-endian throughout) ---- */

static void
be32(uint8_t *p, uint32_t v)
{
	p[0] = v >> 24;
	p[1] = v >> 16;
	p[2] = v >> 8;
	p[3] = v;
}

static void
be64(uint8_t *p, uint64_t v)
{
	be32(p, (uint32_t)(v >> 32));
	be32(p + 4, (uint32_t)v);
}

/* ---- device tree builder ---- */

#define FDT_MAGIC       0xd00dfeed
#define FDT_BEGIN_NODE  0x1
#define FDT_END_NODE    0x2
#define FDT_PROP        0x3
#define FDT_END         0x9

#define FDT_MAX         65536
#define FDT_STRUCT_OFF  0x1000
#define FDT_STRINGS_MAX 4096

struct fdt {
	uint8_t *base;
	size_t len;
	char strings[FDT_STRINGS_MAX];
	size_t slen;
};

static void
fdt_put(struct fdt *f, const void *p, size_t n)
{
	if(f->len + n > FDT_MAX){
		fprintf(stderr, "[9vm-arm64] device tree overflow\n");
		exit(1);
	}
	if(n != 0)
		memcpy(f->base + f->len, p, n);
	f->len += n;
}

static void
fdt_put32(struct fdt *f, uint32_t v)
{
	uint8_t b[4];

	be32(b, v);
	fdt_put(f, b, 4);
}

static void
fdt_align(struct fdt *f)
{
	size_t n = (4 - (f->len & 3)) & 3;

	if(n != 0)
		fdt_put(f, "\0\0\0", n);
}

static size_t
fdt_string(struct fdt *f, const char *name)
{
	size_t off = f->slen, n = strlen(name) + 1;

	if(f->slen + n > sizeof f->strings){
		fprintf(stderr, "[9vm-arm64] device tree strings overflow\n");
		exit(1);
	}
	memcpy(f->strings + f->slen, name, n);
	f->slen += n;
	return off;
}

static void
fdt_begin(struct fdt *f, const char *name)
{
	fdt_put32(f, FDT_BEGIN_NODE);
	fdt_put(f, name, strlen(name) + 1);
	fdt_align(f);
}

static void
fdt_end(struct fdt *f)
{
	fdt_put32(f, FDT_END_NODE);
}

static void
fdt_prop(struct fdt *f, const char *name, const void *data, size_t len)
{
	fdt_put32(f, FDT_PROP);
	fdt_put32(f, (uint32_t)len);
	fdt_put32(f, (uint32_t)fdt_string(f, name));
	fdt_put(f, data, len);
	fdt_align(f);
}

static void
fdt_prop_u32(struct fdt *f, const char *name, uint32_t v)
{
	uint8_t b[4];

	be32(b, v);
	fdt_prop(f, name, b, 4);
}

static void
fdt_prop_str(struct fdt *f, const char *name, const char *s)
{
	fdt_prop(f, name, s, strlen(s) + 1);
}

/* NUL-separated list, e.g. compatible strings */
static void
fdt_prop_strlist(struct fdt *f, const char *name, const char *const *v, int n)
{
	char buf[256];
	size_t off = 0;

	for(int i = 0; i < n; i++){
		size_t l = strlen(v[i]) + 1;

		if(off + l > sizeof buf){
			fprintf(stderr, "[9vm-arm64] compatible list too long\n");
			exit(1);
		}
		memcpy(buf + off, v[i], l);
		off += l;
	}
	fdt_prop(f, name, buf, off);
}

/* reg-style property: pairs of 32-bit cells */
static void
fdt_prop_cells(struct fdt *f, const char *name, const uint32_t *cells, int n)
{
	uint8_t buf[64];

	if((size_t)n * 4 > sizeof buf){
		fprintf(stderr, "[9vm-arm64] too many cells\n");
		exit(1);
	}
	for(int i = 0; i < n; i++)
		be32(buf + i * 4, cells[i]);
	fdt_prop(f, name, buf, (size_t)n * 4);
}

#define PHANDLE_GIC  0x8000
#define PHANDLE_CPU  0x8001

static void
build_fdt(uint8_t *dtb, uint64_t ram_size, const char *cmdline)
{
	struct fdt f;
	uint32_t mem[4], gic[8], timer[12];
	const char *psci_compat[] = { "arm,psci-1.0", "arm,psci-0.2", "arm,psci" };
	const char *gic_compat[] = { "arm,gic-v3" };
	const char *uart_compat[] = { "arm,pl011", "arm,primecell" };
	const char *timer_compat[] = { "arm,armv8-timer" };
	uint32_t ncells;
	uint8_t hdr[40];

	memset(&f, 0, sizeof f);
	f.base = dtb;
	f.len = FDT_STRUCT_OFF;

	/* root */
	fdt_begin(&f, "");
	fdt_prop_u32(&f, "#address-cells", 2);
	fdt_prop_u32(&f, "#size-cells", 2);
	fdt_prop_str(&f, "compatible", "linux,dummy-virt");
	fdt_prop_str(&f, "model", "9vm,arm64-virt");
	fdt_prop_u32(&f, "interrupt-parent", PHANDLE_GIC);

	/* chosen */
	fdt_begin(&f, "chosen");
	fdt_prop_str(&f, "stdout-path", "/pl011@9000000");
	if(cmdline != NULL && cmdline[0] != 0)
		fdt_prop_str(&f, "bootargs", cmdline);
	fdt_end(&f);

	/* memory */
	fdt_begin(&f, "memory@40000000");
	fdt_prop_str(&f, "device_type", "memory");
	mem[0] = (uint32_t)(RAM_BASE >> 32);
	mem[1] = (uint32_t)RAM_BASE;
	mem[2] = (uint32_t)(ram_size >> 32);
	mem[3] = (uint32_t)ram_size;
	fdt_prop_cells(&f, "reg", mem, 4);
	fdt_end(&f);

	/* cpus */
	fdt_begin(&f, "cpus");
	fdt_prop_u32(&f, "#address-cells", 1);
	fdt_prop_u32(&f, "#size-cells", 0);
	fdt_begin(&f, "cpu@0");
	fdt_prop_str(&f, "device_type", "cpu");
	fdt_prop_str(&f, "compatible", "arm,cortex-a57");
	fdt_prop_u32(&f, "reg", 0);
	fdt_prop_u32(&f, "phandle", PHANDLE_CPU);
	fdt_end(&f);
	fdt_end(&f);

	/* psci */
	fdt_begin(&f, "psci");
	fdt_prop_str(&f, "method", "hvc");
	fdt_prop_strlist(&f, "compatible", psci_compat, 3);
	fdt_prop_u32(&f, "cpu_suspend", 0xc4000001);
	fdt_prop_u32(&f, "cpu_off", 0x84000002);
	fdt_prop_u32(&f, "cpu_on", 0xc4000003);
	fdt_prop_u32(&f, "migrate", 0xc4000005);
	fdt_end(&f);

	/* GICv3: distributor then redistributor */
	fdt_begin(&f, "intc@8000000");
	fdt_prop_strlist(&f, "compatible", gic_compat, 1);
	fdt_prop_u32(&f, "#interrupt-cells", 3);
	fdt_prop(&f, "interrupt-controller", NULL, 0);
	fdt_prop_u32(&f, "#redistributor-regions", 1);
	fdt_prop_u32(&f, "phandle", PHANDLE_GIC);
	gic[0] = 0;
	gic[1] = (uint32_t)GICD_BASE;
	gic[2] = 0;
	gic[3] = GICD_SIZE;
	gic[4] = 0;
	gic[5] = (uint32_t)GICR_BASE;
	gic[6] = 0;
	gic[7] = GICR_SIZE;
	fdt_prop_cells(&f, "reg", gic, 8);
	fdt_end(&f);

	/* architected timer: secure phys, phys, virt, hyp; PPIs 13/14/11/10 */
	fdt_begin(&f, "timer");
	fdt_prop_strlist(&f, "compatible", timer_compat, 1);
	fdt_prop(&f, "always-on", NULL, 0);
	{
		static const uint32_t ppis[] = { 13, 14, 11, 10 };

		ncells = 0;
		for(size_t i = 0; i < sizeof ppis / sizeof ppis[0]; i++){
			timer[ncells++] = 1;
			timer[ncells++] = ppis[i];
			timer[ncells++] = 4;	/* level, active high */
		}
	}
	fdt_prop_cells(&f, "interrupts", timer, (int)ncells);
	fdt_end(&f);

	/* PL011 console */
	fdt_begin(&f, "pl011@9000000");
	fdt_prop_strlist(&f, "compatible", uart_compat, 2);
	{
		uint32_t reg[4] = { 0, (uint32_t)UART_BASE, 0, UART_SIZE };
		uint32_t irq[3] = { 0, UART_SPI, 4 };
		uint32_t clk[2] = { 0, 0x8002 };

		fdt_prop_cells(&f, "reg", reg, 4);
		fdt_prop_cells(&f, "interrupts", irq, 3);
		fdt_prop_cells(&f, "clocks", clk, 2);
	}
	fdt_end(&f);

	/* a fixed clock for the "clocks" reference above */
	fdt_begin(&f, "clk@0");
	fdt_prop_str(&f, "compatible", "fixed-clock");
	fdt_prop_u32(&f, "#clock-cells", 0);
	fdt_prop_u32(&f, "clock-frequency", 24000000);
	fdt_prop_u32(&f, "phandle", 0x8002);
	fdt_end(&f);

	/* virtio-mmio transports, one SPI each, as on QEMU virt */
	for(int i = 0; i < VIRTIO_MMIO_SLOTS; i++){
		char name[32];
		uint32_t reg[4], irq[3];
		const char *compat[] = { "virtio,mmio" };

		snprintf(name, sizeof name, "virtio_mmio@%lx",
		         (unsigned long)(VIRTIO_MMIO_BASE + (unsigned long)i * VIRTIO_MMIO_STRIDE));
		fdt_begin(&f, name);
		fdt_prop_strlist(&f, "compatible", compat, 1);
		fdt_prop(&f, "dma-coherent", NULL, 0);
		reg[0] = 0;
		reg[1] = (uint32_t)(VIRTIO_MMIO_BASE + (unsigned long)i * VIRTIO_MMIO_STRIDE);
		reg[2] = 0;
		reg[3] = VIRTIO_MMIO_STRIDE;
		fdt_prop_cells(&f, "reg", reg, 4);
		irq[0] = 0;
		irq[1] = VIRTIO_MMIO_IRQ0 + i;
		irq[2] = 4;
		fdt_prop_cells(&f, "interrupts", irq, 3);
		fdt_end(&f);
	}

	fdt_end(&f);	/* root */
	fdt_put32(&f, FDT_END);

	while(f.len & 3)
		fdt_put32(&f, 0);

	/* header + reserve map; the reserve map sits right after it */
	memset(dtb, 0, FDT_STRUCT_OFF);
	be32(hdr + 0, FDT_MAGIC);
	be32(hdr + 4, (uint32_t)(f.len + f.slen));
	be32(hdr + 8, FDT_STRUCT_OFF);
	be32(hdr + 12, (uint32_t)f.len);
	be32(hdr + 16, 0x28);
	be32(hdr + 20, 17);	/* version */
	be32(hdr + 24, 16);	/* last compatible version */
	be32(hdr + 28, 0);	/* boot cpu */
	be32(hdr + 32, (uint32_t)f.slen);
	be32(hdr + 36, (uint32_t)(f.len - FDT_STRUCT_OFF));
	memcpy(dtb, hdr, sizeof hdr);

	be64(dtb + 0x28, DTB_PA);
	be64(dtb + 0x30, DTB_RESERVE);
	be64(dtb + 0x38, 0);
	be64(dtb + 0x40, 0);

	memcpy(dtb + f.len, f.strings, f.slen);
}

/* ---- PL011 ---- */

struct uart {
	uint32_t cr;
	uint32_t lcrh;
	uint32_t imsc;
	uint32_t ibrd, fbrd;
	uint32_t ifls;
	uint32_t ris;
	uint8_t rx[64];
	unsigned rxhead, rxtail;
	int irq_level;
};

static struct uart uart;
static int vm_fd = -1;
static int tty_raw_set;
static struct termios tty_orig;
static volatile sig_atomic_t running = 1;

static void
uart_irq(int level)
{
	struct kvm_irq_level irq = { .irq = UART_IRQ, .level = (uint32_t)level };

	if(level == uart.irq_level)
		return;
	if(ioctl(vm_fd, KVM_IRQ_LINE, &irq) == 0)
		uart.irq_level = level;
}

static void
uart_update_irq(void)
{
	int rxready = uart.rxhead != uart.rxtail;
	uint32_t bits = uart.ris & uart.imsc;

	uart_irq((rxready && (bits & 0x10)) != 0);
}

static uint32_t
uart_read(uint32_t off)
{
	uint32_t v;

	switch(off & ~3){
	case 0x000:	/* DR */
		if(uart.rxhead == uart.rxtail)
			v = 0;
		else {
			v = uart.rx[uart.rxhead++ % sizeof uart.rx];
			uart.ris &= ~0x10;
			if(uart.rxhead != uart.rxtail)
				uart.ris |= 0x10;
		}
		uart_update_irq();
		return v;
	case 0x018:	/* FR: TX FIFO empty (TXFE), RX FIFO empty (RXFE) */
		v = 0x80;
		if(uart.rxhead == uart.rxtail)
			v |= 0x10;
		return v;
	case 0x034:	/* IMSC */
		return uart.imsc;
	case 0x038:	/* RIS */
		return uart.ris | ((uart.rxhead != uart.rxtail) ? 0x10 : 0);
	case 0x03c:	/* MIS */
		return (uart.ris | ((uart.rxhead != uart.rxtail) ? 0x10 : 0)) & uart.imsc;
	default:
		return 0;
	}
}

static void
uart_write(uint32_t off, uint32_t v)
{
	switch(off & ~3){
	case 0x000:	/* DR: the kernel polls FR and never sets CR, so
			 * always emit; this console has exactly one user */
		putchar((int)(v & 0xff));
		fflush(stdout);
		break;
	case 0x020:
		uart.ibrd = v;
		break;
	case 0x024:
		uart.fbrd = v;
		break;
	case 0x028:
		uart.lcrh = v;
		break;
	case 0x02c:
		uart.cr = v;
		break;
	case 0x030:
		uart.ifls = v;
		break;
	case 0x034:	/* IMSC */
		uart.imsc = v & 0x7ff;
		uart_update_irq();
		break;
	case 0x040:	/* ICR: clear the named interrupt bits */
		uart.ris &= ~v;
		uart_update_irq();
		break;
	default:
		break;
	}
}

/* ---- guest image loading ---- */

static uint64_t
load_image(const char *path, uint8_t *ram, uint64_t ram_size)
{
	struct stat st;
	int fd;
	uint8_t *buf;
	ssize_t n;

	if((fd = open(path, O_RDONLY)) < 0){
		perror(path);
		exit(1);
	}
	if(fstat(fd, &st) < 0 || st.st_size <= 0){
		fprintf(stderr, "[9vm-arm64] %s: bad size\n", path);
		exit(1);
	}
	buf = malloc((size_t)st.st_size);
	if(buf == NULL){
		fprintf(stderr, "[9vm-arm64] out of memory\n");
		exit(1);
	}
	n = read(fd, buf, (size_t)st.st_size);
	close(fd);
	if(n != st.st_size){
		fprintf(stderr, "[9vm-arm64] short read\n");
		exit(1);
	}

	if(n >= 4 && memcmp(buf, "\177ELF", 4) == 0){
		/* 64-bit LE ELF: load PT_LOAD segments at p_paddr */
		uint64_t phoff, phentsize, phnum, entry = ~0ULL;

		memcpy(&entry, buf + 24, 8);
		memcpy(&phoff, buf + 32, 8);
		memcpy(&phentsize, buf + 54, 2);
		memcpy(&phnum, buf + 56, 2);
		for(uint64_t i = 0; i < phnum; i++){
			uint8_t *ph = buf + phoff + i * phentsize;
			uint32_t type, flags;
			uint64_t off, pa, filesz, memsz;

			memcpy(&type, ph + 0, 4);
			memcpy(&flags, ph + 4, 4);
			memcpy(&off, ph + 8, 8);
			memcpy(&pa, ph + 24, 8);
			memcpy(&filesz, ph + 32, 8);
			memcpy(&memsz, ph + 40, 8);
			if(type != 1 || memsz == 0)
				continue;
			if(pa < RAM_BASE || pa + memsz > RAM_BASE + ram_size){
				fprintf(stderr, "[9vm-arm64] ELF segment 0x%llx+0x%llx outside RAM\n",
				        (unsigned long long)pa, (unsigned long long)memsz);
				exit(1);
			}
			memcpy(ram + (pa - RAM_BASE), buf + off, filesz);
			if(memsz > filesz)
				memset(ram + (pa - RAM_BASE) + filesz, 0, memsz - filesz);
			(void)flags;
			if(pa < entry)
				entry = pa;
		}
		free(buf);
		if(entry == ~0ULL)
			entry = KERNEL_PA;
		fprintf(stderr, "[9vm-arm64] ELF entry 0x%llx\n", (unsigned long long)entry);
		return entry;
	}

	if(KERNEL_PA + (uint64_t)n > DTB_PA){
		fprintf(stderr, "[9vm-arm64] Image too large for the space below the device tree\n");
		exit(1);
	}
	memcpy(ram + (KERNEL_PA - RAM_BASE), buf, (size_t)n);
	free(buf);
	fprintf(stderr, "[9vm-arm64] Image at 0x%llx (%lld bytes)\n",
	        (unsigned long long)KERNEL_PA, (long long)n);
	return KERNEL_PA;
}

/* ---- KVM ---- */

static void
die(const char *what)
{
	fprintf(stderr, "[9vm-arm64] %s: %s\n", what, strerror(errno));
	exit(1);
}

static void
set_one_reg(int vcpu, unsigned long id, uint64_t v)
{
	struct kvm_one_reg reg = { .id = id, .addr = (uint64_t)(uintptr_t)&v };

	if(ioctl(vcpu, KVM_SET_ONE_REG, &reg) < 0)
		die("KVM_SET_ONE_REG");
}

static void
vgic_attr(int dev, uint32_t group, uint64_t attr, void *addr)
{
	struct kvm_device_attr a = {
		.group = group,
		.attr = attr,
		.addr = (uint64_t)(uintptr_t)addr,
	};

	/* KVM_SET_DEVICE_ATTR is a device ioctl (the fd from
	 * KVM_CREATE_DEVICE), not a VM ioctl. */
	if(ioctl(dev, KVM_SET_DEVICE_ATTR, &a) < 0){
		fprintf(stderr, "[9vm-arm64] KVM_SET_DEVICE_ATTR fd=%d group=%u attr=%llu addr=0x%llx: %s\n",
		        dev, group, (unsigned long long)attr,
		        (unsigned long long)(uintptr_t)addr, strerror(errno));
		exit(1);
	}
}

static void
setup_vgic(int vm)
{
	struct kvm_create_device cd = {
		.type = KVM_DEV_TYPE_ARM_VGIC_V3,
		.flags = 0,
	};
	uint64_t base;

	if(ioctl(vm, KVM_CREATE_DEVICE, &cd) < 0)
		die("KVM_CREATE_DEVICE VGIC_V3");

	base = GICD_BASE;
	vgic_attr(cd.fd, KVM_DEV_ARM_VGIC_GRP_ADDR, KVM_VGIC_V3_ADDR_TYPE_DIST, &base);
	base = GICR_BASE;
	vgic_attr(cd.fd, KVM_DEV_ARM_VGIC_GRP_ADDR, KVM_VGIC_V3_ADDR_TYPE_REDIST, &base);
	struct kvm_device_attr ctl = {
		.group = KVM_DEV_ARM_VGIC_GRP_CTRL,
		.attr = KVM_DEV_ARM_VGIC_CTRL_INIT,
	};
	if(ioctl(cd.fd, KVM_SET_DEVICE_ATTR, &ctl) < 0)
		die("KVM_DEV_ARM_VGIC_CTRL_INIT");
}

/* ---- console input ---- */

static void
tty_restore(void)
{
	if(tty_raw_set)
		tcsetattr(STDIN_FILENO, TCSANOW, &tty_orig);
}

static void
on_alarm(int sig)
{
	(void)sig;
}

static void
on_signal(int sig)
{
	(void)sig;
	running = 0;
}

static void
poll_stdin(void)
{
	uint8_t buf[64];
	ssize_t n;

	if(uart.rxhead - uart.rxtail >= sizeof uart.rx)
		return;
	n = read(STDIN_FILENO, buf, sizeof buf);
	if(n <= 0)
		return;
	for(ssize_t i = 0; i < n; i++){
		uint8_t c = buf[i];

		if(c == '\r')
			c = '\n';
		uart.rx[uart.rxtail++ % sizeof uart.rx] = c;
	}
	uart.ris |= 0x10;	/* RX interrupt */
	uart_update_irq();
}

/* ---- main ---- */

int
main(int argc, char **argv)
{
	const char *kernel = NULL;
	const char *cmdline = DEFAULT_CMDLINE;
	const char *dtb_out = NULL;
	uint64_t ram_size;
	size_t ram_mb = DEFAULT_RAM_MB;
	uint8_t *ram, *dtb;
	uint64_t entry, dtb_pa = DTB_PA;
	int kvm, vm, vcpu, run_size;
	struct kvm_run *run;

	for(int i = 1; i < argc; i++){
		if(strcmp(argv[i], "-m") == 0 && i + 1 < argc)
			ram_mb = (size_t)strtoul(argv[++i], NULL, 0);
		else if(strcmp(argv[i], "-c") == 0 && i + 1 < argc)
			cmdline = argv[++i];
		else if(strcmp(argv[i], "-dump-dtb") == 0 && i + 1 < argc)
			dtb_out = argv[++i];
		else if(argv[i][0] != '-' && kernel == NULL)
			kernel = argv[i];
		else {
			fprintf(stderr, "usage: %s [-m MB] [-c cmdline] [-dump-dtb file] <kernel.img|kernel.elf>\n",
			        argv[0]);
			return 2;
		}
	}
	if(kernel == NULL){
		fprintf(stderr, "usage: %s [-m MB] [-c cmdline] <kernel.img|kernel.elf>\n", argv[0]);
		return 2;
	}

	ram_size = (uint64_t)ram_mb << 20;
	if(ram_size < 0x10000000ULL){
		fprintf(stderr, "[9vm-arm64] need at least 256 MB of RAM\n");
		return 2;
	}
	if(dtb_pa + DTB_RESERVE > RAM_BASE + ram_size)
		dtb_pa = ((RAM_BASE + ram_size - DTB_RESERVE) & ~0x1fffffULL);

	ram = mmap(NULL, ram_size, PROT_READ | PROT_WRITE,
	           MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	if(ram == MAP_FAILED)
		die("mmap RAM");
	madvise(ram, ram_size, MADV_HUGEPAGE);

	dtb = ram + (dtb_pa - RAM_BASE);
	build_fdt(dtb, ram_size, cmdline);
	if(dtb_out != NULL){
		int fd = open(dtb_out, O_WRONLY | O_CREAT | O_TRUNC, 0644);

		if(fd >= 0){
			uint32_t total;

			memcpy(&total, dtb + 4, 4);
			total = (uint32_t)((total >> 24) | ((total >> 8) & 0xff00) |
			                   ((total << 8) & 0xff0000) | (total << 24));
			write(fd, dtb, total);
			close(fd);
			fprintf(stderr, "[9vm-arm64] device tree written to %s (%u bytes)\n",
			        dtb_out, total);
		}
	}

	entry = load_image(kernel, ram, ram_size);

	kvm = open("/dev/kvm", O_RDWR | O_CLOEXEC);
	if(kvm < 0){
		perror("/dev/kvm");
		return 1;
	}
	if(ioctl(kvm, KVM_GET_API_VERSION, 0) != KVM_API_VERSION){
		fprintf(stderr, "[9vm-arm64] KVM API mismatch\n");
		return 1;
	}
	vm = ioctl(kvm, KVM_CREATE_VM, 0);
	if(vm < 0)
		die("KVM_CREATE_VM");
	vm_fd = vm;

	struct kvm_userspace_memory_region region = {
		.slot = 0,
		.guest_phys_addr = RAM_BASE,
		.memory_size = ram_size,
		.userspace_addr = (uint64_t)(uintptr_t)ram,
	};
	if(ioctl(vm, KVM_SET_USER_MEMORY_REGION, &region) < 0)
		die("KVM_SET_USER_MEMORY_REGION");

	struct kvm_vcpu_init init;
	memset(&init, 0, sizeof init);
	if(ioctl(vm, KVM_ARM_PREFERRED_TARGET, &init) < 0)
		die("KVM_ARM_PREFERRED_TARGET");
	if(ioctl(kvm, KVM_CHECK_EXTENSION, KVM_CAP_ARM_PSCI_0_2) > 0)
		init.features[0] |= 1u << KVM_ARM_VCPU_PSCI_0_2;

	/* vCPU before the vGIC: the redistributor set is sized from the
	 * created vCPUs, and this kernel answers EBUSY to CREATE_VCPU
	 * once a vGIC has been initialized without one. */
	vcpu = ioctl(vm, KVM_CREATE_VCPU, 0);
	if(vcpu < 0)
		die("KVM_CREATE_VCPU");
	if(ioctl(vcpu, KVM_ARM_VCPU_INIT, &init) < 0)
		die("KVM_ARM_VCPU_INIT");

	setup_vgic(vm);

	run_size = ioctl(kvm, KVM_GET_VCPU_MMAP_SIZE, 0);
	if(run_size <= 0)
		die("KVM_GET_VCPU_MMAP_SIZE");
	run = mmap(NULL, (size_t)run_size, PROT_READ | PROT_WRITE, MAP_SHARED, vcpu, 0);
	if(run == MAP_FAILED)
		die("mmap kvm_run");

	set_one_reg(vcpu,
	            KVM_REG_ARM64 | KVM_REG_SIZE_U64 | KVM_REG_ARM_CORE |
	                KVM_REG_ARM_CORE_REG(regs.pc), entry);
	set_one_reg(vcpu,
	            KVM_REG_ARM64 | KVM_REG_SIZE_U64 | KVM_REG_ARM_CORE |
	                KVM_REG_ARM_CORE_REG(regs.regs[0]), dtb_pa);
	set_one_reg(vcpu,
	            KVM_REG_ARM64 | KVM_REG_SIZE_U64 | KVM_REG_ARM_CORE |
	                KVM_REG_ARM_CORE_REG(regs.pstate), 0x3c5);

	fprintf(stderr, "[9vm-arm64] pc=0x%llx dtb=0x%llx ram=%llu MB\n",
	        (unsigned long long)entry, (unsigned long long)dtb_pa,
	        (unsigned long long)ram_mb);

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGALRM, on_alarm);
	if(isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &tty_orig) == 0){
		struct termios raw = tty_orig;

		cfmakeraw(&raw);
		tcsetattr(STDIN_FILENO, TCSANOW, &raw);
		tty_raw_set = 1;
		atexit(tty_restore);
	}
	{
		int fl = fcntl(STDIN_FILENO, F_GETFL, 0);

		if(fl >= 0)
			fcntl(STDIN_FILENO, F_SETFL, fl | O_NONBLOCK);
	}
	struct itimerval it = { { 0, 20000 }, { 0, 20000 } };	/* 20 ms */

	setitimer(ITIMER_REAL, &it, NULL);

	while(running){
		if(ioctl(vcpu, KVM_RUN, 0) < 0){
			if(errno == EINTR){
				poll_stdin();
				continue;
			}
			die("KVM_RUN");
		}

		switch(run->exit_reason){
		case KVM_EXIT_MMIO: {
			uint64_t pa = run->mmio.phys_addr;
			uint32_t len = run->mmio.len;

			if(pa >= UART_BASE && pa < UART_BASE + UART_SIZE){
				uint32_t off = (uint32_t)(pa - UART_BASE);
				uint32_t v = 0;

				if(run->mmio.is_write){
					for(uint32_t i = 0; i < len; i++)
						v |= (uint32_t)run->mmio.data[i] << (8 * i);
					uart_write(off, v);
				} else {
					v = uart_read(off);
					for(uint32_t i = 0; i < len; i++)
						run->mmio.data[i] = (uint8_t)(v >> (8 * i));
				}
				break;
			}
			if(!run->mmio.is_write)
				memset(run->mmio.data, 0, len);
			else
				fprintf(stderr, "\n[9vm-arm64] unhandled MMIO write 0x%llx len %u\n",
				        (unsigned long long)pa, len);
			break;
		}
		case KVM_EXIT_SYSTEM_EVENT:
			fprintf(stderr, "\n[9vm-arm64] guest shutdown (type %u)\n",
			        run->system_event.type);
			running = 0;
			break;
#ifdef KVM_EXIT_ARM_NISV
		case KVM_EXIT_ARM_NISV:
			fprintf(stderr, "\n[9vm-arm64] unhandled guest fault at 0x%llx esr 0x%llx\n",
			        (unsigned long long)run->arm_nisv.fault_ipa,
			        (unsigned long long)run->arm_nisv.esr_iss);
			return 1;
#endif
		case KVM_EXIT_FAIL_ENTRY:
			fprintf(stderr, "\n[9vm-arm64] hardware entry failure 0x%llx\n",
			        (unsigned long long)run->fail_entry.hardware_entry_failure_reason);
			return 1;
		case KVM_EXIT_INTERNAL_ERROR:
			fprintf(stderr, "\n[9vm-arm64] internal error %u\n", run->internal.suberror);
			return 1;
		case KVM_EXIT_SHUTDOWN:
			fprintf(stderr, "\n[9vm-arm64] guest reset/shutdown\n");
			running = 0;
			break;
		default:
			fprintf(stderr, "\n[9vm-arm64] exit reason %u\n", run->exit_reason);
			return 1;
		}
	}
	return 0;
}
