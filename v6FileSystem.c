#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>

//superblock struct
typedef struct {
    unsigned int isize;
    unsigned int fsize;
    unsigned int nfree;
    unsigned int free[251];
    char flock;
    char ilock;
    char fmod;
    unsigned int time;
} superblock_type;

//Inode Struct
typedef struct {
    unsigned short flags;
    unsigned short nlinks;
    unsigned int uid;
    unsigned int gid;
    unsigned int size0;
    unsigned int size1;
    unsigned int addr[9];
    unsigned int actime;
    unsigned int modtime;
} inode_type;

typedef struct {
    unsigned int inode;
    char filename[28];
} dir_type;

//int chain[256];
int zeros[256];

int fd;
superblock_type superBlock;
inode_type root_inode;
int total_num_inodes;
int addr_dir;
int addr_inode;
char last_dir[32];

int curr_inode = -1;

//Defining size of inode and block as const for better readibility
const int INODESIZE = 64;
const int BLOCKSIZE = 1024;

//This method will write a block to FileSystem
void writeBlockToFS(int bNumber,void *input, int num_bytes){
    lseek(fd,BLOCKSIZE * bNumber,SEEK_SET);
    write(fd,&input,num_bytes);
}

//This methof will write Inode to file system
void writeInodeToFS(int iNumber,void * input, int num_bytes){
    lseek(fd,((iNumber-1)*INODESIZE)+(2*BLOCKSIZE),SEEK_SET);
    write(fd,&input,num_bytes);
}

//doing a linear search on all the available inodes to check if it's unallocated
int findUnallocatedInode(){
    int total_num_inodes = superBlock.isize*16;

    int i;
    for(i=1;i<=total_num_inodes;i++){
        lseek(fd,2048+(64*(i-1)),SEEK_SET);
        inode_type temp_inode;
        read(fd,&temp_inode,sizeof(temp_inode));
        int if_unallocated = temp_inode.flags & 1<<15;
        if(if_unallocated == 0){
            printf("%d allocated as free inode\n",i);
            return i;
        }
    }
    printf("No free inode to allocate\n");
    return -1;
}

//utiliity function to allocate a free block to a directory
int allocateFreeBlockToDir(int blockNumber, int parentInode,int firstBlock,int free_inode){
    int newDataBlock;

    //either get a free block or use the parameter passed
    if(blockNumber == -1)
        newDataBlock = getFreeBlock();
    else
        newDataBlock = blockNumber;

    if(newDataBlock == -1)
        return -1;

    int i = 0;

    //allocating . and .. only for addr[0]
    if(firstBlock == 1){
        dir_type directory[2];

        directory[0].inode = free_inode;
        directory[1].inode = parentInode;

        directory[0].filename[0] = '.';

        directory[1].filename[0] = '.';
        directory[1].filename[1] = '.';

        i = 2;

        lseek(fd,newDataBlock*1024,SEEK_SET);
        write(fd,&directory,sizeof(directory));
    }

    //writing default directory entries for all 32 bytes in a block dedicated for directory
    for(;i<32;i++){
        lseek(fd,newDataBlock*1024 + (32*i),SEEK_SET);
        dir_type temp_directory;
        temp_directory.inode = -1;
        memset(temp_directory.filename,'\0',sizeof(temp_directory.filename));
        write(fd,&temp_directory,sizeof(temp_directory));
    }

    return newDataBlock; //return the block number to attach it to addr of parent inode
}

//allocating a new inode to directory
int allocateNewInodeToDir(int inode_num, int parentInode){
    int free_inode;

    if(inode_num == -1)
        free_inode = findUnallocatedInode();
    else
        free_inode = inode_num;

    //read the free inode to change values
    lseek(fd,2048+(64*(free_inode-1)),SEEK_SET);
    inode_type newInode;
    read(fd,&newInode,sizeof(newInode));
    
    //since its a new inode dedicated to directory only one data block is sufficient
    int newDataBlock = allocateFreeBlockToDir(-1,parentInode,1,free_inode);

    if(newDataBlock == -1)
        return -1;
    
    //writing all the default values of inode

    //flags - 1(allocated)10(directory)00(small file)1(uid)1(gid)111(rwx for owner)101(rx for group)100(read for everyone)
    newInode.flags = root_inode.flags | 51180;
    newInode.size0 = 0;
    newInode.size1 = 2*32; //size of two directories i.e. . and ..
    newInode.nlinks = 1;
    newInode.uid = 0;
    newInode.gid = 0;
    newInode.addr[0] = newDataBlock; //assigning the new data block to addr[0]
    newInode.actime = (int)time(NULL);
    newInode.modtime = (int)time(NULL); //unix epoch time

    //writing inode to the filesystem
    lseek(fd,2048+(64*(free_inode-1)),SEEK_SET);
    write(fd,&newInode,sizeof(newInode));

    return free_inode;
}

