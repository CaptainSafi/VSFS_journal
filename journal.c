#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define FSMAGIC         0x56534653U
#define BLOCKSIZE       4096U
#define INODESIZE       128U

#define JOURNALBLOCKIDX 1U
#define JOURNALBLOCKS   16U
#define INODEBLOCKS     2U
#define DATABLOCKS      64U

#define INODEBMAPIDX    (JOURNALBLOCKIDX + JOURNALBLOCKS)
#define DATABMAPIDX     (INODEBMAPIDX + 1U)
#define INODESTARTIDX   (DATABMAPIDX + 1U)
#define DATASTARTIDX    (INODESTARTIDX + INODEBLOCKS)
#define TOTALBLOCKS     (DATASTARTIDX + DATABLOCKS)

#define DIRECTPOINTERS  8U
#define DEFAULTIMAGE    "vsfs.img"

#define JOURNALMAGIC    0x4A524E4CU  /* "JRNL" */

#define RECDATA         1
#define RECCOMMIT       2

struct superblock {
    uint32_t magic;
    uint32_t blocksize;
    uint32_t totalblocks;
    uint32_t inodecount;
    uint32_t journalblock;
    uint32_t inodebitmap;
    uint32_t databitmap;
    uint32_t inodestart;
    uint32_t datastart;
    uint8_t  pad[128 - 9 * 4];
};

struct inode {
    uint16_t type;   /* 0=free,1=file,2=dir */
    uint16_t links;
    uint32_t size;
    uint32_t direct[DIRECTPOINTERS];
    uint32_t ctime;
    uint32_t mtime;
    uint8_t  pad[128 - 2 - 2 - 4 - DIRECTPOINTERS * 4 - 4 - 4];
};

#define NAMELEN 28
struct dirent {
    uint32_t inode;
    char     name[NAMELEN];
};

struct journalheader {
    uint32_t magic;
    uint32_t nbytesused;
};

struct recheader {
    uint16_t type;   /* RECDATA or RECCOMMIT */
    uint16_t size;   /* total size of this record in bytes */
};

struct datarecord {
    struct recheader hdr;
    uint32_t blockno;
    uint8_t  data[BLOCKSIZE];
};

struct commitrecord {
    struct recheader hdr;
};

static void die(const char *msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}

static void preadblock(int fd, uint32_t blockindex, void *buf) {
    off_t offset = (off_t)blockindex * BLOCKSIZE;
    ssize_t n = pread(fd, buf, BLOCKSIZE, offset);
    if (n != (ssize_t)BLOCKSIZE) {
        die("pread");
    }
}

static void pwriteblock(int fd, uint32_t blockindex, const void *buf) {
    off_t offset = (off_t)blockindex * BLOCKSIZE;
    ssize_t n = pwrite(fd, buf, BLOCKSIZE, offset);
    if (n != (ssize_t)BLOCKSIZE) {
        die("pwrite");
    }
}

static int bitmap_test(const uint8_t *bitmap, uint32_t index) {
    return (bitmap[index / 8] >> (index % 8)) & 0x1;
}

static void bitmap_set(uint8_t *bitmap, uint32_t index) {
    bitmap[index / 8] |= (uint8_t)(1U << (index % 8));
}

/* Journal helpers */

static off_t journal_start_offset(void) {
    return (off_t)JOURNALBLOCKIDX * BLOCKSIZE;
}

static uint32_t journal_total_bytes(void) {
    return JOURNALBLOCKS * BLOCKSIZE;
}

static void journal_read_header(int fd, struct journalheader *jh) {
    off_t off = journal_start_offset();
    ssize_t n = pread(fd, jh, sizeof(*jh), off);
    if (n != (ssize_t)sizeof(*jh)) {
        jh->magic = 0;
        jh->nbytesused = 0;
    }
}

static void journal_write_header(int fd, const struct journalheader *jh) {
    off_t off = journal_start_offset();
    ssize_t n = pwrite(fd, jh, sizeof(*jh), off);
    if (n != (ssize_t)sizeof(*jh)) {
        die("journal header write");
    }
}

