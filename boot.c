/* boot.c  -  Boot image composition */

/* Copyright 1992-1997 Werner Almesberger. See file COPYING for details. */


#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <a.out.h>
#include <sys/stat.h>

#include "config.h"
#include "common.h"
#include "lilo.h"
#include "geometry.h"
#include "cfg.h"
#include "map.h"
#include "partition.h"
#include "boot.h"
#include <asm/page.h>


static GEOMETRY geo;
static struct stat st;


static void check_size(char *name,int setup_secs,int sectors)
{
    if (sectors > setup_secs+MAX_KERNEL_SECS)
	die("Kernel %s is too big",name);
}


void boot_image(char *spec,IMAGE_DESCR *descr)
{
    BOOT_SECTOR buff;
    SETUP_HDR hdr;
    char *initrd;
    int setup,fd,sectors;
    int modern_kernel;

    if (verbose > 0) printf("Boot image: %s\n",spec);
    fd = geo_open(&geo,spec,O_RDONLY);
    if (fstat(fd,&st) < 0) die("fstat %s: %s",spec,strerror(errno));
    if (read(fd,(char *) &buff,SECTOR_SIZE) != SECTOR_SIZE)
	die("read %s: %s",spec,strerror(errno));
#ifdef LCF_VARSETUP
    setup = buff.sector[VSS_NUM] ? buff.sector[VSS_NUM] : SETUPSECS;
#else
    setup = SETUPSECS;
#endif
    if (read(fd,(char *) &hdr,sizeof(hdr)) != sizeof(hdr))
	die("read %s: %s",spec,strerror(errno));
    modern_kernel = !strncmp(hdr.signature,NEW_HDR_SIG,4) && hdr.version >=
      NEW_HDR_VERSION;
    if (modern_kernel) descr->flags |= FLAG_MODKRN;
    if (verbose > 1)
	printf("Setup length is %d sector%s.\n",setup,setup == 1 ? "" : "s");
    map_add(&geo,0,(st.st_size+SECTOR_SIZE-1)/SECTOR_SIZE);
    sectors = map_end_section(&descr->start,setup+SPECIAL_SECTORS);
    if (!modern_kernel || !(hdr.flags & LFLAG_HIGH))
	check_size(spec,setup,sectors);
    else {
	if (hdr.start % PAGE_SIZE)
	    die("Can't load kernel at mis-aligned address 0x%08lx\n",hdr.start);
	descr->start_page = hdr.start/PAGE_SIZE; /* load kernel high */
    }
    geo_close(&geo);
    if (verbose > 1)
	printf("Mapped %d sector%s.\n",sectors,sectors == 1 ? "" : "s");
    if ((initrd = cfg_get_strg(cf_kernel,"initrd")) || (initrd = cfg_get_strg(
      cf_options,"initrd"))) {
	if (!modern_kernel) die("Kernel doesn't support initial RAM disks");
	if (verbose > 0) printf("Mapping RAM disk %s\n",initrd);
	fd = geo_open(&geo,initrd,O_RDONLY);
	if (fstat(fd,&st) < 0) die("fstat %s: %s",initrd,strerror(errno));
	*(unsigned long *) descr->rd_size = st.st_size;
	map_begin_section();
	map_add(&geo,0,(st.st_size+SECTOR_SIZE-1)/SECTOR_SIZE);
	sectors = map_end_section(&descr->initrd,0);
	if (verbose > 1)
	    printf("RAM disk: %d sector%s.\n",sectors,sectors == 1 ?  "" :
	      "s");
	geo_close(&geo);
    }
}


void boot_device(char *spec,char *range,IMAGE_DESCR *descr)
{
    char *here;
    int start,secs;
    int sectors;

    if (verbose > 0) printf("Boot device: %s, range %s\n",spec,range);
    (void) geo_open(&geo,spec,O_NOACCESS);
    here = strchr(range,'-');
    if (here) {
	*here++ = 0;
	start = to_number(range);
	if ((secs = to_number(here)-start+1) < 0) die("Invalid range");
    }
    else {
	here = strchr(range,'+');
	if (here) {
	    *here++ = 0;
	    start = to_number(range);
	    secs = to_number(here);
	}
	else {
	    start = to_number(range);
	    secs = 1;
	}
    }
    map_add(&geo,start,secs);
    check_size(spec,SETUPSECS,sectors = map_end_section(&descr->start,60));
				/* this is a crude hack ... ----------^^*/
    geo_close(&geo);
    if (verbose > 1)
	printf("Mapped %d sector%s.\n",sectors,sectors == 1 ? "" : "s");
}


void do_map_drive(void)
{
    const char *tmp;
    char *end;
    int from,to;

    tmp = cfg_get_strg(cf_other,"map-drive");
    from = strtoul(tmp,&end,0);
    if (from > 0xff || *end)
	cfg_error("Invalid drive specification \"%s\"",tmp);
    cfg_init(cf_map_drive);
    (void) cfg_parse(cf_map_drive);
    tmp = cfg_get_strg(cf_map_drive,"to");
    if (!tmp) cfg_error("TO is required");
    to = strtoul(tmp,&end,0);
    if (to > 0xff || *end)
	cfg_error("Invalid drive specification \"%s\"",tmp);
    if (from || to) { /* 0 -> 0 is special */
	int i;

	for (i = 0; i < curr_drv_map; i++) {
	    if (drv_map[i] == ((to << 8) | from))
		die("Mapping 0x%02x to 0x%02x already exists",from,to);
	    if ((drv_map[i] & 0xff) == from)
		die("Ambiguous mapping 0x%02x to 0x%02x or 0x%02x",from,
		  drv_map[i] >> 8,to);
	}
	if (curr_drv_map == DRVMAP_SIZE)
	    cfg_error("Too many drive mappings (more than %d)",DRVMAP_SIZE);
	if (verbose > 3)
	    printf("  Mapping BIOS drive 0x%02x to 0x%02x\n",from,to);
	drv_map[curr_drv_map++] = (to << 8) | from;
    }
    cfg_unset(cf_other,"map-drive");
}


