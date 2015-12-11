#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <ctype.h>

#include "bootsect.h"
#include "bpb.h"
#include "direntry.h"
#include "fat.h"
#include "dos.h"

#define BAD 1
#define NOT_BAD 0
#define ORPHAN 1
#define NO_ORPHAN 0
#define START 0
#define NO_START 1
void itoa(int i, char *b){
    char const digit[] = "0123456789";
    char* p = b;
    if(i<0){
        *p++ = '-';
        i *= -1;
    }
    int shifter = i;
    do{ //Move to where representation ends
        ++p;
        shifter = shifter/10;
    }while(shifter);
    *p = '\0';
    do{ //Move back, inserting digits as u go
        *--p = digit[i%10];
        i = i/10;
    }while(i);
    //return b;
}
void print_wrong_dirent(struct direntry* dirent, uint32_t observed_size){
	int i;
    char name[9];
    char extension[4];
    name[8] = ' ';
    extension[3] = ' ';
    memcpy(name, &(dirent->deName[0]), 8);
    memcpy(extension, dirent->deExtension, 3);
    /* names are space padded - remove the spaces */
    for (i = 8; i > 0; i--) 
    {
	if (name[i] == ' ') 
	    name[i] = '\0';
	else 
	    break;
    }

    /* remove the spaces from extensions */
    for (i = 3; i > 0; i--) 
    {
	if (extension[i] == ' ') 
	    extension[i] = '\0';
	else 
	    break;
    }
    
    if ((dirent->deAttributes & ATTR_DIRECTORY) != 0){ //if it is a directory
        // don't deal with hidden directories; MacOS makes these
        // for trash directories and such; just ignore them.
		if ((dirent->deAttributes & ATTR_HIDDEN) != ATTR_HIDDEN)
			{
				printf("%s/ (directory): deFileSize: %u. FATSize: %u\n", name,
				getulong(dirent->deFileSize), observed_size);
			}
	}
    else {
        /*
         * a "regular" file entry
         */
	printf("%s.%s (starting cluster %d) . deFileSize: %u. FATSize: %u\n", 
	       name, extension, getushort(dirent->deStartCluster), getulong(dirent->deFileSize), observed_size);
    }
}

void free_redundant_clusters(uint16_t start_cluster, uint8_t * image_buf, struct bpb33 * bpb){
	printf("Start free_redundant_clusters\n");
	uint32_t cluster_size=(uint32_t) bpb->bpbBytesPerSec * bpb->bpbSecPerClust;
	uint16_t cluster=start_cluster;
	uint16_t end_cluster;
	while(!is_end_of_file(cluster)){
		/*
		 * set the content of the cluster to blank (0)
		 * */
		uint8_t * address=cluster_to_addr(cluster, image_buf,bpb);
		memset(address,0,cluster_size);
		printf("Freed cluster %d. Done!\n",(uint)cluster);
		/*
		 * mark the FAT entry as free after freeing the cluster*/
		end_cluster=cluster;
		cluster=get_fat_entry(cluster, image_buf, bpb);
		set_fat_entry(end_cluster, CLUST_FREE,image_buf,bpb);
		printf("Set cluster %d as free. Done!\n", (uint)end_cluster);
	}
}


