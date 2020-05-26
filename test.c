#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <signal.h>

#define CLEAR_REFS_RANGES_IMPL
#include "clear_refs_ranges.h"

#define PAGE_SIZE  (4096)
#define MAX_BLOCKS (10)

int           n_blocks;
void         *blocks[MAX_BLOCKS];
int           num_pages[MAX_BLOCKS];
int           max_num_pages;
struct crr_t  crr;


static void * get_block_addr(char block) {
    int idx;

    idx = block - 'a';

    if (idx > MAX_BLOCKS || idx < 0) {
        printf("  No block labeled '%c'\n", block);
        return NULL;
    }

    return blocks[idx];
}

static int get_addr_range(char block, int page_first, int page_last, void **start, void **end) {
    void *data;


    data = get_block_addr(block);

    if (!data) { return 0; }

    *start = data  +  (page_first * PAGE_SIZE);
    *end   = *start + ((page_last - page_first + 1) * PAGE_SIZE);

    return 1;
}


static void cmd_a(int n_pages) {
    int   size;
    void *data;
    int   i;

    if (n_blocks == MAX_BLOCKS) {
        printf("  Can't allocate any more blocks\n");
        return;
    }

    size = n_pages * 4096;

    data = mmap(NULL,
                size,
                PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS,
                -1,
                0);

    printf("  New block labeled '%c'\n", 'a' + n_blocks);

    num_pages[n_blocks] = n_pages;
    blocks[n_blocks++]  = data;

    if (n_pages > max_num_pages) {
        max_num_pages = n_pages;
    }
}

static void cmd_w(char block, int page_first, int page_last) {
    void *start, *end;

    if (!get_addr_range(block, page_first, page_last, &start, &end)) {
        return;
    }

    for (; start < end; start += 1) {
        *((char*)start) = 'z';
    }

    for (; start < end; start += 1) {
        if (*((char*)start) != 'z') {
            /* that's confusing, but the check helps us force a read */
            *((char*)start) = 'z';
        }
    }
}

static void cmd_ca(void) {
    char buff[256];

    sprintf(buff, "echo 4 > /proc/%d/clear_refs", getpid());

    system(buff);
}

static void cmd_cr(char block, int page_first, int page_last) {
    void *start, *end;
    int   err;

    if (!get_addr_range(block, page_first, page_last, &start, &end)) {
        return;
    }

    if ((err = crr_range(&crr, start, end)) < 0) {
        printf("  crr_range() failed with error %d\n", err);
    }
}

static int get_page_soft_dirty_bit(char block, int page_nr) {
    void               *data;
    unsigned long long  abs_page_nr;
    char                buff[256];
    FILE               *f;
    unsigned long long  map_entry;
    int                 n_read;
    int                 soft_dirty;

    data = get_block_addr(block);
    if (!data) { return -1; }

    abs_page_nr = (((unsigned long long)data) / PAGE_SIZE) + page_nr;

    sprintf(buff, "/proc/%d/pagemap", getpid());

    f = fopen(buff, "r");

    if (!f) {
        printf("  Could not open pagemap file!\n");
        return -1;
    }

    n_read = 0;
    while (n_read < 8) {
        n_read += pread(fileno(f),
                        (&map_entry) + n_read,
                        8 - n_read,
                        (abs_page_nr * 8) + n_read);
    }

    fclose(f);

    return (map_entry >> 55) & 1;
}

static void return_to_std_screen(void) {
    printf("\e[?1049l");
    fflush(stdout);
}

static void sig_int_handler(int junk) {
    return_to_std_screen();
    exit(0);
}

int main(int argc, char **argv) {
    int  err;
    char line[512];
    int  len;
    int  i, j;
    char c;

    if ((err = crr_open(&crr)) < 0) {
        printf("crr_open() failed with error %d\n", err);
        return 1;
    }

    printf("\e[?1049h\e[2J\e[H");
    fflush(stdout);

    signal(SIGINT, sig_int_handler);
    atexit(return_to_std_screen);

    while (printf("> "),
           fflush(stdout),
           fgets(line, sizeof(line), stdin)) {

        len = strlen(line);

        if (len < 1) { goto invalid; }

        switch (line[0]) {
            case 's':
                goto show;

            case 'a':
                if (sscanf(line + 1, "%d", &i) != 1) {
                    goto invalid;
                }
                cmd_a(i);
                goto show;

            case 'w':
                if (sscanf(line + 1, "%c%d,%d", &c, &i, &j) != 3) {
                    goto invalid;
                }
                cmd_w(c, i, j);
                goto show;

            case 'c':
                if (len < 2) { goto invalid; }
                if (line[1] == 'a') {
                    cmd_ca();
                } else if (line[1] == 'r') {
                    if (sscanf(line + 2, "%c%d,%d", &c, &i, &j) != 3) {
                        goto invalid;
                    }
                    cmd_cr(c, i, j);
                } else {
                    goto invalid;
                }
                goto show;

            case 'l':
                printf("\e[?1049h\e[2J\e[H");
                fflush(stdout);
                break;

            case 'q':
                goto out;

show:
            if (n_blocks) {
                printf("  P");
                for (j = 0; j < n_blocks; j += 1) {
                    printf(" %c", 'a' + j);
                }
                printf("\n");
                for (i = 0; i < max_num_pages; i += 1) {
                    printf("%3d ", i);
                    for (j = 0; j < n_blocks; j += 1) {
                        if (num_pages[j] > i) {
                            if (get_page_soft_dirty_bit('a' + j, i)) {
                                printf("\e[0;41m");
                            } else {
                                printf("\e[0;42m");
                            }
                        }
                        printf(" \e[00m ");
                    }
                    printf("\n");
                }
            } else {
                printf("There are no blocks\n");
            }

            break;

            default:
invalid:
                printf("  ?\n");
                printf("  Commands:\n");
                printf("    s      -- show blocks\n");
                printf("    aN     -- allocate a new block with N pages\n");
                printf("    wBF,L  -- write data to pages F through L of block B\n");
                printf("    ca     -- clear all soft-dirty bits in the address space\n");
                printf("    crBF,L -- clear soft-dirty bits of pages F through L of block B\n");
                printf("    l      -- clear the screen\n");
                printf("    q      -- quit\n");
        }
    }

out:
    crr_close(&crr);

    return 0;
}