#define PART(s,n) (((struct partition *) (s)[0].par_c.ptable)[(n)])


void boot_other(char *loader,char *boot,char *part,IMAGE_DESCR *descr)
{
    int b_fd,l_fd,p_fd,walk,found,size;
    unsigned short magic;
    BOOT_SECTOR buff[SETUPSECS-1];
    struct stat st;
    char *pos;
    char npart[10];
    int i;

    if (!loader) loader = DFL_CHAIN;
    if (!part) {   /*  "/dev/hdXN and /dev/sdXN have default partition tables -- JRC */
        if (strlen(boot)==9 && 
       	  (strncmp(boot,"/dev/hd",7)==0 || strncmp(boot,"/dev/sd",7)==0) &&
       	  boot[8]>='1' && boot[8]<='4') {
	    part = strcpy(npart, boot);
	    part[8] = 0;
	}
    }
    if (verbose > 0)
	printf("Boot other: %s%s%s, loader %s\n",boot,part ? ", on " : "",part
	  ? part : "",loader);
    if (cfg_get_flag(cf_other,"unsafe")) {
	(void) geo_open_boot(&geo,boot);
	if (part) die("TABLE and UNSAFE are mutually incompatible.");
    }
    else {
	b_fd = geo_open(&geo,boot,O_RDONLY);
	if (fstat(b_fd,&st) < 0)
	    die("fstat %s: %s",boot,strerror(errno));
	if (!geo.file) part_verify(st.st_rdev,0);
	if (lseek(b_fd,(long) BOOT_SIG_OFFSET,0) < 0)
	    die("lseek %s: %s",boot,strerror(errno));
	if ((size = read(b_fd,(char *) &magic,2)) != 2)
	    if (size < 0) die("read %s: %s",boot,strerror(errno));
	    else die("Can't get magic number of %s",boot);
	if (magic != BOOT_SIGNATURE)
	    die("First sector of %s doesn't have a valid boot signature",boot);
    }
    memset(buff,0,sizeof(buff));
    if ((l_fd = open(loader,O_RDONLY)) < 0)
	die("open %s: %s",loader,strerror(errno));
    if ((size = read(l_fd,buff,sizeof(buff)+1)) < 0)
	die("read %s: %s",loader,strerror(errno));
    check_version(buff,STAGE_CHAIN);
    if (size > sizeof(buff))
	die("Chain loader %s is too big",loader);
    if (!part) {
        p_fd = -1; /* pacify GCC */
        PART(buff,0).boot_ind = geo.device;
        PART(buff,0).start_sect = geo.start;     /* pseudo partition table */
    }
    else {
	if ((p_fd = open(part,O_RDONLY)) < 0)
	    die("open %s: %s",part,strerror(errno));
	if (lseek(p_fd,(long) PART_TABLE_OFFSET,0) < 0)
	    die("lseek %s: %s",part,strerror(errno));
	if (read(p_fd,(char *) buff[0].par_c.ptable,PART_TABLE_SIZE) !=
	  PART_TABLE_SIZE)
	    die("read %s: %s",part,strerror(errno));
	found = 0;
	for (walk = 0; walk < PARTITION_ENTRIES; walk++)
	    if (!PART(buff,walk).sys_ind || PART(buff,walk).start_sect !=
	      geo.start) {
		/*
		 * Don't remember what this is supposed to be good for :-(
		 */
		if (PART(buff,walk).sys_ind != PART_DOS12 && PART(buff,walk).
		  sys_ind != PART_DOS16_SMALL && PART(buff,walk).sys_ind !=
		  PART_DOS16_BIG)
		  PART(buff,walk).sys_ind = PART_INVALID;
	    }
	    else {
		if (found) die("Duplicate entry in partition table");
		buff[0].par_c.offset = walk*PARTITION_ENTRY;
		PART(buff,walk).boot_ind = 0x80;
		found = 1;
	    }
	if (!found) die("Partition entry not found.");
	(void) close(p_fd);
    }
    (void) close(l_fd);
    buff[0].par_c.drive = geo.device;
    drv_map[curr_drv_map] = 0;
    prt_map[curr_prt_map] = 0;
    pos = (char *) buff+buff[0].par_c.drvmap;
    memcpy(pos,drv_map,sizeof(drv_map));
    memcpy(pos+sizeof(drv_map),prt_map,sizeof(prt_map)-2);
    map_add_zero();
    for (i = 0; i < SETUPSECS-1; i++) map_add_sector(&buff[i]);
    map_add(&geo,0,1);
    (void) map_end_section(&descr->start,SETUPSECS+SPECIAL_SECTORS);
	/* size is known */
    geo_close(&geo);
    if (verbose > 1)
	printf("Mapped %d (%d+1+1) sectors.\n",SETUPSECS+2,SETUPSECS);
}