int check_file_size(struct direntry * dirent, uint8_t * image_buf, struct bpb33* bpb, int * orphan_list){
	uint16_t followclust=0;
    uint32_t size;
    uint32_t observed_size=0;
    uint32_t cluster_size=(uint32_t) bpb->bpbBytesPerSec * bpb->bpbSecPerClust;
    uint16_t cluster;
    uint16_t end_cluster;
    uint16_t start_free_cluster;
    uint16_t file_cluster;
    uint16_t prev_cluster;
    int bad_detected=NOT_BAD;
    
    if (dirent->deName[0] == SLOT_EMPTY)
    {
		/* if we found an empty slot--> return 0, there's nothing left to check*/
	return 0;
    }

    if ((dirent->deName[0]) == SLOT_DELETED)
    /* if we found a deleted slot--> return 0, there's nothing left to check*/
    {
	return 0;
    }

    if ((dirent->deName[0]) == 0x2E)
    {
	// dot entry ("." or "..")
	// skip it, because it may have been checked somewhere else
        return followclust;
    }

    if ((dirent->deAttributes & ATTR_WIN95LFN) == ATTR_WIN95LFN)//// ASK PROFESSOR: why do I have to mask? 
    {
	// ignore any long file name extension entries
		return 0;
    }
    else if ((dirent->deAttributes & ATTR_VOLUME) != 0) 
    {//ignore volume
		return 0;
    } 
    else {
		/* if the dirent is either a file or a director--> check the size*/
		size=getulong(dirent->deFileSize);
		cluster=getushort(dirent->deStartCluster);
		orphan_list[cluster]=NO_ORPHAN;
		while (!is_end_of_file(cluster)){
			/*When the cluster is not in the EOF range.
			 * This is to find the size in the FAT chain
			 * */
			 if(is_valid_cluster(cluster,bpb)){
				observed_size+=cluster_size; 
				orphan_list[cluster]=NO_ORPHAN;
				if ((0<=(int) (observed_size-size)) && (int)(observed_size-size)<cluster_size){
					/*
					 * This is the last cluster in the chain*/
					end_cluster=cluster;
					start_free_cluster=get_fat_entry(cluster,image_buf,bpb);
				}
			}
			if (is_bad(cluster)){
					bad_detected=BAD;
					set_fat_entry(prev_cluster, FAT12_MASK & CLUST_EOFS,image_buf, bpb);
					if ((observed_size-size)<cluster_size){
						putulong(dirent->deFileSize,observed_size);
					}
					printf("BAD entry detected. Previous cluster is: %d\n",(uint)prev_cluster);
					break;
			}
			prev_cluster=cluster;
			cluster=get_fat_entry(cluster,image_buf, bpb);
		}//end while loop
		
		if (size<(observed_size-cluster_size)||size>observed_size){
			/*
			 * Wrong size in the dirent(different from the FAT chain)--> 
			 * 1, print the wrong size
			 * 2, fix
			 */
			 printf("Start to print wrong ");
			print_wrong_dirent(dirent,observed_size);
			if (size>observed_size){
				putulong(dirent->deFileSize,observed_size);
				printf("changed the file size back to %d\n",(uint)observed_size);
			}
			else{
				print_wrong_dirent(dirent,observed_size);
				set_fat_entry(end_cluster,(uint16_t)(CLUST_EOFS),image_buf,bpb);
				free_redundant_clusters(start_free_cluster,image_buf,bpb);
			}
		}
		
		if ((dirent->deAttributes & ATTR_DIRECTORY)!=0){
			/*
			 * If it is a directory-> change followclust as a return value to later follow the directory
			 * */
			if ((dirent->deAttributes & ATTR_HIDDEN) != ATTR_HIDDEN){	//ignore hidden directory
				file_cluster = getushort(dirent->deStartCluster);
				followclust = file_cluster;
			}
		}
	}

    return followclust;
}

/* write the values into a directory entry */
void write_dirent(struct direntry *dirent, char *filename, 
		  uint16_t start_cluster, uint32_t size)
{
    char *p, *p2;
    char *uppername;
    int len, i;

    /* clean out anything old that used to be here */
    memset(dirent, 0, sizeof(struct direntry));

    /* extract just the filename part */
    uppername = strdup(filename);
    p2 = uppername;
    for (i = 0; i < strlen(filename); i++) 
    {
	if (p2[i] == '/' || p2[i] == '\\') 
	{
	    uppername = p2+i+1;
	}
    }

    /* convert filename to upper case */
    for (i = 0; i < strlen(uppername); i++) 
    {
	uppername[i] = toupper(uppername[i]);
    }

    /* set the file name and extension */
    memset(dirent->deName, ' ', 8);
    p = strchr(uppername, '.');
    memcpy(dirent->deExtension, "___", 3);
    if (p == NULL) 
    {
	fprintf(stderr, "No filename extension given - defaulting to .___\n");
    }
    else 
    {
	*p = '\0';
	p++;
	len = strlen(p);
	if (len > 3) len = 3;
	memcpy(dirent->deExtension, p, len);
    }

    if (strlen(uppername)>8) 
    {
	uppername[8]='\0';
    }
    memcpy(dirent->deName, uppername, strlen(uppername));
    free(p2);

    /* set the attributes and file size */
    dirent->deAttributes = ATTR_NORMAL;
    putushort(dirent->deStartCluster, start_cluster);
    putulong(dirent->deFileSize, size);

    /* could also set time and date here if we really
       cared... */
}


/* create_dirent finds a free slot in the directory, and write the
   directory entry */

void create_dirent(struct direntry *dirent, char *filename, 
		   uint16_t start_cluster, uint32_t size,
		   uint8_t *image_buf, struct bpb33* bpb)
{
    while (1) 
    {
	if (dirent->deName[0] == SLOT_EMPTY) 
	{
	    /* we found an empty slot at the end of the directory */
	    write_dirent(dirent, filename, start_cluster, size);
	    dirent++;

	    /* make sure the next dirent is set to be empty, just in
	       case it wasn't before */
	    memset((uint8_t*)dirent, 0, sizeof(struct direntry));
	    dirent->deName[0] = SLOT_EMPTY;
	    return;
	}

	if (dirent->deName[0] == SLOT_DELETED) 
	{
	    /* we found a deleted entry - we can just overwrite it */
	    write_dirent(dirent, filename, start_cluster, size);
	    return;
	}
	dirent++;
    }
}