static void journal_init_if_needed(int fd, struct journalheader *jh) {
    journal_read_header(fd, jh);

    if (jh->magic != JOURNALMAGIC ||
        jh->nbytesused < sizeof(struct journalheader) ||
        jh->nbytesused > journal_total_bytes()) {

        uint8_t zero[BLOCKSIZE];
        memset(zero, 0, sizeof(zero));
        for (uint32_t i = 0; i < JOURNALBLOCKS; i++) {
            pwriteblock(fd, JOURNALBLOCKIDX + i, zero);
        }

        jh->magic = JOURNALMAGIC;
        jh->nbytesused = sizeof(struct journalheader);
        journal_write_header(fd, jh);
    }
}

static uint32_t journal_space_remaining(const struct journalheader *jh) {
    uint32_t total = journal_total_bytes();
    if (jh->nbytesused > total) {
        return 0;
    }
    return total - jh->nbytesused;
}

static void journal_append_raw(int fd, struct journalheader *jh,
                               const void *record, uint16_t size) {
    off_t off = journal_start_offset() + (off_t)jh->nbytesused;
    ssize_t n = pwrite(fd, record, size, off);
    if (n != (ssize_t)size) {
        die("journal append");
    }
    jh->nbytesused += size;
    journal_write_header(fd, jh);
}

/* create implementation */