void openfs(char* fileName){
    
    //Opening a file with read write persmission and creating in case it is absent
    fd = open(fileName, O_CREAT | O_RDWR, 0644);
    lseek(fd,BLOCKSIZE,SEEK_SET);

    printf("File %s opened with permission O_CREAT, O_RDWR\n",fileName);

    //Checking if the file is already present with super block and inode written
    if(access(fileName,F_OK) == 0){
        struct stat st;
        stat(fileName, &st);
        if(st.st_size >= (2*BLOCKSIZE+INODESIZE)){
            printf("File %s already exists, reading super block and root inode\n",fileName);
            read(fd,&superBlock,BLOCKSIZE);
            read(fd,&root_inode,sizeof(root_inode));
        }
    }
}

//Adding a free block by writing to the filesystem
//modified to handle case when random block is freed at a random and free array is full
void addFreeBlock(int bNumber){

    if(superBlock.nfree == 251){
        int temp_free[252];
        int i;
        //copying 251 and free array into a temp array which we will be writing into bNumber which needs to be freed
        temp_free[0] = 251;
        for(i=1;i<252;i++){
            temp_free[i] = superBlock.free[i-1];
        }

        //writing the temp_free array into bNumber for it to be reused later
        writeBlockToFS(bNumber,temp_free,sizeof(temp_free));
        superBlock.nfree = 0;
        
    }else if(bNumber > 0){
        //we just initialise the block with bunch of zeros
        lseek(fd,BLOCKSIZE * (bNumber),SEEK_SET);
        write(fd,&zeros,sizeof(zeros));
    }
    
    //in any case we set the value of free array to bNumber and increase the nfree value
    superBlock.free[superBlock.nfree] = bNumber;
    superBlock.nfree++;

    lseek(fd,1024,SEEK_SET);
    write(fd,&superBlock,sizeof(superBlock));
}

int getFreeBlock(){
    //reducing nfree value by 1
    superBlock.nfree--;

    //if no free data block remains
    if(superBlock.free[superBlock.nfree] == 0){
        printf("No free data block available\n");
        return -1;
    }

    //if nfree becomes 0 we copy the values from next chain, as per the algorithms taught in class
    if(superBlock.nfree == 0){
        int bNumber = superBlock.free[0];
        lseek(fd,BLOCKSIZE * bNumber,SEEK_SET);
        int chain[252];
        read(fd,&chain,sizeof(chain));
        int i;
        superBlock.nfree = chain[0];
        
        for(i=1;i<=251;i++)
            superBlock.free[i-1] = chain[i];
        
        lseek(fd,1024,SEEK_SET);
        write(fd,&superBlock,sizeof(superBlock));
        printf("Free Block Number allocated: %d\n",bNumber);
        return bNumber;
    }else{
        lseek(fd,1024,SEEK_SET);
        write(fd,&superBlock,sizeof(superBlock));
        printf("Free Block Number allocated: %d\n",superBlock.free[superBlock.nfree]);
        return superBlock.free[superBlock.nfree];
    }
}

void quit(){
    printf("Received quit command\nClosing File\n");
    close(fd);
    printf("Quitting\n");
    exit(0);
}