void follow_dir_scan(uint16_t cluster, uint8_t * image_buf, struct bpb33* bpb, int * orphan_list){
	while (is_valid_cluster(cluster,bpb)){
		struct direntry *dirent = (struct direntry*)cluster_to_addr(cluster, image_buf, bpb);

        int numDirEntries = (bpb->bpbBytesPerSec * bpb->bpbSecPerClust) / sizeof(struct direntry);
        int i = 0;
		for ( ; i < numDirEntries; i++){
			uint16_t followclust = check_file_size(dirent,image_buf, bpb,orphan_list);
            if (followclust)
                follow_dir_scan(followclust, image_buf, bpb,orphan_list);
            dirent++;
	}
	cluster = get_fat_entry(cluster, image_buf, bpb);
	}
}

void traverse_root_scan(uint8_t* image_buf, struct bpb33* bpb, int * orphan_list){
	uint16_t cluster = 0;

    struct direntry *dirent = (struct direntry*)cluster_to_addr(cluster, image_buf, bpb);

    int i = 0;
    for ( ; i < bpb->bpbRootDirEnts; i++)
    {
        uint16_t followclust = check_file_size(dirent, image_buf, bpb, orphan_list);
        if (is_valid_cluster(followclust, bpb))
            follow_dir_scan(followclust, image_buf, bpb, orphan_list);
        dirent++;
    }
}
void usage(char *progname) {
    fprintf(stderr, "usage: %s <imagename>\n", progname);
    exit(1);
}

void check_start_cluster(int * start_list,int *len_list, uint8_t * image_buf, struct bpb33* bpb ){
	uint16_t not_start;
	for (int i=CLUST_FIRST;i<*len_list;i++){
		not_start=get_fat_entry((uint16_t)i,image_buf,bpb);
		if (is_valid_cluster(not_start,bpb)){
			start_list[(uint)not_start]=NO_START;
		}
	}
}

void collect_orphan(int *start_list, int *orphan_list, int * len_list, uint8_t *image_buf, struct bpb33 * bpb){
	uint32_t cluster_size=(uint32_t) bpb->bpbBytesPerSec * bpb->bpbSecPerClust;
	int size=0;
	uint16_t cluster;
	int count_orphan=0;
	for (int i=CLUST_FIRST;i<*len_list;i++){
		size=0;
		if (orphan_list[i]==ORPHAN && start_list[i]==START && is_start((uint16_t)i,image_buf,bpb)){
			count_orphan++;
			cluster=(uint16_t)i;
			while (!is_end_of_file(cluster)){
				if (is_valid_cluster(cluster,bpb)){
					size+=cluster_size;
					cluster=get_fat_entry(cluster,image_buf,bpb);
				}
				else{
					break;
				}
			}//end while loop
			struct direntry *root_dirent=(struct direntry *)cluster_to_addr(0,image_buf,bpb);
			//char filename[13];
			//char num [3];
			//itoa(count_orphan,num);
			//strcat(filename,"found");
			//strcat(filename, num);
			//strcat(filename, ".dat");
			//strcat(filename,"/0");
			create_dirent(root_dirent,"found.dat", (uint16_t)i, (uint32_t) size,
		   image_buf,bpb);
		   printf("Created file: Found.dat. File size: %d. Start Cluster: %d\n", (uint)size, (uint)i );
		}
	}
}

int main(int argc, char** argv) {
    uint8_t *image_buf;
    int fd;
    struct bpb33* bpb;
    if (argc < 2) {
	usage(argv[0]);
    }
    image_buf = mmap_file(argv[1], &fd);
    bpb = check_bootsector(image_buf);
    int entries= bpb->bpbSectors/bpb->bpbSecPerClust;
    int start_list [entries+1];
	int orphan_list [entries+1];
	int len_list=entries+1;
	for (int i=0;i<entries+1;i++){
		start_list[i]=START;
		orphan_list[i]=ORPHAN;
	}
	//printf("BEFORE CHECKING START CLUSTER\n");
	check_start_cluster(start_list,&len_list,image_buf,bpb);
	//printf("DONE CHECKING START CLUSTER\n");
	traverse_root_scan(image_buf,bpb,orphan_list);
	//printf("DONE TRAVERSING ROOT\n");
	collect_orphan(start_list,orphan_list,&len_list,image_buf,bpb);
	//printf("DONE COLLECTING ORPHAN\n");
    unmmap_file(image_buf, &fd);
    return 0;
}
