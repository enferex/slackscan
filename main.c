/*
 * slackscan: A filesystem slack space scanner.
 *
 *
 * MIT License
 *
 * Copyright (c) 2016 Matt Davis (enferex)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <ctype.h>
#include <fcntl.h>
#include <getopt.h>
#include <unistd.h>
#include <et/com_err.h>
#include <ext2fs/ext2fs.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

/* Resources:
 * Google
 * ext2fs.h
 * Probably this at some point: http://www.nongnu.org/ext2-doc/ext2.html
 * See 2.2.2: http://dubeyko.com/development/FileSystems/ext2fs/libext2fs.pdf
 */

static void usage(const char *execname)
{
    printf("Usage: %s -f <file> -d <device> [-v]\n"
           "  -d <device>: Parition to scan (e.g., /dev/sda1)\n"
           "  -f <file>:   File to scan\n"
           "  -v:          Display indiviual slackspace information\n"
#ifdef EXPERIMENTAL_EXTRACT
           "  -x:          Dump contents of file (-f)\n"
#endif
           , execname);
           //"  -i <file>:   Inject file into slackspace\n",
    exit(EXIT_SUCCESS);
}

/* Calculate slack space */
static size_t calc_slack(size_t blksize, blk64_t n_blocks, size_t fsize)
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
    blk64_t     n_blocks,
    size_t      n_bytes,
    size_t      total_slack)
{
    printf("%s: %zu inodes, %llu blocks, %zu bytes, %zu slack bytes\n",
           name, n_inodes, n_blocks, n_bytes, total_slack);
}

