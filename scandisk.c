#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>

#include "bootsect.h"
#include "bpb.h"
#include "direntry.h"
#include "fat.h"
#include "dos.h"

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
	printf("%d\n", is_end_of_file(cluster));
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


int check_file_size(struct direntry * dirent, uint8_t * image_buf, struct bpb33* bpb){
	uint16_t followclust=0;
    uint32_t size;
    uint32_t observed_size=(uint32_t) bpb->bpbBytesPerSec * bpb->bpbSecPerClust;
    uint32_t cluster_size=(uint32_t) bpb->bpbBytesPerSec * bpb->bpbSecPerClust;
    uint16_t cluster;
    uint16_t end_cluster;
    uint16_t start_free_cluster;
    uint16_t file_cluster;
    
    if (dirent->deName[0] == SLOT_EMPTY)
    {
		/* if we found an empty slot--> return 0, there's nothing left to check*/
	return followclust;
    }

    if ((dirent->deName[0]) == SLOT_DELETED)
    /* if we found a deleted slot--> return 0, there's nothing left to check*/
    {
	return followclust;
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
		return followclust;
    }
    else if ((dirent->deAttributes & ATTR_VOLUME) != 0) 
    {//ignore volume
		return followclust;
    } 
    else {
		/* if the dirent is either a file or a director--> check the size*/
		size=getulong(dirent->deFileSize);
		cluster=getushort(dirent->deStartCluster);
		cluster=get_fat_entry(cluster,image_buf, bpb);
		while (!is_end_of_file(cluster)){
			/*When the cluster is not in the EOF range.
			 * This is to find the size in the FAT chain
			 * */
			 //change to: Check if it is a bad or free cluster?-> yes-> 
			 if(is_valid_cluster(cluster,bpb)){
				observed_size+=cluster_size; 
				if ((0<=(int) (observed_size-size)) && (int)(observed_size-size)<cluster_size){
					/*
					 * This is the last cluster in the chain*/
					end_cluster=cluster;
					start_free_cluster=get_fat_entry(cluster,image_buf,bpb);
			}
				cluster=get_fat_entry(cluster, image_buf,bpb);
			}
			//else{//really have to be else
				//break;
			//}
		}//end while loop
		if (size<(observed_size-cluster_size)||size>observed_size){
			/*
			 * Wrong size in the dirent(different from the FAT chain)--> 
			 * 1, print the wrong size
			 * 2, fix
			 */
			print_wrong_dirent(dirent,observed_size);
			if (size>observed_size){
				putulong(dirent->deFileSize,observed_size);
				printf("changed the file size back to %d\n",(uint)observed_size);
			}
			else{
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
void follow_dir_scan(uint16_t cluster, uint8_t * image_buf, struct bpb33* bpb){
	while (is_valid_cluster(cluster,bpb)){
		struct direntry *dirent = (struct direntry*)cluster_to_addr(cluster, image_buf, bpb);

        int numDirEntries = (bpb->bpbBytesPerSec * bpb->bpbSecPerClust) / sizeof(struct direntry);
        int i = 0;
		for ( ; i < numDirEntries; i++){
			uint16_t followclust = check_file_size(dirent,image_buf, bpb);
            if (followclust)
                follow_dir_scan(followclust, image_buf, bpb);
            dirent++;
	}
	cluster = get_fat_entry(cluster, image_buf, bpb);
	}
}

void traverse_root_scan(uint8_t* image_buf, struct bpb33* bpb){
	uint16_t cluster = 0;

    struct direntry *dirent = (struct direntry*)cluster_to_addr(cluster, image_buf, bpb);

    int i = 0;
    for ( ; i < bpb->bpbRootDirEnts; i++)
    {
        uint16_t followclust = check_file_size(dirent, image_buf, bpb);
        if (is_valid_cluster(followclust, bpb))
            follow_dir_scan(followclust, image_buf, bpb);
        dirent++;
    }
}
void usage(char *progname) {
    fprintf(stderr, "usage: %s <imagename>\n", progname);
    exit(1);
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
    //#define CLUSTER_SIZE bpb->bpbBytesPerSec * bpb->bpbSecPerClust;
    // your code should start here...
	traverse_root_scan(image_buf,bpb);
    unmmap_file(image_buf, &fd);
    return 0;
}