void initfs(int totalBlocks,int totalInodeBlocks){
    printf("Initializing the file system\n");
    int totalIsize = 0;
    int total_num_inodes = totalInodeBlocks*16;

    //initializing superblock with appropriate values
    superBlock.isize = totalInodeBlocks;
    superBlock.fsize = totalBlocks;
    superBlock.flock = 'x';
    superBlock.ilock = 'x';
    superBlock.fmod = 'x';
    superBlock.time = (int)time(NULL); //unix epoch time
    superBlock.nfree = 0;

    printf("Writing Super Block to the file system\n");

    writeBlockToFS(1,&superBlock,BLOCKSIZE);

    int currBlockNumber = totalInodeBlocks + 2;
    int i;
    
    printf("Adding all free blocks\n");

    for(currBlockNumber = totalInodeBlocks + 1;currBlockNumber<totalBlocks;currBlockNumber++){
        if(currBlockNumber == totalInodeBlocks + 1)
            addFreeBlock(0);
        else
            addFreeBlock(currBlockNumber);
    }

    int idx;
    for(idx = 0;idx<256;idx++)
        zeros[idx] = 0;

    int currInodeNumber;

    printf("Allocating free inode to File System\n");

    for(currInodeNumber = 2;currInodeNumber<=total_num_inodes;currInodeNumber++){
        inode_type temp_inode;
        
        temp_inode.flags = 0;
        temp_inode.size0 = 0;
        temp_inode.size1 = 0;
        temp_inode.nlinks = 0;
        temp_inode.uid = 0;
        temp_inode.gid = 0;
        temp_inode.addr[0] = 0;
        temp_inode.addr[1] = 0;
        temp_inode.addr[2] = 0;
        temp_inode.addr[3] = 0;
        temp_inode.addr[4] = 0;
        temp_inode.addr[5] = 0;
        temp_inode.addr[6] = 0;
        temp_inode.addr[7] = 0;
        temp_inode.addr[8] = 0;
        temp_inode.actime = 0;
        temp_inode.modtime = 0;

        lseek(fd,2048 + ((currInodeNumber-1)*64),SEEK_SET);
        write(fd,&temp_inode,sizeof(temp_inode));

    }

    int status = allocateNewInodeToDir(1,1); //allocating inode 1 as root
    curr_inode = 1;

}

/*
makedir() - function to create a new directory
parameters : dir_name - directory name to be created, inode_curr - curr_inode where the directory needs to be created
we move to inode_curr and create a new new entry in the first free 32 bytes directory entry
we allocate a new inode to directory using allocateNewInodeToDir()
*/

int makedir(char* dir_name,int inode_curr){
    
    if(strlen(dir_name) > 28){
        printf("Length of file/dir should be less than or equal to 28 characters\n");
        return -1;
    }

    lseek(fd,2048+(64*(inode_curr-1)),SEEK_SET);
    inode_type temp_inode;
    read(fd,&temp_inode,sizeof(temp_inode));

    int idx = 0;
    int dir_made = 0;
    int temp_addr = -1;
    
    //checking if directory already present
    for(idx = 0;idx<9;idx++){
        if(temp_inode.addr[idx]!=0){
            int dir_idx = 0;
            for(dir_idx=0;dir_idx<32;dir_idx++){
                lseek(fd,(1024*temp_inode.addr[idx]) + 32*dir_idx,SEEK_SET);
                dir_type temp_dir;
                read(fd,&temp_dir,sizeof(temp_dir));
                if(strcmp(temp_dir.filename,dir_name) == 0){
                    return -2;
                }
            }
        }
    }

    //looping through all the addr and searching for the first 32 bytes where the inode number is -1
    for(idx=0;idx<9;idx++){
        if(temp_inode.addr[idx]!=0){
            int dir_idx;
            for(dir_idx=0;dir_idx<32;dir_idx++){
                lseek(fd,(1024*temp_inode.addr[idx]) + 32*dir_idx,SEEK_SET);
                dir_type temp_dir;
                read(fd,&temp_dir,sizeof(temp_dir));
                if(temp_dir.inode == -1 && dir_made == 0){
                    int new_inode = allocateNewInodeToDir(-1,inode_curr);
                    dir_made = 1;
                    temp_dir.inode = new_inode;
                    int j;
                    for(j=0;j<strlen(dir_name);j++)
                        temp_dir.filename[j] = dir_name[j];
                    
                    for(;j<28;j++){
                        temp_dir.filename[j] = '\0';
                    }
                    lseek(fd,(1024*temp_inode.addr[idx]) + 32*dir_idx,SEEK_SET);
                    write(fd,&temp_dir,sizeof(temp_dir));
                    break;
                }
            }

            if(dir_made)
                break;
        }else if(temp_addr == -1){
            temp_addr = idx;
        }
    }

    /*
        if directory isn't made successfully in the previous block we check for the first addr which is free
        then we create a new block to be dedicated for a block using allocateFreeBlockToDir()
    */

    if(dir_made == 0){
        if(temp_addr == -1)
            return -1;
        if(temp_addr == 0)
            temp_inode.addr[temp_addr] = allocateFreeBlockToDir(-1,inode_curr,1,inode_curr);
        else
            temp_inode.addr[temp_addr] = allocateFreeBlockToDir(-1,inode_curr,0,inode_curr);
        
        if(temp_inode.addr[temp_addr] == -1)
            return -1;
        
        lseek(fd,(1024*temp_inode.addr[temp_addr]) + 32*2,SEEK_SET);
        dir_type temp_dir;
        read(fd,&temp_dir,sizeof(temp_dir));
        int new_inode = allocateNewInodeToDir(-1,inode_curr);
        temp_dir.inode = new_inode;
        int j;
        for(j=0;j<strlen(dir_name);j++)
            temp_dir.filename[j] = dir_name[j];

        //we then write the temp_directory in the appropriate address
        lseek(fd,(1024*temp_inode.addr[temp_addr]) + 32*2,SEEK_SET);
        write(fd,&temp_dir,sizeof(temp_dir));
    }

    //we change the size of the temp_inode to accomodate the additional 32 bytes
    temp_inode.size1 = temp_inode.size1 + 32;
    lseek(fd,2048+(64*(inode_curr-1)),SEEK_SET);
    write(fd,&temp_inode,sizeof(temp_inode));

    return 1;
}