/* Returns the amount of slack space */
static unsigned process_inode(
    ext2_filsys        fs,
    struct ext2_inode *inode,
    ext2_ino_t         dir,
    ext2_ino_t         ino,
    _Bool              be_verbose)
{
    char *name;
    size_t size = EXT2_I_SIZE(inode);
    blk64_t n_blocks = ext2fs_inode_data_blocks2(fs, inode);
    const unsigned slack = calc_slack(fs->blocksize, n_blocks, size);

    if (be_verbose) {
        ext2fs_get_pathname(fs, dir, ino, &name);
        printf("[%u:%s]: (%u blocks) (%zu bytes) (slack %u) "
               "(blocksize %u)\n",
               ino, name, inode->i_blocks, size, slack,
               EXT2_BLOCK_SIZE(fs->super));
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
    blk64_t n_blocks;
    size_t n_inodes, n_bytes, total_slack;
    struct ext2_inode inode;

    /* Open the device (first partition (e.g., /dev/sda1)) */
    printf("Scanning device: %s...\n", devname);
    if ((err = ext2fs_open(devname, 0, 0, 0, unix_io_manager, &fs))) {
        fprintf(stderr, "Error opening '%s' (error: %ld: %s)\n",
                devname, err, error_message(err));
        exit(EXIT_FAILURE);
    }

    /* Scan all inodes in 'devname' and collect stats */
    n_inodes = n_blocks = n_bytes = total_slack = 0;
    ext2fs_open_inode_scan(fs, 0, &scanner);
    while (((err = ext2fs_get_next_inode(scanner, &ino, &inode)) == 0) &&
           (ino != 0)) {
        if (LINUX_S_ISDIR(inode.i_mode))
          dir = ino;

        total_slack += process_inode(fs, &inode, dir, ino, verbose);
        n_bytes     += EXT2_I_SIZE(&inode);
        n_blocks    += ext2fs_inode_data_blocks2(fs, &inode);
        ++n_inodes;
    }

    /* Cleanup */
    ext2fs_close_inode_scan(scanner);
    ext2fs_close(fs);

    print_summary(devname, n_inodes, n_blocks, n_bytes, total_slack);
}

/* Scan /proc/partitions and return an allocated string that matches devid */
static char *get_device_name(dev_t devid)
{
    FILE *fp;
    _Bool found;
    int min, maj;
    char line[1024] = {0};
    char *nametok, *name;

    if (!(fp = fopen("/proc/partitions", "r"))) {
        fprintf(stderr, "Error opening /proc/partitions: %s\n",
                strerror(errno));
        exit(EXIT_FAILURE);
    }

    /* Read each line in /proc/partitions until we find a match for devid */
    found = false;
    name = nametok = NULL;
    while (fgets(line, sizeof(line), fp) != NULL) {
        if (line[0] == '#' || !isspace(line[0]) || line[0] == '\n')
          continue;
        maj = atoi(strtok(line, " "));
        min = atoi(strtok(NULL, " "));
        (void) strtok(NULL, " ");
        nametok = strtok(NULL, "\n");
        if (major(devid) == maj && minor(devid) == min) {
            found = true;
            break;
        }
    }

    if (!found) {
        fprintf(stderr, "Error finding device string\n");
        exit(EXIT_FAILURE);
    }

    /* Alloc a blob to store the device name with the /dev/ prefix */
    if (!(name = malloc(strlen(nametok) + strlen("/dev/") + 1))) {
        fprintf(stderr, "Error duplicating device string\n");
        exit(EXIT_FAILURE);
    }

    /* Chop off trailing \n */
    if (nametok[strlen(nametok)-1] == '\n')
      nametok[strlen(nametok)-1] = '\0';

    sprintf(name, "/dev/%s", nametok);
    fclose(fp);
    return name;
}

/* Scan a file for slack space */
static void scan_file(const char *fname, _Bool extract)
{
    ext2_filsys fs;
    ext2_file_t file;
    blk64_t n_blocks;
    errcode_t err;
    char *devname;
    struct ext2_inode *inode;
    size_t n_inodes, n_bytes, total_slack;
    struct stat st;

    /* Get file's inode and device info */
    if (stat(fname, &st) == -1) {
        fprintf(stderr, "Error calling stat on %s: %s\n",
               fname, strerror(errno));
        exit(EXIT_FAILURE);
    }

    /* Open the fs struct and get the inode for ino */
    devname = get_device_name(st.st_dev);
    if ((err = ext2fs_open(devname, 0, 0, 0, unix_io_manager, &fs))) {
        fprintf(stderr, "Error opening '%s' (error: %ld: %s)\n",
                devname, err, error_message(err));
        exit(EXIT_FAILURE);
    }

    if ((err = ext2fs_file_open(fs, st.st_ino, 0, &file))) {
        fprintf(stderr, "Error  '%s' (error: %ld: %s)\n",
                devname, err, error_message(err));
        exit(EXIT_FAILURE);
    }
    inode = ext2fs_file_get_inode(file);

    n_inodes    = 1;
    n_bytes     = EXT2_I_SIZE(inode);
    n_blocks    = ext2fs_inode_data_blocks2(fs, inode);
    total_slack = calc_slack(EXT2_BLOCK_SIZE(fs->super), n_blocks, n_bytes);
    print_summary(fname, n_inodes, n_blocks, n_bytes, total_slack);

#ifdef EXPERIMENTAL_EXTRACT
    if (extract) {
        int i;
        FILE *fp;
        printf("Slack contents of %s:\n", fname);
        if (!(fp = fopen(fname, "r"))) {
            fprintf(stderr, "Could not obtain a file handle to %s\n", fname);
            exit(EXIT_FAILURE);
        }

        rewind(fp);
        fgetc(fp);

        for (i=0; i<n_bytes+8; /*i<(4096-st.st_size);*/ ++i)
          printf("0x%02x ", ((char *)fp->_IO_read_base)[i]);
    }
#endif /* EXPERIMENTAL_EXTRACT */

    ext2fs_close(fs);
    free(devname);
}

int main(int argc, char **argv)
{
    int opt;
    _Bool annoying, extract;
    const char *devname, *fname;

    /* Args */
    annoying = extract = false;
    fname = devname = NULL;
    while ((opt = getopt(argc, argv, "i:d:f:vx")) != -1) {
        switch (opt) {
        case 'd': devname = optarg; break;
        case 'f': fname = optarg; break;
        case 'v': annoying = true; break;
#ifdef EXPERIMENTAL_EXTRACT
        case 'x': extract = true; break;
#endif
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

    if (devname)
      scan_device(devname, annoying);
    if (fname)
      scan_file(fname, extract);

    return 0;
}
