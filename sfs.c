#define FUSE_USE_VERSION 26

#include <errno.h>
#include <fuse.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>

#include "sfs.h"
#include "diskio.h"


static const char default_img[] = "test.img";

/* Options passed from commandline arguments */
struct options {
    const char *img;
    int background;
    int verbose;
    int show_help;
    int show_fuse_help;
} options;


#define log(fmt, ...) \
    do { \
        if (options.verbose) \
            printf(" # " fmt, ##__VA_ARGS__); \
    } while (0)


/* libfuse2 leaks, so let's shush LeakSanitizer if we are using Asan. */
const char* __asan_default_options() { return "detect_leaks=0"; }

/*
Function that reads in all directory entries from a certain directory
Argument is pointer to the array of sfs_entry that needs to be filled,
second argument is first block of the directory
*/
static void load_dir(struct sfs_entry *dir, blockidx_t firstblock) {
    // first block
    disk_read(dir, SFS_BLOCK_SIZE, SFS_DATA_OFF + firstblock * SFS_BLOCK_SIZE);
    // second block
    blockidx_t *next = (blockidx_t*) malloc(sizeof(blockidx_t));
    disk_read(next, sizeof(blockidx_t), SFS_BLOCKTBL_OFF + firstblock * sizeof(blockidx_t));
    disk_read(dir+SFS_BLOCK_SIZE/sizeof(struct sfs_entry), SFS_BLOCK_SIZE, SFS_DATA_OFF + *next * SFS_BLOCK_SIZE);
    free(next);
    return;
}

static int count_blocks(blockidx_t firstblock) {
    blockidx_t curr = firstblock;
    int count = 0;

    while (curr != SFS_BLOCKIDX_END) {
        disk_read(
            &curr,
            sizeof(blockidx_t),
            SFS_BLOCKTBL_OFF + curr * sizeof(blockidx_t)
        );
        count ++;
    }

    return count;
}

static void read_file(char *buf, blockidx_t firstblock) {
    blockidx_t curr = firstblock;
    size_t blocks_read = 0;
    while (curr != SFS_BLOCKIDX_END) {
        // Read from the data
        disk_read(
            (buf+blocks_read*SFS_BLOCK_SIZE), 
            SFS_BLOCK_SIZE, 
            SFS_DATA_OFF + curr * SFS_BLOCK_SIZE
        );

        blocks_read ++;

        // Read what next block id is
        disk_read(
            &curr,
            sizeof(blockidx_t),
            SFS_BLOCKTBL_OFF + curr * sizeof(blockidx_t)
        );
    }
}

/*
Function that seperates the last part of a path and returns the parent path
*/
static void get_parent(const char *path, char *parent_path) {
    strcpy(parent_path, path);
    char* child = strrchr(parent_path, '/');
    child[0] = '\0';
    return;
}

/*
Function that seperates the last part of a path and returns the child name*/
static void get_child(const char *path, char **newdir) {
    const char delim = '/';
    *newdir = strrchr(path, delim) + 1;
    return;
}

/*
Function to check if the file or directory at path is in the root directory*/
static int in_root(const char *path) {
    int count = 0;
    for (size_t i = 0; i < strlen(path); i++)
    {
        if (path[i] == '/') {count++;}
    }
    if (count == 1) {return 1;} else {return 0;}
}



/*
 * This is a helper function that is optional, but highly recomended you
 * implement and use. Given a path, it looks it up on disk. It will return 0 on
 * success, and a non-zero value on error (e.g., the file did not exist).
 * The resulting directory entry is placed in the memory pointed to by
 * ret_entry. Additionally it can return the offset of that direntry on disk in
 * ret_entry_off, which you can use to update the entry and write it back to
 * disk (e.g., rmdir, unlink, truncate, write).
 *
 * You can start with implementing this function to work just for paths in the
 * root entry, and later modify it to also work for paths with subdirectories.
 * This way, all of your other functions can use this helper and will
 * automatically support subdirectories. To make this function support
 * subdirectories, we recommend you refactor this function to be recursive, and
 * take the current directory as argument as well. For example:
 *
 *  static int get_entry_rec(const char *path, const struct sfs_entry *parent,
 *                           size_t parent_nentries, blockidx_t parent_blockidx,
 *                           struct sfs_entry *ret_entry,
 *                           unsigned *ret_entry_off)
 *
 * Here parent is the directory it is currently searching (at first the rootdir,
 * later the subdir). The parent_nentries tells the function how many entries
 * there are in the directory (SFS_ROOTDIR_NENTRIES or SFS_DIR_NENTRIES).
 * Finally, the parent_blockidx contains the blockidx of the given directory on
 * the disk, which will help in calculating ret_entry_off.
 */

static int get_entry_rec(const char *path, struct sfs_entry *parent,
                           size_t parent_nentries,
                           blockidx_t *parent_blockidx,
                           char *token,
                           struct sfs_entry *ret_entry,
                           unsigned *ret_entry_off) 
{
    struct sfs_entry *ent;
    for (size_t i = 0; i < parent_nentries; i++)
    {
        ent = parent + i;
        if (strcmp(token, ent->filename) == 0) {
            token = strtok(NULL, "/");
            if (token == NULL) {
                // We have reached end of path
                *ret_entry = *ent;
                *ret_entry_off = i;
                return 0;
            } else {
                // Need to read in the next dir
                struct sfs_entry *newparent = (struct sfs_entry*) malloc(SFS_DIR_SIZE);
                load_dir(newparent, ent->first_block);

                *parent_blockidx = ent->first_block;
                int r = get_entry_rec(path, newparent, SFS_DIR_NENTRIES, parent_blockidx, token, ret_entry, ret_entry_off);

                free(newparent);
                return r;
            }
        } 
    }
    return -ENOENT;
}

static int get_entry(const char *path, struct sfs_entry *ret_entry,
                     unsigned *ret_entry_off, blockidx_t *parent_blockidx)
{
    /* Get the next component of the path. Make sure to not modify path if it is
     * the value passed by libfuse (i.e., make a copy). Note that strtok
     * modifies the string you pass it. */

    /* Allocate memory for the rootdir (and later, subdir) and read it from disk */

    /* Loop over all entries in the directory, looking for one with the name
     * equal to the current part of the path. If it is the last part of the
     * path, return it. If there are more parts remaining, recurse to handle
     * that subdirectory. */


    char *pathc = (char*) malloc(strlen(path) + 1);
    strncpy(pathc, path, strlen(path) + 1);
    char *token = strtok(pathc, "/");

    struct sfs_entry *root = (struct sfs_entry*) malloc(SFS_ROOTDIR_SIZE);
    disk_read(root, SFS_ROOTDIR_SIZE, SFS_ROOTDIR_OFF);

    int r = get_entry_rec(pathc, root, SFS_ROOTDIR_NENTRIES, parent_blockidx, token, ret_entry, ret_entry_off);
    
    free(root); free(pathc);
    return r;
}



/*
 * Retrieve information about a file or directory.
 * You should populate fields of `stbuf` with appropriate information if the
 * file exists and is accessible, or return an error otherwise.
 *
 * For directories, you should at least set st_mode (with S_IFDIR) and st_nlink.
 * For files, you should at least set st_mode (with S_IFREG), st_nlink and
 * st_size.
 *
 * Return 0 on success, < 0 on error.
 */
static int sfs_getattr(const char *path,
                       struct stat *st)
{
    int res = 0;

    log("getattr %s\n", path);

    memset(st, 0, sizeof(struct stat));
    /* Set owner to user/group who mounted the image */
    st->st_uid = getuid();
    st->st_gid = getgid();
    /* Last accessed/modified just now */
    st->st_atime = time(NULL);
    st->st_mtime = time(NULL);

    if (strcmp(path, "/") == 0) {
        st->st_mode = S_IFDIR | 0755;
        st->st_nlink = 2;
    } else {
        struct sfs_entry *ret_entry = (struct sfs_entry*) malloc(sizeof(struct sfs_entry));
        unsigned int *ret_entry_off = (unsigned int*) malloc(sizeof(int));
        blockidx_t *parent_blockidx = (blockidx_t *) malloc(sizeof(blockidx_t));
        int r = get_entry(path, ret_entry, ret_entry_off, parent_blockidx);
        if (r == -ENOENT) {
            res = r;
        } else {
            st->st_size = ret_entry->size;
            if (SFS_DIRECTORY & ret_entry->size) {
                st->st_mode = S_IFDIR;
                st->st_nlink = 2;
            }
            else {
                st->st_mode = S_IFREG;
                st->st_nlink = 1;
            }
        }
        free(ret_entry); free(ret_entry_off);
    }

    return res;
}


/*
 * Return directory contents for `path`. This function should simply fill the
 * filenames - any additional information (e.g., whether something is a file or
 * directory) is later retrieved through getattr calls.
 * Use the function `filler` to add an entry to the directory. Use it like:
 *  filler(buf, <dirname>, NULL, 0);
 * Return 0 on success, < 0 on error.
 */
static int sfs_readdir(const char *path,
                       void *buf,
                       fuse_fill_dir_t filler,
                       off_t offset,
                       struct fuse_file_info *fi)
{
    (void)offset; (void)fi;
    log("readdir %s\n", path);
    int err;

    if (strcmp(path, "/") == 0) {
        // root directory

        struct sfs_entry *rootdir = (struct sfs_entry *) malloc(SFS_ROOTDIR_SIZE);
        disk_read(rootdir, SFS_ROOTDIR_SIZE, SFS_ROOTDIR_OFF);

        for (size_t i = 0; i < SFS_ROOTDIR_NENTRIES; i++)
        {
            struct sfs_entry *ent = rootdir + i;
            if (strlen(ent->filename) == 0) {
                continue;
            } else {
                err = filler(buf, ent->filename, NULL, 0);
            }
        }

        free(rootdir); 
        return err;
    } else {

        struct sfs_entry *ret_entry = (struct sfs_entry*) malloc(sizeof(struct sfs_entry));
        unsigned int *ret_entry_offset = (unsigned int *) malloc(sizeof(unsigned int));
        blockidx_t *parent_blockidx = (blockidx_t *) malloc(sizeof(blockidx_t));

        int r = get_entry(path, ret_entry, ret_entry_offset, parent_blockidx);
        if (r != 0) {return r;}

        free(ret_entry_offset); free(parent_blockidx);

        // get directory data
        struct sfs_entry *dir = (struct sfs_entry*) malloc(SFS_DIR_SIZE);
        load_dir(dir, ret_entry->first_block);

        // find all files
        struct sfs_entry *ent;
        for (size_t i = 0; i < SFS_DIR_NENTRIES; i++)
        {
            ent = dir + i;
            if (strlen(ent->filename) == 0) {
                continue;
            } else {
                err = filler(buf, ent->filename, NULL, 0);
            }
        }

        free(ret_entry); free(dir);
        return err;
    }
}


/*
 * Read contents of `path` into `buf` for  up to `size` bytes.
 * Note that `size` may be bigger than the file actually is.
 * Reading should start at offset `offset`; the OS will generally read your file
 * in chunks of 4K byte.
 * Returns the number of bytes read (writting into `buf`), or < 0 on error.
 */
static int sfs_read(const char *path,
                    char *buf,
                    size_t size,
                    off_t offset,
                    struct fuse_file_info *fi)
{
    (void)fi;
    log("read %s size=%zu offset=%ld\n", path, size, offset);

    // find the entry

    struct sfs_entry *ent = (struct sfs_entry *) malloc(sizeof(struct sfs_entry));
    unsigned int *ent_off = (unsigned int*) malloc(sizeof(unsigned int));
    blockidx_t *parent_blockidx = (blockidx_t *) malloc(sizeof(blockidx_t));

    if (get_entry(path, ent, ent_off, parent_blockidx) == -ENOENT) {
        return -ENOENT;
    }

    free(ent_off); free(parent_blockidx); // won't use those
    if (ent->first_block == SFS_BLOCKIDX_END) {return 0;}

    // Load the file in memory entirely
    int amnt_blocks = count_blocks(ent->first_block);
    char *file_buf = (char *) malloc(amnt_blocks * SFS_BLOCK_SIZE);
    read_file(file_buf, ent->first_block);

    // read into the buffer the right bytes
    if (ent->size - (size_t)offset < size) {size = ent->size - offset;}
    memcpy(buf, file_buf + offset, size);
    
    free(ent); free(file_buf);
    return size;
}


/*
 * Create directory at `path`.
 * The `mode` argument describes the permissions, which you may ignore for this
 * assignment.
 * Returns 0 on success, < 0 on error.
 */
static int sfs_mkdir(const char *path,
                     mode_t mode)
{
    log("mkdir %s mode=%o\n", path, mode);

    // Seperating the last name from the path
    char* newdir;
    get_child(path, &newdir);

    // check size of name
    if (strlen(newdir) >= 58) {return -ENAMETOOLONG;}

    // find two empty blocks
    blockidx_t *blocktable = (blockidx_t*) malloc(SFS_BLOCKTBL_SIZE);
    disk_read(blocktable, SFS_BLOCKTBL_SIZE, SFS_BLOCKTBL_OFF);

    blockidx_t block1 = SFS_BLOCKIDX_EMPTY;
    blockidx_t block2 = SFS_BLOCKIDX_EMPTY;
    for (size_t i = 0; i < SFS_BLOCKTBL_NENTRIES - 1; i++)
    {
        if (blocktable[i] == SFS_BLOCKIDX_EMPTY && blocktable[i+1] == SFS_BLOCKIDX_EMPTY) {
            block1 = i;
            block2 = i + 1;
            break;
        }
    }
    if (block1 == SFS_BLOCKIDX_EMPTY) {return -1;} // no more space

    // set correct values of block1 and block2 in the block table
    disk_write(&block2, sizeof(blockidx_t), SFS_BLOCKTBL_OFF + block1 * sizeof(blockidx_t)); // block 1 points to block 2
    blockidx_t end = SFS_BLOCKIDX_END;
    disk_write(&end, sizeof(blockidx_t), SFS_BLOCKTBL_OFF + block2 * sizeof(blockidx_t)); // block 2 points to end
    
    // fill blocks with empty entries
    // we make one large array of empty entries and write it at once
    // instead of writing one by one which is inefficient
    struct sfs_entry *empty_ent = (struct sfs_entry*) malloc(sizeof(struct sfs_entry));
    strcpy(empty_ent->filename, "");
    empty_ent->size = 0;
    empty_ent->first_block = SFS_BLOCKIDX_EMPTY;

    struct sfs_entry *empty_entries = (struct sfs_entry*) malloc(SFS_DIR_SIZE);
    for (size_t i = 0; i < SFS_DIR_NENTRIES; i++)
    {
        memcpy(empty_entries + i, empty_ent, sizeof(struct sfs_entry));
    }
    disk_write(empty_entries, SFS_DIR_SIZE, SFS_DATA_OFF + block1 * SFS_BLOCK_SIZE);

    free(empty_entries); free(empty_ent);

    // Finding an empty entry in parent
    if (in_root(path) == 1) {
        // find it in root directory
        struct sfs_entry *rootdir = (struct sfs_entry *) malloc(SFS_ROOTDIR_SIZE);;
        disk_read(rootdir, SFS_ROOTDIR_SIZE, SFS_ROOTDIR_OFF);

        struct sfs_entry *empty_ent;
        for (size_t i = 0; i < SFS_ROOTDIR_NENTRIES; i++)
        {
            empty_ent = rootdir + i;
            if (strlen(empty_ent->filename) == 0) {
                break;
            }
        }

        strcpy(empty_ent->filename, newdir);
        empty_ent->size = SFS_DIRECTORY;
        empty_ent->first_block = block1;

        disk_write(rootdir, SFS_ROOTDIR_SIZE, SFS_ROOTDIR_OFF);
        free(rootdir);

    } else {
        // find it in subdirectory

        // retreive parent path
        char *parent_path = (char *) malloc((strlen(path)+1) * sizeof(char));
        get_parent(path, parent_path);

        struct sfs_entry *parent_entry = (struct sfs_entry*) malloc(sizeof(struct sfs_entry));
        unsigned int *parent_entry_off = (unsigned int*) malloc(sizeof(unsigned int));
        blockidx_t *parent_blockidx = (blockidx_t *) malloc(sizeof(blockidx_t));

        int r = get_entry(parent_path, parent_entry, parent_entry_off, parent_blockidx);
        if (r != 0) {return r;}

        free(parent_blockidx); free(parent_entry_off); free(parent_path); // won't use those

        // find empty entry in parent
        struct sfs_entry *parent_dir = (struct sfs_entry*) malloc(SFS_DIR_SIZE);;
        load_dir(parent_dir, parent_entry->first_block);
        
        struct sfs_entry *empty_ent;
        for (size_t i = 0; i < SFS_DIR_NENTRIES; i++)
        {
            empty_ent = parent_dir + i;
            if (strlen(empty_ent->filename) == 0) {
                break;
            }
        }

        strcpy(empty_ent->filename, newdir);
        empty_ent->size = SFS_DIRECTORY;
        empty_ent->first_block = block1;

        disk_write(parent_dir, SFS_DIR_SIZE, SFS_DATA_OFF + parent_entry->first_block * SFS_BLOCK_SIZE);        

        free(parent_entry);
    }     

    free(blocktable);
    return 0;
}


/*
 * Remove directory at `path`.
 * Directories may only be removed if they are empty, otherwise this function
 * should return -ENOTEMPTY.
 * Returns 0 on success, < 0 on error.
 */
static int sfs_rmdir(const char *path)
{
    log("rmdir %s\n", path);
    struct sfs_entry *ret_entry = (struct sfs_entry *) malloc(sizeof(struct sfs_entry));
    unsigned int *ret_entry_off = (unsigned int *) malloc(sizeof(off_t));
    blockidx_t *parent_blockidx = (blockidx_t *) malloc(sizeof(blockidx_t));

    int r = get_entry(path, ret_entry, ret_entry_off, parent_blockidx);
    if (r != 0) {return r;}

    // Load the directory into memory
    struct sfs_entry *dir = (struct sfs_entry *) malloc(SFS_DIR_SIZE);
    load_dir(dir, ret_entry->first_block);

    // check if directory is empty
    for (size_t i = 0; i < SFS_DIR_NENTRIES; i++)
    {
        struct sfs_entry *ent = dir + i;
        if (strlen(ent->filename) != 0) {
            return -ENOTEMPTY;
        }
    }

    // free the blocks
    blockidx_t freeblock = SFS_BLOCKIDX_EMPTY;
    blockidx_t block2;
    disk_read(&block2, sizeof(blockidx_t), SFS_BLOCKTBL_OFF + ret_entry->first_block * sizeof(blockidx_t));
    disk_write(&freeblock, sizeof(blockidx_t), SFS_BLOCKTBL_OFF + ret_entry->first_block * sizeof(blockidx_t));
    disk_write(&freeblock, sizeof(blockidx_t), SFS_BLOCKTBL_OFF + block2 * sizeof(blockidx_t));

    // remove entry from parents
    strcpy(ret_entry->filename, "");
    ret_entry->size = 0;
    ret_entry->first_block = freeblock;

    // write to disk
    if (in_root(path) == 1) {
        disk_write(ret_entry, sizeof(struct sfs_entry), SFS_ROOTDIR_OFF + *ret_entry_off * sizeof(struct sfs_entry));
    } else {
        disk_write(ret_entry, sizeof(struct sfs_entry), SFS_DATA_OFF + *parent_blockidx * SFS_BLOCK_SIZE + *ret_entry_off * sizeof(struct sfs_entry));
    }


    free(ret_entry); free(ret_entry_off); free(parent_blockidx); free(dir);
    return 0;
}


/*
 * Remove file at `path`.
 * Can not be used to remove directories.
 * Returns 0 on success, < 0 on error.
 */
static int sfs_unlink(const char *path)
{
    log("unlink %s\n", path);

    // Get entry of file
    struct sfs_entry *ret_entry = (struct sfs_entry *) malloc(sizeof(struct sfs_entry));
    unsigned int *ret_entry_off = (unsigned int *) malloc(sizeof(off_t));
    blockidx_t *parent_blockidx = (blockidx_t *) malloc(sizeof(blockidx_t));

    int r = get_entry(path, ret_entry, ret_entry_off, parent_blockidx);
    if (r != 0) {return r;}

    blockidx_t freeblock = SFS_BLOCKIDX_EMPTY;

    // remove entries from blocktable

    blockidx_t curr = ret_entry->first_block;
    blockidx_t next;
    while (curr != SFS_BLOCKIDX_END) {
        disk_read(&next, sizeof(blockidx_t), SFS_BLOCKTBL_OFF + curr * sizeof(blockidx_t));
        disk_write(&freeblock, sizeof(blockidx_t), SFS_BLOCKTBL_OFF + curr * sizeof(blockidx_t));
        curr = next;
    }

    // remove entry from parent
    strcpy(ret_entry->filename, "");
    ret_entry->size = 0;
    ret_entry->first_block = freeblock;

    // write it to disk
    if (in_root(path) == 1) {
        disk_write(ret_entry, sizeof(struct sfs_entry), SFS_ROOTDIR_OFF + *ret_entry_off * sizeof(struct sfs_entry));
    } else {
        disk_write(ret_entry, sizeof(struct sfs_entry), SFS_DATA_OFF + *parent_blockidx * SFS_BLOCK_SIZE + *ret_entry_off * sizeof(struct sfs_entry));
    }

    free(ret_entry); free(ret_entry_off); free(parent_blockidx);

    return 0;
}


/*
 * Create an empty file at `path`.
 * The `mode` argument describes the permissions, which you may ignore for this
 * assignment.
 * Returns 0 on success, < 0 on error.
 */
static int sfs_create(const char *path,
                      mode_t mode,
                      struct fuse_file_info *fi)
{
    (void)fi;
    log("create %s mode=%o\n", path, mode);

    // Get the filename
    char *newdir;
    get_child(path, &newdir);
    if (strlen(newdir) >= 58) {return -ENAMETOOLONG;}

    // create a new entry for a file
    struct sfs_entry newfile;
    strcpy(newfile.filename, newdir);
    newfile.size = 0;
    newfile.first_block = SFS_BLOCKIDX_END;

    // find empty entry and write
    if (in_root(path) == 1) {
        // in rootdir
        struct sfs_entry *rootdir = (struct sfs_entry *) malloc(SFS_ROOTDIR_SIZE);
        disk_read(rootdir, SFS_ROOTDIR_SIZE, SFS_ROOTDIR_OFF);

        struct sfs_entry *empty_ent;
        for (size_t i = 0; i < SFS_ROOTDIR_NENTRIES; i++)
        {
            if (strlen(rootdir[i].filename) == 0) {
                empty_ent = rootdir + i;
            }
        }
        if (!empty_ent) {return -1;} // no more entries

        *empty_ent = newfile;

        disk_write(rootdir, SFS_ROOTDIR_SIZE, SFS_ROOTDIR_OFF);
        free(rootdir);
        
    } else {
        // find it in subdir

        // get parent path
        char *parent_path = (char *) malloc((strlen(path)+1) * sizeof(char));
        get_parent(path, parent_path);

        struct sfs_entry *parent_entry = (struct sfs_entry *) malloc(sizeof(struct sfs_entry));
        unsigned int *parent_entry_off = (unsigned int *) malloc(sizeof(off_t));
        blockidx_t *parent_blockidx = (blockidx_t *) malloc(sizeof(blockidx_t));

        int r = get_entry(parent_path, parent_entry, parent_entry_off, parent_blockidx);
        if (r != 0) {return r;}

        free(parent_path);

        free(parent_entry_off); free(parent_blockidx); // won't use those

        // load parent dir
        struct sfs_entry *parent_dir = (struct sfs_entry *) malloc(SFS_DIR_SIZE);
        load_dir(parent_dir, parent_entry->first_block);
        
        struct sfs_entry *empty_ent;
        for (size_t i = 0; i < SFS_DIR_NENTRIES; i++)
        {
            if (strlen(parent_dir[i].filename) == 0) {
                empty_ent = parent_dir + i;
            }
        }

        if (!empty_ent) {return -1;} // no more entries

        *empty_ent = newfile;

        disk_write(parent_dir, SFS_DIR_SIZE, SFS_DATA_OFF + parent_entry->first_block * SFS_BLOCK_SIZE);
        free(parent_dir); free(parent_entry);
    }


    return 0;
}


/*
 * Shrink or grow the file at `path` to `size` bytes.
 * Excess bytes are thrown away, whereas any bytes added in the process should
 * be nil (\0).
 * Returns 0 on success, < 0 on error.
 */
static int sfs_truncate(const char *path, off_t size)
{
    log("truncate %s size=%ld\n", path, size);

    // getting the entry
    struct sfs_entry *ret_entry = (struct sfs_entry *) malloc(sizeof(struct sfs_entry));
    unsigned int *ret_entry_off = (unsigned int *) malloc(sizeof(off_t));
    blockidx_t *parent_blockidx = (blockidx_t *) malloc(sizeof(blockidx_t));
    int r = get_entry(path, ret_entry, ret_entry_off, parent_blockidx);
    if (r != 0){return r;}



    unsigned int curr_block_amnt = (ret_entry->size + SFS_BLOCK_SIZE - 1) / SFS_BLOCK_SIZE;
    unsigned int block_amnt_need = (size + SFS_BLOCK_SIZE - 1) / SFS_BLOCK_SIZE;
    if (block_amnt_need < curr_block_amnt) {
        // SHRINKING

        // getting all the blockidx in an array
        blockidx_t *blocks = (blockidx_t *) malloc(curr_block_amnt*sizeof(blockidx_t));
        blocks[0] = ret_entry->first_block;
        for (size_t i = 1; i < curr_block_amnt; i++)
        {
            disk_read(blocks + i, sizeof(blockidx_t), SFS_BLOCKTBL_OFF + blocks[i-1] * sizeof(blockidx_t));
        }

        // removing the right amount of blocks starting from the back
        int blocks_torem = curr_block_amnt - block_amnt_need;
        blockidx_t empty = SFS_BLOCKIDX_EMPTY;
        for (int i = 0; i < blocks_torem; i++)
        {
            disk_write(&empty, sizeof(blockidx_t), SFS_BLOCKTBL_OFF + blocks[curr_block_amnt - (1 + i)] * sizeof(blockidx_t));
        }
        
        // write EOF on last used block
        if (size != 0) {
            blockidx_t eof = SFS_BLOCKIDX_END;
            disk_write(&eof, sizeof(blockidx_t), SFS_BLOCKTBL_OFF + blocks[curr_block_amnt - (1 + blocks_torem)] * sizeof(blockidx_t));
        } else {
            ret_entry->first_block = SFS_BLOCKIDX_END;
        }

        free(blocks);

    } else if (block_amnt_need > curr_block_amnt) {
        // GROWING

        // loading up blocktable
        blockidx_t *blocktable = (blockidx_t *) malloc(SFS_BLOCKTBL_SIZE);
        disk_read(blocktable, SFS_BLOCKTBL_SIZE, SFS_BLOCKTBL_OFF);

        // finding right amount of free blocks
        int blocks_to_add = block_amnt_need - curr_block_amnt;
        blockidx_t newblocks[blocks_to_add];
        
        int found = 0;
        for (size_t i = 0; i < SFS_BLOCKTBL_NENTRIES; i++)
        {
            if (blocktable[i] == SFS_BLOCKIDX_EMPTY) {
                newblocks[found] = i;
                found++;
                if (found == blocks_to_add) {break;}
            }
        }

        // find last blockidx
        if (ret_entry->first_block != SFS_BLOCKIDX_END) {
            blockidx_t lastblock = ret_entry->first_block;
            blockidx_t next = blocktable[lastblock];
            while (next != SFS_BLOCKIDX_END) {
                lastblock = next;
                next = blocktable[lastblock];
            }
            blocktable[lastblock] = newblocks[0];
        } else {
            ret_entry->first_block = newblocks[0];
        }
        
        // setting right values for those blocks in table
        for (int i = 0; i < blocks_to_add - 1; i++)
        {
            blocktable[newblocks[i]] = newblocks[i+1];
        }
        blocktable[newblocks[blocks_to_add - 1]] = SFS_BLOCKIDX_END;

        // write back blocktable
        disk_write(blocktable, SFS_BLOCKTBL_SIZE, SFS_BLOCKTBL_OFF);
        
        free(blocktable);
    }

    // change size in entry
    ret_entry->size = size;


    // write the new entry for the file
    if (in_root(path) == 1) {
        disk_write(ret_entry, sizeof(struct sfs_entry), SFS_ROOTDIR_OFF + *ret_entry_off * sizeof(struct sfs_entry));
    } else {
        disk_write(ret_entry, sizeof(struct sfs_entry), SFS_DATA_OFF + *parent_blockidx * SFS_BLOCK_SIZE + *ret_entry_off * sizeof(struct sfs_entry));
    }
    free(parent_blockidx);
    free(ret_entry_off); 
    free(ret_entry);
    return 0;
}


/*
 * Write contents of `buf` (of `size` bytes) to the file at `path`.
 * The file is grown if nessecary, and any bytes already present are overwritten
 * (whereas any other data is left intact). The `offset` argument specifies how
 * many bytes should be skipped in the file, after which `size` bytes from
 * buffer are written.
 * This means that the new file size will be max(old_size, offset + size).
 * Returns the number of bytes written, or < 0 on error.
 */
static int sfs_write(const char *path,
                     const char *buf,
                     size_t size,
                     off_t offset,
                     struct fuse_file_info *fi)
{
    (void)fi;
    log("write %s data='%.*s' size=%zu offset=%ld\n", path, (int)size, buf,
        size, offset);

    return -ENOSYS;
}


/*
 * Move/rename the file at `path` to `newpath`.
 * Returns 0 on succes, < 0 on error.
 */
static int sfs_rename(const char *path,
                      const char *newpath)
{
    /* Implementing this function is optional, and not worth any points. */
    log("rename %s %s\n", path, newpath);

    return -ENOSYS;
}


static const struct fuse_operations sfs_oper = {
    .getattr    = sfs_getattr,
    .readdir    = sfs_readdir,
    .read       = sfs_read,
    .mkdir      = sfs_mkdir,
    .rmdir      = sfs_rmdir,
    .unlink     = sfs_unlink,
    .create     = sfs_create,
    .truncate   = sfs_truncate,
    .write      = sfs_write,
    .rename     = sfs_rename,
};


#define OPTION(t, p)                            \
    { t, offsetof(struct options, p), 1 }
#define LOPTION(s, l, p)                        \
    OPTION(s, p),                               \
    OPTION(l, p)
static const struct fuse_opt option_spec[] = {
    LOPTION("-i %s",    "--img=%s",     img),
    LOPTION("-b",       "--background", background),
    LOPTION("-v",       "--verbose",    verbose),
    LOPTION("-h",       "--help",       show_help),
    OPTION(             "--fuse-help",  show_fuse_help),
    FUSE_OPT_END
};

static void show_help(const char *progname)
{
    printf("usage: %s mountpoint [options]\n\n", progname);
    printf("By default this FUSE runs in the foreground, and will unmount on\n"
           "exit. If something goes wrong and FUSE does not exit cleanly, use\n"
           "the following command to unmount your mountpoint:\n"
           "  $ fusermount -u <mountpoint>\n\n");
    printf("common options (use --fuse-help for all options):\n"
           "    -i, --img=FILE      filename of SFS image to mount\n"
           "                        (default: \"%s\")\n"
           "    -b, --background    run fuse in background\n"
           "    -v, --verbose       print debug information\n"
           "    -h, --help          show this summarized help\n"
           "        --fuse-help     show full FUSE help\n"
           "\n", default_img);
}

int main(int argc, char **argv)
{
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);

    options.img = strdup(default_img);

    fuse_opt_parse(&args, &options, option_spec, NULL);

    if (options.show_help) {
        show_help(argv[0]);
        return 0;
    }

    if (options.show_fuse_help) {
        assert(fuse_opt_add_arg(&args, "--help") == 0);
        args.argv[0][0] = '\0';
    }

    if (!options.background)
        assert(fuse_opt_add_arg(&args, "-f") == 0);

    disk_open_image(options.img);

    return fuse_main(args.argc, args.argv, &sfs_oper, NULL);
}