/*
path_to_inode() - this is a utility function to navigate and resolve a path to its inode
paramteres - ppath - path that needs to be parsed, curr_inode_temp - if explicitly there is a need to specify the curr_inode
*/

int path_to_inode(char* ppath,int curr_inode_temp){
    char path[256];
    int i;
    int count = 0; //stores the number of parts in a path
    int len = strlen(ppath);
    for(i=0;i<strlen(ppath);i++){
        path[i] = ppath[i];
        if(i>0 && path[i] == '/')
            count++;
    }
    path[i] = '\0';
    
    int curr;

    if(curr_inode_temp!=-1)
        curr = curr_inode_temp;
    else
        curr = curr_inode;

    if(path[len-1]!='/')
        count++;
    
    i = 0;

    if(path[0] == '/'){
        curr= 1;
        i++;
    }

    char dir[29]; //dir stores the current directory in consideration
    int h = 0;

    while(path[i]!='/' && i<len){
        dir[h] = path[i];
        i++;
        h++;
    }
    dir[h] = '\0';
    i++;
    
    //this loop will pick all the directories in parts splitted by '/' and then look for that directory in curr directory
    //we then modify the curr directory to the inode of that directory in case it is found

    while(dir != NULL && count>0) {
        lseek(fd,2048+(64*(curr-1)),SEEK_SET);
        inode_type temp_inode;
        read(fd,&temp_inode,sizeof(temp_inode));
        int flag_found = 0;

        //needs to be checked if the curr directory which is being looked at is a directory or not
        int check_dir = temp_inode.flags & (1<<14);
        int check_dir2 = temp_inode.flags & (1<<13);

        if(check_dir == 16384 &&  check_dir2 == 0){
            int idx = 0;
            for(idx=0;idx<9;idx++){ //looping through all the addr and checking if dir is found or not
                if(temp_inode.addr[idx]!=0){
                    int dir_idx;
                    for(dir_idx=0;dir_idx<32;dir_idx++){
                        lseek(fd,(1024*temp_inode.addr[idx]) + 32*dir_idx,SEEK_SET);
                        dir_type temp_dir;
                        read(fd,&temp_dir,sizeof(temp_dir));
                        int temp = strcmp(dir,temp_dir.filename);
                        if(strcmp(dir,temp_dir.filename) == 0){
                            addr_dir = (1024*temp_inode.addr[idx]) + 32*dir_idx;
                            addr_inode = curr;
                            curr = temp_dir.inode;
                            flag_found = 1;
                            break;
                        }
                    }
                    if(flag_found)
                        break;
                }
            }
        }

        if(flag_found == 0) //if we didn't find dir then the address is not valid
            return -1;
        
        int h = 0;
        while(path[i]!='/' && i<len){
            dir[h] = path[i];
            i++;
            h++;
        }
        i++;
        dir[h] = '\0';
        count--;
    }
    return curr;
}

