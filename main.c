#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <getopt.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <et/com_err.h>
#include <ext2fs/ext2fs.h>

/* Resources:
 * Google.
 * ext2fs.h
 * Probably this at some point: http://www.nongnu.org/ext2-doc/ext2.html
 */

static void usage(const char *execname)
{
    printf("Usage: %s -f <filesystem> -d <device> [-v]i [-x]\n", 
           "  -d <device>: Parition to scan (e.g., /dev/sda1)\n"
           "  -f <file>:   File to scan\n"
           "  -v:          Display indiviual slackspace information\n"
           "  -x:          Dump contents of file (-f)\n",
           //"  -i <file>:   Inject file into slackspace\n",
           execname);
    exit(EXIT_SUCCESS);
}

/* Calculate slack space */
static size_t calc_slack(size_t blksize, size_t n_blocks, size_t fsize)
{
    size_t slack = 0;

    if (n_blocks) {
        if ((n_blocks * blksize) >= fsize)
          slack = (n_blocks * blksize) - fsize;
    }
    else {
        if (blksize >= fsize)
          slack = blksize - fsize;
    }

    return slack;
}

static void print_summary(
    const char *name,
    size_t      n_inodes,
    size_t      n_blocks,
    size_t      n_bytes,
    size_t      total_slack)
{
    printf("%s: %zu inodes, %zu blocks, %zu bytes, %zu slack bytes\n",
           name, n_inodes, n_blocks, n_bytes, total_slack);
}

static int process_block(ext2_filsys fs, blk_t *blocknr, int blockcnt, void *private)
{
    ++(*((unsigned *)private));
    return 0;
}

static unsigned count_blocks(ext2_filsys fs, ext2_ino_t ino)
{
    unsigned count = 0;
    ext2fs_block_iterate(fs, ino, 0, NULL, process_block, (void *)&count);
    return count;
}
    
/* Returns the amount of slack space */
static unsigned process_inode(
    ext2_filsys              fs,
    const struct ext2_inode *inode,
    ext2_ino_t               dir,
    ext2_ino_t               ino,
    _Bool                    be_verbose)
{
    char *name;
    const unsigned actual_blocks = count_blocks(fs, ino);
    const unsigned slack = calc_slack(
        fs->blocksize, actual_blocks/*inode->i_blocks*/, inode->i_size);

    if (be_verbose) {
        ext2fs_get_pathname(fs, dir, ino, &name);
        printf("[%u:%s] (%u blocks) (%u bytes) (slack %u) "
               "(actual blocks: %u)\n",
               ino, name, inode->i_blocks, inode->i_size, slack, actual_blocks);
        ext2fs_free_mem(&name);
    }

    return slack;
}

/* Scan a device (e.g., /dev/sda1) 
 * for slack space (requires proper permissions).
 */
static void scan_device(const char *devname,  _Bool verbose)
{
    errcode_t err;
    ext2_filsys fs;
    ext2_ino_t ino, dir;
    ext2_inode_scan scanner;
    size_t n_inodes, n_blocks, n_bytes, total_slack;
    struct ext2_inode inode;

    n_inodes = n_blocks = n_bytes = total_slack = 0;

    /* Open the device (first partition (e.g., /dev/sda1)) */
    printf("Scanning device: %s...\n", devname);
    if ((err = ext2fs_open(devname, 0, 0, 0, unix_io_manager, &fs))) {
        fprintf(stderr, "Error opening '%s' (error: %d: %s)\n",
                devname, err, error_message(err));
        exit(EXIT_FAILURE);
    }

    /* Scan all inodes in 'devname' and collect stats */
    ext2fs_open_inode_scan(fs, 0, &scanner);
    while ((err = ext2fs_get_next_inode(scanner, &ino, &inode) == 0) && 
           (ino != 0)) {
        if (LINUX_S_ISDIR(inode.i_mode))
          dir = ino;
        total_slack += process_inode(fs, &inode, dir, ino, verbose);
        n_bytes += inode.i_size;
        n_blocks += inode.i_blocks;
        ++n_inodes;
    }

    /* Cleanup */
    ext2fs_close_inode_scan(scanner);
    ext2fs_close(fs);

    print_summary(devname, n_inodes, n_blocks, n_bytes, total_slack);
}

/* Scan a file for slack space */
static void scan_file(const char *fname, _Bool extract)
{
    int fd;
    FILE *fp;
    struct stat st;
    size_t i, n_inodes, n_blocks, n_bytes, total_slack;

    /* Just use fstat to get file properties */
    if ((fd = open(fname, O_RDONLY)) == -1) {
        fprintf(stderr, "Could not open file: %s\n", fname);
        exit(EXIT_FAILURE);
    }
    fstat(fd, &st);
    n_inodes = 1;
    n_bytes = st.st_size;
    n_blocks = st.st_blocks;
    total_slack = calc_slack(st.st_blksize, n_blocks, n_bytes);
    print_summary(fname, n_inodes, n_blocks, n_bytes, total_slack);

    if (extract) {
        printf("Slack contents of %s:\n", fname);
        if (!(fp = fdopen(fd, "r"))) {
            fprintf(stderr, "Could not obtain a file handle to %s\n", fname);
            exit(EXIT_FAILURE);
        }

        fseek(fp, 1, SEEK_SET);
        fseek(fp, 0, SEEK_CUR);
        fgetc(fp);

        for (i=0; i<st.st_size; /*i<(4096-st.st_size);*/ ++i)
          printf("0x%02x ", ((char *)fp->_IO_read_base)[i]);
    }

    close(fd);
}

/* Inject the file 'file' into the slackspace on disk 'devname' */
static void inject_file(const char *file, const char *devname)
{
    fprintf(stderr, "Slack space injection is not yet supported.\n");
}

int main(int argc, char **argv)
{
    int opt;
    _Bool annoying, extract, inject;
    const char *devname, *fname;

    /* Args */
    annoying = extract = inject;
    fname = devname = NULL;
    while ((opt = getopt(argc, argv, "i:d:f:vx")) != -1) {
        switch (opt) {
        //case 'i': inject = optarg; break;
        case 'd': devname = optarg; break;
        case 'f': fname = optarg; break;
        case 'v': annoying = true; break;
        case 'x': extract = true; break;
        default:
            fprintf(stderr, "Unknown option specified see the help:\n");
            usage(argv[0]);
            break;
        }
    }

    if (!devname && !fname) {
        fprintf(stderr, "No device or file specified\n");
        exit(EXIT_FAILURE);
    }

    initialize_ext2_error_table();

    /* The user can either perform file injection or scan (display metadata).
     * Not both.
     */
    if (inject)
      inject_file(fname, devname);
    else {
        if (devname)
          scan_device(devname, annoying);
        if (fname)
          scan_file(fname, extract);
    }

    return 0;
}