static void do_create(const char *imagepath, const char *name) {
    int fd = open(imagepath, O_RDWR);
    if (fd < 0) {
        die("open image");
    }

    struct superblock sb;
    preadblock(fd, 0, &sb);

    if (sb.magic != FSMAGIC || sb.blocksize != BLOCKSIZE ||
        sb.totalblocks != TOTALBLOCKS) {
        fprintf(stderr, "Unexpected superblock; wrong image?\n");
        close(fd);
        exit(EXIT_FAILURE);
    }

    struct journalheader jh;
    journal_init_if_needed(fd, &jh);

    uint8_t inodebmap[BLOCKSIZE];
    uint8_t databmap[BLOCKSIZE];
    preadblock(fd, INODEBMAPIDX, inodebmap);
    preadblock(fd, DATABMAPIDX, databmap);

    uint32_t inodecount = (INODEBLOCKS * BLOCKSIZE) / INODESIZE;
    uint32_t totalinodebytes = INODEBLOCKS * BLOCKSIZE;
    uint8_t *inodearea = malloc(totalinodebytes);
    if (!inodearea) {
        die("malloc inodearea");
    }

    for (uint32_t i = 0; i < INODEBLOCKS; i++) {
        preadblock(fd, INODESTARTIDX + i, inodearea + i * BLOCKSIZE);
    }
    struct inode *inodes = (struct inode *)inodearea;

    struct inode *rootino = &inodes[0];
    if (rootino->type != 2) {
        fprintf(stderr, "root inode is not a directory\n");
        free(inodearea);
        close(fd);
        exit(EXIT_FAILURE);
    }
    if (rootino->direct[0] == 0) {
        fprintf(stderr, "root directory has no data block\n");
        free(inodearea);
        close(fd);
        exit(EXIT_FAILURE);
    }

    uint32_t root_blockno = rootino->direct[0];

    uint8_t dirblock[BLOCKSIZE];
    preadblock(fd, root_blockno, dirblock);

    struct dirent *dirents = (struct dirent *)dirblock;
    uint32_t max_entries = BLOCKSIZE / sizeof(struct dirent);

    uint32_t entries_in_use = rootino->size / sizeof(struct dirent);
    if (entries_in_use > max_entries) {
        fprintf(stderr, "root directory size is too large\n");
        free(inodearea);
        close(fd);
        exit(EXIT_FAILURE);
    }

    int32_t free_inum = -1;
    for (uint32_t i = 1; i < inodecount; i++) {
        if (!bitmap_test(inodebmap, i)) {
            free_inum = (int32_t)i;
            break;
        }
    }
    if (free_inum < 0) {
        fprintf(stderr, "No free inode available\n");
        free(inodearea);
        close(fd);
        exit(EXIT_FAILURE);
    }

    int32_t free_dirent_idx = -1;
    for (uint32_t i = 2; i < max_entries; i++) {
        if (dirents[i].inode == 0 || dirents[i].name[0] == '\0') {
            free_dirent_idx = (int32_t)i;
            break;
        }
    }
    if (free_dirent_idx < 0) {
        fprintf(stderr, "No free directory entry slot in root\n");
        free(inodearea);
        close(fd);
        exit(EXIT_FAILURE);
    }

    struct inode *newino = &inodes[free_inum];
    memset(newino, 0, sizeof(*newino));
    newino->type = 1;  /* file */
    newino->links = 1;
    newino->size = 0;
    memset(newino->direct, 0, sizeof(newino->direct));
    time_t now = time(NULL);
    newino->ctime = (uint32_t)now;
    newino->mtime = (uint32_t)now;

    bitmap_set(inodebmap, (uint32_t)free_inum);

    struct dirent *de = &dirents[free_dirent_idx];
    de->inode = (uint32_t)free_inum;
    memset(de->name, 0, sizeof(de->name));
    strncpy(de->name, name, sizeof(de->name) - 1);

    if ((uint32_t)free_dirent_idx >= entries_in_use) {
        rootino->size = (free_dirent_idx + 1) * sizeof(struct dirent);
    }

    uint32_t inode_byte_offset = (uint32_t)free_inum * INODESIZE;
    uint32_t inode_block_rel = inode_byte_offset / BLOCKSIZE;
    uint32_t inode_block_index = INODESTARTIDX + inode_block_rel;

    uint8_t inodeblock_buf[BLOCKSIZE];
    memcpy(inodeblock_buf,
           inodearea + inode_block_rel * BLOCKSIZE,
           BLOCKSIZE);

    uint16_t datarec_size = (uint16_t)(sizeof(struct recheader) +
                                       sizeof(uint32_t) +
                                       BLOCKSIZE);
    uint16_t commit_size = (uint16_t)sizeof(struct commitrecord);
    uint32_t needed = 3U * datarec_size + commit_size;

    if (journal_space_remaining(&jh) < needed) {
        fprintf(stderr, "Not enough space in journal; run 'journal install' first\n");
        free(inodearea);
        close(fd);
        exit(EXIT_FAILURE);
    }

    struct datarecord dr;

    memset(&dr, 0, sizeof(dr));
    dr.hdr.type = RECDATA;
    dr.hdr.size = (uint16_t)(sizeof(struct recheader) +
                             sizeof(uint32_t) +
                             BLOCKSIZE);
    dr.blockno = INODEBMAPIDX;
    memcpy(dr.data, inodebmap, BLOCKSIZE);
    journal_append_raw(fd, &jh, &dr, dr.hdr.size);

    memset(&dr, 0, sizeof(dr));
    dr.hdr.type = RECDATA;
    dr.hdr.size = (uint16_t)(sizeof(struct recheader) +
                             sizeof(uint32_t) +
                             BLOCKSIZE);
    dr.blockno = inode_block_index;
    memcpy(dr.data, inodeblock_buf, BLOCKSIZE);
    journal_append_raw(fd, &jh, &dr, dr.hdr.size);

    memset(&dr, 0, sizeof(dr));
    dr.hdr.type = RECDATA;
    dr.hdr.size = (uint16_t)(sizeof(struct recheader) +
                             sizeof(uint32_t) +
                             BLOCKSIZE);
    dr.blockno = root_blockno;
    memcpy(dr.data, dirblock, BLOCKSIZE);
    journal_append_raw(fd, &jh, &dr, dr.hdr.size);

    struct commitrecord cr;
    memset(&cr, 0, sizeof(cr));
    cr.hdr.type = RECCOMMIT;
    cr.hdr.size = (uint16_t)sizeof(struct commitrecord);
    journal_append_raw(fd, &jh, &cr, cr.hdr.size);

    printf("journal create: logged creation of '%s' as inode %d\n",
           name, free_inum);

    free(inodearea);
    close(fd);
}

/* install implementation */