/*
rm() - used to delete a file,returns -1 if trying to delete a directory
paramteres - path of the file which needs to be deleted
description:    we need to parse the given path to find the parent inode of the directory entry which needs to be deleted
                we also need to fetch the inode of the file which needs to deleted
*/
int rm(char* path){ //addr_dir = address in bytes where the directory entry is present

    int curr = path_to_inode(path,-1);

    //reading the current inode whihc needs to be deleted
    inode_type temp_inode;
    lseek(fd,2048+(64*(curr-1)),SEEK_SET);
    read(fd,&temp_inode,64);

    //reading the parent inode of the file which needs to be delted
    inode_type parent_inode;
    lseek(fd,2048+(64*(addr_inode-1)),SEEK_SET);
    read(fd,&parent_inode,64);

    //checking if the given path corresponds to a file
    int if_file = temp_inode.flags & 1<<12;
    int if_file2 = temp_inode.flags & 1<<13;
    
    if(if_file || if_file2)
        return -1;

    int i=0;
    int file_inode;
    //setting all addr of curr inode to default value
    for(i=0;i<9;i++){
        
        if(temp_inode.addr[i] == 0)
            continue;
        
        int curr_block = temp_inode.addr[i];
        temp_inode.addr[i] = 0;

        printf("Block number %d freed\n",curr_block);

        addFreeBlock(curr_block);
    }

    temp_inode.flags = 0;
    temp_inode.size0 = 0;
    temp_inode.size1 = 0;
    temp_inode.nlinks = 0;
    temp_inode.uid = 0;
    temp_inode.gid = 0;
    temp_inode.actime = 0;
    temp_inode.modtime = 0;

    //unallocating the inode
    lseek(fd,2048+(64*(curr-1)),SEEK_SET);
    write(fd,&temp_inode,sizeof(zeros));

    printf("Inode Number: %d deemed unallocated\n",curr);
    
    lseek(fd,addr_dir,SEEK_SET);
    dir_type temp_dir;
    read(fd,&temp_dir,sizeof(temp_dir));
    temp_dir.inode = -1;

    //setting the filename in the parent inode as null values
    memset(temp_dir.filename,'\0',sizeof(temp_dir.filename));
    lseek(fd,addr_dir,SEEK_SET);
    write(fd,&temp_dir,sizeof(addr_dir));

    //reducing the parent inode size
    parent_inode.size1 = parent_inode.size1 - 32;
    lseek(fd,2048+(64*(addr_inode-1)),SEEK_SET);
    write(fd,&parent_inode,64);

    return 1; //successfully deleted;

}

