#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <asm/rsi_cmds.h>
#include <linux/io.h>
#include <linux/cdev.h>
#include <linux/ioctl.h>
//IMG_IOCTL_GET_RD_IPA
struct rd_ipa_size_data {
	unsigned long rd_addr;
	unsigned long ipa_start;
	unsigned long ipa_size;
};
//IMG_IOCTL_MAP_IPA
struct map_ipa_data {
	unsigned long guest_rd_addr;
	unsigned long guest_ipa;
	unsigned long map_size;
};
struct load_file_data {
	char file_path[256];
	unsigned long file_size;
};
struct write_file_data {
	char file_path[256];
	unsigned long file_size;
};
int get_rd_addr(unsigned long *rd_addr);
int map_mem(unsigned long guest_rd_addr, unsigned long guest_ipa,
	    unsigned long map_size);
static int reserved_init(void);
void __maybe_unused get_ripas_from_range(unsigned long base, unsigned long end);
int load_file(struct load_file_data *load_file_request);
int write_file(struct write_file_data *write_file_request);