struct txnentry {
    uint32_t blockno;
    uint8_t  data[BLOCKSIZE];
};

static void do_install(const char *imagepath) {
    int fd = open(imagepath, O_RDWR);
    if (fd < 0) {
        die("open image");
    }

    struct journalheader jh;
    journal_read_header(fd, &jh);

    if (jh.magic != JOURNALMAGIC ||
        jh.nbytesused < sizeof(struct journalheader) ||
        jh.nbytesused > journal_total_bytes()) {
        fprintf(stderr, "No valid journal present\n");
        close(fd);
        exit(EXIT_FAILURE);
    }

    uint32_t nbytes = jh.nbytesused;
    uint32_t pos = sizeof(struct journalheader);
    off_t base = journal_start_offset();

    struct txnentry *txn = NULL;
    uint32_t txn_cap = 0;
    uint32_t txn_len = 0;

    while (pos + sizeof(struct recheader) <= nbytes) {
        struct recheader hdr;
        ssize_t n = pread(fd, &hdr, sizeof(hdr), base + pos);
        if (n != (ssize_t)sizeof(hdr)) {
            die("install read header");
        }

        if (hdr.size < sizeof(struct recheader) ||
            pos + hdr.size > nbytes) {
            break;
        }

        if (hdr.type == RECDATA) {
            if (hdr.size != sizeof(struct recheader) +
                             sizeof(uint32_t) +
                             BLOCKSIZE) {
                break;
            }

            struct datarecord dr;
            n = pread(fd, &dr, hdr.size, base + pos);
            if (n != (ssize_t)hdr.size) {
                break;
            }

            if (txn_len == txn_cap) {
                uint32_t new_cap = txn_cap ? txn_cap * 2 : 8;
                struct txnentry *new_txn =
                    realloc(txn, new_cap * sizeof(*txn));
                if (!new_txn) {
                    die("realloc txn");
                }
                txn = new_txn;
                txn_cap = new_cap;
            }
            txn[txn_len].blockno = dr.blockno;
            memcpy(txn[txn_len].data, dr.data, BLOCKSIZE);
            txn_len++;
        } else if (hdr.type == RECCOMMIT) {
            if (hdr.size != sizeof(struct commitrecord)) {
                break;
            }

            struct commitrecord cr;
            n = pread(fd, &cr, hdr.size, base + pos);
            if (n != (ssize_t)hdr.size) {
                break;
            }

            for (uint32_t i = 0; i < txn_len; i++) {
                pwriteblock(fd, txn[i].blockno, txn[i].data);
            }
            txn_len = 0;
        } else {
            break;
        }

        pos += hdr.size;
    }

    free(txn);

    struct journalheader newjh;
    newjh.magic = JOURNALMAGIC;
    newjh.nbytesused = sizeof(struct journalheader);

    uint8_t zero[BLOCKSIZE];
    memset(zero, 0, sizeof(zero));
    for (uint32_t i = 0; i < JOURNALBLOCKS; i++) {
        pwriteblock(fd, JOURNALBLOCKIDX + i, zero);
    }
    journal_write_header(fd, &newjh);

    printf("journal install: applied committed transactions and cleared journal\n");
    close(fd);
}

/* main */

int main(int argc, char *argv[]) {
    const char *imagepath = DEFAULTIMAGE;

    if (argc < 2) {
        fprintf(stderr,
                "Usage:\n"
                "  %s create <name> [image]\n"
                "  %s install [image]\n",
                argv[0], argv[0]);
        return EXIT_FAILURE;
    }

    if (strcmp(argv[1], "create") == 0) {
        if (argc < 3) {
            fprintf(stderr, "create: missing file name\n");
            return EXIT_FAILURE;
        }
        if (argc >= 4) {
            imagepath = argv[3];
        }
        do_create(imagepath, argv[2]);
    } else if (strcmp(argv[1], "install") == 0) {
        if (argc >= 3) {
            imagepath = argv[2];
        }
        do_install(imagepath);
    } else {
        fprintf(stderr,
                "Unknown command '%s'. Use 'create' or 'install'.\n",
                argv[1]);
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