/*
cpin() - used to copy external file to internal v6 filesystem
parameters: extFile - path to external file, intFile - intFile name;
            inode_curr - inode of the directory where the files needs to stored
description: we first find and allocate a free inode for this file.
            we set the directory entry in the parent inode to inode number and file name
            we allocate free data blocks for this file
            copy contents of extfile in a buffer of size 1024 which we write to the available free data block
*/
int cpin(char* extFile,char* intFile,int inode_curr){
    
    if(strlen(intFile)>28){
        printf("Length of file/directory should be less than or equal to 28 characters\n");
        return -1;
    }
    
    int fde = open(extFile, O_CREAT | O_RDWR, 0644);
    char buf[1024];

    lseek(fd,2048+(64*(inode_curr-1)),SEEK_SET);
    inode_type temp_inode;
    read(fd,&temp_inode,sizeof(temp_inode));

    int idx = 0;
    int fileMade = 0;
    int temp_addr = -1;
    inode_type newInode;

    struct stat st;
    stat(extFile, &st);

    int free_inode;

    //looping through all the addrs of the current inode to find the first free 32 bytes to allocate new inode
    for(idx=0;idx<9;idx++){
        if(temp_inode.addr[idx]!=0){
            int dir_idx;
            for(dir_idx=0;dir_idx<32;dir_idx++){
                lseek(fd,(1024*temp_inode.addr[idx]) + 32*dir_idx,SEEK_SET);
                dir_type temp_dir;
                read(fd,&temp_dir,sizeof(temp_dir));
                if(temp_dir.inode == -1){
                    free_inode = findUnallocatedInode();
                    lseek(fd,2048 + (64*(free_inode-1)),SEEK_SET);
                    read(fd,&newInode,64);
                    //1(allocated)00(plain file)00(small file)0(uid)0(gid)111(rwx for owner)101(rx for group)100(read for everyone)
                    newInode.flags = 33260;
                    newInode.size0 = 0;
                    newInode.size1 = st.st_size; //size of inode.size1 equal to extFile size
                    newInode.nlinks = 1;
                    newInode.actime = (int)time(NULL);
                    newInode.modtime = (int)time(NULL);

                    temp_dir.inode = free_inode;
                    int j;
                    for(j=0;j<strlen(intFile);j++)
                        temp_dir.filename[j] = intFile[j];
                    for(;j<28;j++)
                        temp_dir.filename[j] = '\0';
                    fileMade = 1;
                    lseek(fd,(1024*temp_inode.addr[idx]) + 32*dir_idx,SEEK_SET);
                    write(fd,&temp_dir,sizeof(temp_dir));

                    break;
                }
            }

        if(fileMade)
            break;
        }else if(temp_addr == -1){
            temp_addr = idx;
        }
    }

    //if file isn't created successfully, then we check if there is any address which is 0 and then allocate a free block to it
    //this free block will be a dir block where in turn we will allocate a inode to it and do a similar process as above

    if(fileMade == 0){
        if(temp_addr == -1)
            return -1;
        
        if(temp_addr == 0)
            temp_inode.addr[temp_addr] = allocateFreeBlockToDir(-1,inode_curr,1,inode_curr);
        else
            temp_inode.addr[temp_addr] = allocateFreeBlockToDir(-1,inode_curr,0,inode_curr);

        if(temp_inode.addr[temp_addr] == -1)
            return -1;
        
        lseek(fd,(1024*temp_inode.addr[temp_addr]) + 32*2,SEEK_SET);
        dir_type temp_dir;
        read(fd,&temp_dir,sizeof(temp_dir));

        free_inode = findUnallocatedInode();
        lseek(fd,2048 + (64*(free_inode-1)),SEEK_SET);
        read(fd,&newInode,64);
        newInode.flags = 32260; 
        newInode.size0 = 0;
        newInode.size1 = st.st_size;
        newInode.nlinks = 1;
        newInode.actime = (int)time(NULL);
        newInode.modtime = (int)time(NULL);

        temp_dir.inode = free_inode;
        
        int j;
        for(j=0;j<strlen(intFile);j++)
            temp_dir.filename[j] = intFile[j];
        for(;j<28;j++)
            temp_dir.filename[j] = '\0';

        lseek(fd,(1024*temp_inode.addr[temp_addr]) + 32*2,SEEK_SET);
        write(fd,&temp_dir,sizeof(temp_dir));
    }

    //here we copy all the contents of the external file to internal file system

    int flag = 1;
    int addr_idx = 0;
    while(flag == 1){
        lseek(fde,1024*idx,SEEK_SET);
        int num_bytes = read(fde,&buf,1024);
        printf("Num Bytes read:%d\n",num_bytes);
        if(num_bytes!=1024)
            flag = 0;
        
        newInode.addr[addr_idx] = getFreeBlock(); //we fetch a free block to store the contents
        if(newInode.addr[addr_idx] == -1)
            return -1;
        
        lseek(fd,1024*newInode.addr[addr_idx],SEEK_SET);
        write(fd,&buf,num_bytes);

        idx++;
        addr_idx++;
    }

    //changing the size of the parent inode to include the new directory entry
    temp_inode.size1 = temp_inode.size1 + 32;
    lseek(fd,2048+(64*(inode_curr-1)),SEEK_SET);
    write(fd,&temp_inode,sizeof(temp_inode));

    lseek(fd,2048+(64*(free_inode-1)),SEEK_SET);
    write(fd,&newInode,sizeof(newInode));

    return 1;
}

/*
cpout() - copy from internal filesystem to external filesystem
parameteres: extFile - externalFile path; intFile - internalFile path
description - we move to the inode of the intFile path and copy all the data blocks represented by addr to extFile
*/
int cpout(char* extFile,char* intFile){
    int inode_curr = path_to_inode(intFile,-1); //fetching the inode to intFile

    printf("Inode for Int file:%d\n",inode_curr);
    if(inode_curr == -1)
        return -1;
    
    int fde = open(extFile, O_CREAT | O_RDWR, 0644); //opening the external file to write contents
    
    char buf[1024];

    inode_type temp_inode;
    lseek(fd,2048 + ((inode_curr-1)*64),SEEK_SET);
    read(fd,&temp_inode,sizeof(temp_inode));

    int i=0;
    int sz = temp_inode.size1; //storing the file size in a temp variable
    
    for(i=0;i<9;i++){
        if(temp_inode.addr[i]!=0){
            lseek(fd,1024*temp_inode.addr[i],SEEK_SET);
            int to_write;
            //reading to buffer
            if(sz>=1024){
                read(fd,&buf,1024);
                sz = sz - 1024;
                to_write = 1024;
            }else{
                read(fd,&buf,sz);
                to_write = sz;
                sz = 0;
            }

            //writing to external file system
            write(fde,&buf,to_write);
        }
    }

    //updating access time
    temp_inode.actime = (int)time(NULL);
    lseek(fd,2048 + ((inode_curr-1)*64),SEEK_SET);
    write(fd,&temp_inode,sizeof(temp_inode));
}

//utility function to process the path to split the path with '/' delimeter
int process_path(char* path){
    int len = strlen(path);
    
    int k=0;
    if(path[0] == '/')
        k++;
    int count = 1;
    for(;k<len;k++){
        if(path[k] == '/')
            count++;
    }

    int i=len-1;
    while(i>=0 && path[i]!='/'){
        i--;
    }

    char temp[256];

    int j=0;
    while(j<i){
        temp[j] = path[j];
        j++;
    }

    temp[j] = '\0';

    int idx = 0;
    while((i+1)<len){
        i++;
        last_dir[idx++] = path[i];
    }
    last_dir[idx] = '\0'; // global string to store the last directory in a path

    if(count == 1){
        if(path[0] == '/'){
            return 1;
        }else{
            return curr_inode;
        }
    }

    return path_to_inode(temp,-1); //returns the inode of the path
}

void main(){

    while(1){
        printf("###################################\n");
        printf("Input command alongwith arguments\n");
        
        char cmd[256];
        scanf(" %[^\n]s",cmd); //for reading strings with whitespaces
        
        char *token;
        char *first;
        char *second;
        token = strtok(cmd," "); //using strtok to split string based upon delimeter
        
        if(strcmp(token,"openfs") == 0){
            first = strtok(NULL," ");
            openfs(first);
        }else if(strcmp(token,"initfs") == 0){
            first = strtok(NULL," ");
            second = strtok(NULL," ");

            initfs(atoi(first),atoi(second));
        }else if(strcmp(token,"cpin") == 0){
            first = strtok(NULL," ");
            second = strtok(NULL," ");

            int inode_curr = process_path(second);
            int status;

            if(inode_curr == -1)
                printf("Error: Not a valid directory\n");
            else{
                status = cpin(first,last_dir,inode_curr);
                if(status == -1){
                    printf("cpin unsuccesfull\n");
                }else{
                    printf("File copied succesfully\n");
                }
            }

        }else if(strcmp(token,"cpout") == 0){
            first = strtok(NULL," ");
            second = strtok(NULL," ");

            int status = cpout(second,first);
            if(status == -1)
                printf("Invalid file/address\n");
            
            if(status == -1){
                printf("Writing file failed\n");
            }else{
                printf("File written succesfully\n");
            }

        }else if(strcmp(token,"mkdir") == 0){
            first = strtok(NULL," ");
            int inode_curr = process_path(first);
            int status;

            if(inode_curr == -1)
                printf("Error: Not a directory\n");
            else
                status = makedir(last_dir,inode_curr);

            if(status == -2)
                printf("Cannot create directory, %s already present\n",first);
            else if(status == -1){
                printf("mkdir unsuccesfull\n");
            }else{
                printf("%s created successfully\n",first);
            }

        }else if(strcmp(token,"cd") == 0){
            first = strtok(NULL," ");
            int temp_curr_inode = curr_inode;
            curr_inode = path_to_inode(first,-1);
            printf("Curr Inode set to %d\n",curr_inode);
            if(curr_inode == -1){
                curr_inode = temp_curr_inode;
                printf("Invalid Address\n");
            }

        }else if(strcmp(token,"rm") == 0){
            first = strtok(NULL," ");
            int status = rm(first);

            if(status == -1){
                printf("File not found\n");
            }else{
                printf("File deleted succesfully\n");
            }

        }else if(strcmp(token,"q") == 0){
            quit();
        }else{
            printf("Invalid command\n");
        }
    }
}