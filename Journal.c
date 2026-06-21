// CSE-321 || Semester: Fall 2025
// Metadata journaling for a small VSFS-like file system.

#define _DEFAULT_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>

#define JOURNAL_MAGIC_NUM 0x4A524E4CU   // "JRNL"
#define FS_ID             0x56534653U   // "VSFS"

#define BLOCK_SIZE 4096U
#define INODE_SIZE 128U
#define DIRECT_POINTERS 8U
#define NAME_LEN 28U

#define JOURNAL_START 1U
#define JOURNAL_SIZE 16U
#define INODE_MAP_BLOCK      (JOURNAL_START + JOURNAL_SIZE)
#define DATA_MAP_BLOCK       (INODE_MAP_BLOCK + 1U)
#define INODE_TABLE_START    (DATA_MAP_BLOCK + 1U)
#define INODE_TABLE_BLOCKS   2U
#define DATA_REGION_START    (INODE_TABLE_START + INODE_TABLE_BLOCKS)
#define DATA_REGION_SIZE     64U
#define TOTAL_IMAGE_BLOCKS   (DATA_REGION_START + DATA_REGION_SIZE)

#define RECORD_TYPE_DATA   1U
#define RECORD_TYPE_COMMIT 2U
#define DEFAULT_FILE "vsfs.img"

struct superblock_layout {
    uint32_t magic_value;
    uint32_t block_sz;
    uint32_t total_blocks_count;
    uint32_t inode_total;

    uint32_t journal_start_block;
    uint32_t inode_bitmap_block;
    uint32_t data_bitmap_block;
    uint32_t inode_table_block;
    uint32_t data_region_block;

    uint8_t fill[128 - 9 * 4];
};

struct inode_layout {
    uint16_t file_type;       // 0=free, 1=file, 2=directory
    uint16_t link_count;
    uint32_t file_size;

    uint32_t direct_ptrs[DIRECT_POINTERS];

    uint32_t create_time;
    uint32_t modify_time;

    uint8_t fill_byte[128 - (2 + 2 + 4 + DIRECT_POINTERS * 4 + 4 + 4)];
};

struct dir_entry_layout {
    uint32_t inode_ref;
    char filename[NAME_LEN];
};

_Static_assert(sizeof(struct superblock_layout) == 128, "superblock size mismatch");
_Static_assert(sizeof(struct inode_layout) == 128, "inode size mismatch");
_Static_assert(sizeof(struct dir_entry_layout) == 32, "dirent size mismatch");

struct journal_metadata {
    uint32_t magic_num;
    uint32_t used_space;
};

struct record_metadata {
    uint16_t rec_type;
    uint16_t total_size;
};

struct pending_update {
    uint32_t target_block;
    uint8_t block_content[BLOCK_SIZE];
};

struct metadata_view {
    uint8_t inode_bitmap[BLOCK_SIZE];
    uint8_t data_bitmap[BLOCK_SIZE];
    uint8_t inode_block_a[BLOCK_SIZE];
    uint8_t inode_block_b[BLOCK_SIZE];
    uint8_t root_dir_block[BLOCK_SIZE];
};

static void error_exit(const char *message) {
    perror(message);
    exit(EXIT_FAILURE);
}

static void read_exact_at(int handle, void *buffer, size_t length, off_t position) {
    uint8_t *dst = (uint8_t *)buffer;
    size_t done = 0;

    while (done < length) {
        ssize_t result = pread(handle, dst + done, length - done, position + (off_t)done);
        if (result < 0) {
            if (errno == EINTR) continue;
            error_exit("pread");
        }
        if (result == 0) {
            fprintf(stderr, "Incomplete read: expected %zu bytes, got %zu bytes\n", length, done);
            exit(EXIT_FAILURE);
        }
        done += (size_t)result;
    }
}

static void write_exact_at(int handle, const void *buffer, size_t length, off_t position) {
    const uint8_t *src = (const uint8_t *)buffer;
    size_t done = 0;

    while (done < length) {
        ssize_t result = pwrite(handle, src + done, length - done, position + (off_t)done);
        if (result < 0) {
            if (errno == EINTR) continue;
            error_exit("pwrite");
        }
        if (result == 0) {
            fprintf(stderr, "Incomplete write: expected %zu bytes, wrote %zu bytes\n", length, done);
            exit(EXIT_FAILURE);
        }
        done += (size_t)result;
    }
}

static void read_disk_block(int handle, uint32_t block_number, void *buffer) {
    if (block_number >= TOTAL_IMAGE_BLOCKS) {
        fprintf(stderr, "Invalid block read request: %u\n", block_number);
        exit(EXIT_FAILURE);
    }
    read_exact_at(handle, buffer, BLOCK_SIZE, (off_t)block_number * BLOCK_SIZE);
}

static void write_disk_block(int handle, uint32_t block_number, const void *buffer) {
    if (block_number >= TOTAL_IMAGE_BLOCKS) {
        fprintf(stderr, "Invalid block write request: %u\n", block_number);
        exit(EXIT_FAILURE);
    }
    write_exact_at(handle, buffer, BLOCK_SIZE, (off_t)block_number * BLOCK_SIZE);
}

static int test_bit(const uint8_t *bit_array, uint32_t position) {
    return (bit_array[position / 8] >> (position % 8)) & 0x1;
}

static void set_bit(uint8_t *bit_array, uint32_t position) {
    bit_array[position / 8] |= (uint8_t)(1U << (position % 8));
}

static uint32_t calculate_inode_count(void) {
    return INODE_TABLE_BLOCKS * (BLOCK_SIZE / INODE_SIZE);
}

static size_t journal_max_size(void) {
    return (size_t)JOURNAL_SIZE * (size_t)BLOCK_SIZE;
}

static off_t journal_start_pos(void) {
    return (off_t)JOURNAL_START * (off_t)BLOCK_SIZE;
}

static void read_full_journal(int handle, uint8_t *journal_buffer, size_t capacity) {
    read_exact_at(handle, journal_buffer, capacity, journal_start_pos());
}

static void write_full_journal(int handle, const uint8_t *journal_buffer, size_t capacity) {
    write_exact_at(handle, journal_buffer, capacity, journal_start_pos());
}

static struct journal_metadata *get_journal_header(uint8_t *journal_buffer) {
    return (struct journal_metadata *)journal_buffer;
}

static const struct journal_metadata *get_const_journal_header(const uint8_t *journal_buffer) {
    return (const struct journal_metadata *)journal_buffer;
}

static size_t data_record_total_size(void) {
    return sizeof(struct record_metadata) + sizeof(uint32_t) + BLOCK_SIZE;
}

static size_t commit_record_total_size(void) {
    return sizeof(struct record_metadata);
}

static void initialize_journal_if_needed(uint8_t *journal_buffer, size_t capacity, int *init_flag) {
    struct journal_metadata *jh = get_journal_header(journal_buffer);
    *init_flag = 0;

    if (jh->magic_num != JOURNAL_MAGIC_NUM) {
        memset(journal_buffer, 0, capacity);
        jh->magic_num = JOURNAL_MAGIC_NUM;
        jh->used_space = (uint32_t)sizeof(struct journal_metadata);
        *init_flag = 1;
        return;
    }

    if (jh->used_space < sizeof(struct journal_metadata) || jh->used_space > capacity) {
        fprintf(stderr, "Journal corrupted: used_space=%u\n", jh->used_space);
        exit(EXIT_FAILURE);
    }
}

static void verify_journal_exists(const uint8_t *journal_buffer) {
    const struct journal_metadata *jh = get_const_journal_header(journal_buffer);

    if (jh->magic_num != JOURNAL_MAGIC_NUM) {
        fprintf(stderr, "Journal not found. Run create first.\n");
        exit(EXIT_FAILURE);
    }
    if (jh->used_space < sizeof(struct journal_metadata) || jh->used_space > journal_max_size()) {
        fprintf(stderr, "Journal header invalid.\n");
        exit(EXIT_FAILURE);
    }
}

static void append_to_journal(uint8_t *journal_buffer, size_t capacity, const void *source, size_t bytes) {
    struct journal_metadata *jh = get_journal_header(journal_buffer);

    if ((size_t)jh->used_space + bytes > capacity) {
        fprintf(stderr, "Journal full: need %zu bytes, have %zu bytes. Run install first.\n",
                bytes, capacity - (size_t)jh->used_space);
        exit(EXIT_FAILURE);
    }

    memcpy(journal_buffer + jh->used_space, source, bytes);
    jh->used_space += (uint32_t)bytes;
}

static void append_data_record(uint8_t *journal_buffer, size_t capacity,
                               uint32_t block_num, const uint8_t *block_data) {
    struct record_metadata rh;

    rh.rec_type = (uint16_t)RECORD_TYPE_DATA;
    rh.total_size = (uint16_t)data_record_total_size();

    append_to_journal(journal_buffer, capacity, &rh, sizeof(rh));
    append_to_journal(journal_buffer, capacity, &block_num, sizeof(block_num));
    append_to_journal(journal_buffer, capacity, block_data, BLOCK_SIZE);
}

static void append_commit_record(uint8_t *journal_buffer, size_t capacity) {
    struct record_metadata rh;

    rh.rec_type = (uint16_t)RECORD_TYPE_COMMIT;
    rh.total_size = (uint16_t)commit_record_total_size();

    append_to_journal(journal_buffer, capacity, &rh, sizeof(rh));
}

static void validate_filename(const char *name) {
    if (!name || name[0] == '\0') {
        fprintf(stderr, "Empty name.\n");
        exit(EXIT_FAILURE);
    }
    if (strchr(name, '/')) {
        fprintf(stderr, "Name cannot contain '/'.\n");
        exit(EXIT_FAILURE);
    }
    if (strlen(name) >= NAME_LEN) {
        fprintf(stderr, "Name too long. Maximum length is %u characters.\n", NAME_LEN - 1U);
        exit(EXIT_FAILURE);
    }
}

static uint32_t locate_free_inode(const uint8_t *bitmap, uint32_t total_inodes) {
    for (uint32_t idx = 1; idx < total_inodes; ++idx) {
        if (!test_bit(bitmap, idx)) return idx;
    }

    fprintf(stderr, "No available inodes.\n");
    exit(EXIT_FAILURE);
}

static uint32_t block_containing_inode(uint32_t inode_index) {
    uint32_t inodes_per_block = BLOCK_SIZE / INODE_SIZE;
    return INODE_TABLE_START + (inode_index / inodes_per_block);
}

static uint32_t offset_within_inode_block(uint32_t inode_index) {
    uint32_t inodes_per_block = BLOCK_SIZE / INODE_SIZE;
    return (inode_index % inodes_per_block) * INODE_SIZE;
}

static void write_inode_to_block(uint8_t *block_data, uint32_t inode_index,
                                 const struct inode_layout *inode) {
    uint32_t offset = offset_within_inode_block(inode_index);
    memcpy(block_data + offset, inode, sizeof(*inode));
}

static void read_inode_from_block(const uint8_t *block_data, uint32_t inode_index,
                                  struct inode_layout *out_inode) {
    uint32_t offset = offset_within_inode_block(inode_index);
    memcpy(out_inode, block_data + offset, sizeof(*out_inode));
}

static int find_empty_dir_slot(const uint8_t *dir_block, uint32_t *slot_index) {
    const struct dir_entry_layout *entries = (const struct dir_entry_layout *)dir_block;
    uint32_t entry_count = BLOCK_SIZE / sizeof(struct dir_entry_layout);

    for (uint32_t i = 0; i < entry_count; ++i) {
        if (entries[i].inode_ref == 0 && entries[i].filename[0] == '\0') {
            *slot_index = i;
            return 1;
        }
    }

    return 0;
}

static int filename_exists(const uint8_t *dir_block, const char *name) {
    const struct dir_entry_layout *entries = (const struct dir_entry_layout *)dir_block;
    uint32_t entry_count = BLOCK_SIZE / sizeof(struct dir_entry_layout);

    for (uint32_t i = 0; i < entry_count; ++i) {
        if (entries[i].filename[0] != '\0' && strncmp(entries[i].filename, name, NAME_LEN) == 0) {
            return 1;
        }
    }

    return 0;
}

static void clear_transaction(struct pending_update **list, size_t *count, size_t *capacity) {
    free(*list);
    *list = NULL;
    *count = 0;
    *capacity = 0;
}

static void add_to_transaction(struct pending_update **list, size_t *count, size_t *capacity,
                               uint32_t block_num, const uint8_t *data) {
    if (*count == *capacity) {
        size_t new_cap = (*capacity == 0) ? 8 : *capacity * 2;
        struct pending_update *new_list = realloc(*list, new_cap * sizeof(struct pending_update));
        if (!new_list) error_exit("realloc transaction");
        *list = new_list;
        *capacity = new_cap;
    }

    (*list)[*count].target_block = block_num;
    memcpy((*list)[*count].block_content, data, BLOCK_SIZE);
    (*count)++;
}

static void apply_transaction_to_disk(int handle, const struct pending_update *list, size_t count) {
    for (size_t idx = 0; idx < count; ++idx) {
        if (list[idx].target_block >= TOTAL_IMAGE_BLOCKS) {
            fprintf(stderr, "install: invalid block %u skipped\n", list[idx].target_block);
            continue;
        }
        write_disk_block(handle, list[idx].target_block, list[idx].block_content);
    }
}

static void apply_block_to_view(struct metadata_view *view, uint32_t block_num, const uint8_t *block_data) {
    if (block_num == INODE_MAP_BLOCK) {
        memcpy(view->inode_bitmap, block_data, BLOCK_SIZE);
    } else if (block_num == DATA_MAP_BLOCK) {
        memcpy(view->data_bitmap, block_data, BLOCK_SIZE);
    } else if (block_num == INODE_TABLE_START) {
        memcpy(view->inode_block_a, block_data, BLOCK_SIZE);
    } else if (block_num == INODE_TABLE_START + 1U) {
        memcpy(view->inode_block_b, block_data, BLOCK_SIZE);
    } else if (block_num == DATA_REGION_START) {
        memcpy(view->root_dir_block, block_data, BLOCK_SIZE);
    }
}

static void apply_transaction_to_view(struct metadata_view *view,
                                      const struct pending_update *list, size_t count) {
    for (size_t idx = 0; idx < count; ++idx) {
        apply_block_to_view(view, list[idx].target_block, list[idx].block_content);
    }
}

static size_t find_safe_journal_end(const uint8_t *journal_buffer) {
    const struct journal_metadata *jh = get_const_journal_header(journal_buffer);
    size_t used = (size_t)jh->used_space;
    size_t pos = sizeof(struct journal_metadata);
    size_t last_committed_end = sizeof(struct journal_metadata);

    while (pos + sizeof(struct record_metadata) <= used) {
        struct record_metadata rh;
        memcpy(&rh, journal_buffer + pos, sizeof(rh));

        if (rh.total_size < sizeof(struct record_metadata)) break;
        if (pos + (size_t)rh.total_size > used) break;

        if (rh.rec_type == RECORD_TYPE_DATA) {
            if ((size_t)rh.total_size != data_record_total_size()) break;
        } else if (rh.rec_type == RECORD_TYPE_COMMIT) {
            if ((size_t)rh.total_size != commit_record_total_size()) break;
            last_committed_end = pos + (size_t)rh.total_size;
        } else {
            break;
        }

        pos += (size_t)rh.total_size;
    }

    return last_committed_end;
}

static void replay_committed_journal_to_view(const uint8_t *journal_buffer, struct metadata_view *view) {
    const struct journal_metadata *jh = get_const_journal_header(journal_buffer);
    size_t used = (size_t)jh->used_space;
    size_t pos = sizeof(struct journal_metadata);

    struct pending_update *pending_list = NULL;
    size_t pending_cnt = 0;
    size_t pending_cap = 0;

    while (pos + sizeof(struct record_metadata) <= used) {
        struct record_metadata rh;
        memcpy(&rh, journal_buffer + pos, sizeof(rh));

        if (rh.total_size < sizeof(struct record_metadata)) break;
        if (pos + (size_t)rh.total_size > used) break;

        if (rh.rec_type == RECORD_TYPE_DATA) {
            if ((size_t)rh.total_size != data_record_total_size()) break;

            uint32_t block_num = 0;
            memcpy(&block_num, journal_buffer + pos + sizeof(struct record_metadata), sizeof(block_num));
            const uint8_t *block_data = journal_buffer + pos + sizeof(struct record_metadata) + sizeof(block_num);
            add_to_transaction(&pending_list, &pending_cnt, &pending_cap, block_num, block_data);
        } else if (rh.rec_type == RECORD_TYPE_COMMIT) {
            if ((size_t)rh.total_size != commit_record_total_size()) break;

            apply_transaction_to_view(view, pending_list, pending_cnt);
            clear_transaction(&pending_list, &pending_cnt, &pending_cap);
        } else {
            break;
        }

        pos += (size_t)rh.total_size;
    }

    clear_transaction(&pending_list, &pending_cnt, &pending_cap);
}

static void load_home_metadata(int handle, struct metadata_view *view) {
    read_disk_block(handle, INODE_MAP_BLOCK, view->inode_bitmap);
    read_disk_block(handle, DATA_MAP_BLOCK, view->data_bitmap);
    read_disk_block(handle, INODE_TABLE_START, view->inode_block_a);
    read_disk_block(handle, INODE_TABLE_START + 1U, view->inode_block_b);
    read_disk_block(handle, DATA_REGION_START, view->root_dir_block);
}

static void read_and_validate_superblock(int handle, struct superblock_layout *sb) {
    uint8_t block[BLOCK_SIZE];
    read_disk_block(handle, 0, block);
    memcpy(sb, block, sizeof(*sb));

    if (sb->magic_value != FS_ID ||
        sb->block_sz != BLOCK_SIZE ||
        sb->total_blocks_count != TOTAL_IMAGE_BLOCKS ||
        sb->inode_total != calculate_inode_count() ||
        sb->journal_start_block != JOURNAL_START ||
        sb->inode_bitmap_block != INODE_MAP_BLOCK ||
        sb->data_bitmap_block != DATA_MAP_BLOCK ||
        sb->inode_table_block != INODE_TABLE_START ||
        sb->data_region_block != DATA_REGION_START) {
        fprintf(stderr, "Invalid VSFS image format.\n");
        exit(EXIT_FAILURE);
    }
}

static void execute_install(const char *image_path) {
    int handle = open(image_path, O_RDWR);
    if (handle < 0) error_exit("open image");

    size_t jcap = journal_max_size();
    uint8_t *jbuf = malloc(jcap);
    if (!jbuf) error_exit("malloc journal buffer");

    read_full_journal(handle, jbuf, jcap);
    verify_journal_exists(jbuf);

    const struct journal_metadata *jh = get_const_journal_header(jbuf);
    size_t used = (size_t)jh->used_space;
    size_t pos = sizeof(struct journal_metadata);

    struct pending_update *pending_list = NULL;
    size_t pending_cnt = 0;
    size_t pending_cap = 0;

    while (pos + sizeof(struct record_metadata) <= used) {
        struct record_metadata rh;
        memcpy(&rh, jbuf + pos, sizeof(rh));

        if (rh.total_size < sizeof(struct record_metadata)) break;
        if (pos + (size_t)rh.total_size > used) break;

        if (rh.rec_type == RECORD_TYPE_DATA) {
            if ((size_t)rh.total_size != data_record_total_size()) break;

            uint32_t block_num = 0;
            memcpy(&block_num, jbuf + pos + sizeof(struct record_metadata), sizeof(block_num));
            const uint8_t *block_data = jbuf + pos + sizeof(struct record_metadata) + sizeof(block_num);
            add_to_transaction(&pending_list, &pending_cnt, &pending_cap, block_num, block_data);
        } else if (rh.rec_type == RECORD_TYPE_COMMIT) {
            if ((size_t)rh.total_size != commit_record_total_size()) break;

            apply_transaction_to_disk(handle, pending_list, pending_cnt);
            clear_transaction(&pending_list, &pending_cnt, &pending_cap);
        } else {
            break;
        }

        pos += (size_t)rh.total_size;
    }

    clear_transaction(&pending_list, &pending_cnt, &pending_cap);

    memset(jbuf, 0, jcap);
    struct journal_metadata *new_hdr = get_journal_header(jbuf);
    new_hdr->magic_num = JOURNAL_MAGIC_NUM;
    new_hdr->used_space = (uint32_t)sizeof(struct journal_metadata);
    write_full_journal(handle, jbuf, jcap);

    printf("install: replayed committed transactions and cleared the journal.\n");

    free(jbuf);
    if (close(handle) < 0) error_exit("close image");
}

static void execute_create(const char *image_path, const char *name) {
    validate_filename(name);

    int handle = open(image_path, O_RDWR);
    if (handle < 0) error_exit("open image");

    struct superblock_layout sb;
    read_and_validate_superblock(handle, &sb);

    struct metadata_view current;
    struct metadata_view updated;
    load_home_metadata(handle, &current);

    size_t jcap = journal_max_size();
    uint8_t *jbuf = malloc(jcap);
    if (!jbuf) error_exit("malloc journal buffer");

    read_full_journal(handle, jbuf, jcap);
    int init_status = 0;
    initialize_journal_if_needed(jbuf, jcap, &init_status);

    struct journal_metadata *jhdr = get_journal_header(jbuf);
    size_t safe_end = find_safe_journal_end(jbuf);
    if (safe_end < (size_t)jhdr->used_space) {
        // Do not let a later create accidentally commit a partial tail.
        memset(jbuf + safe_end, 0, jcap - safe_end);
        jhdr->used_space = (uint32_t)safe_end;
    }

    // Build the effective metadata state by replaying already-committed journaled updates
    // in memory only. This allows multiple create commands before install.
    replay_committed_journal_to_view(jbuf, &current);
    memcpy(&updated, &current, sizeof(updated));

    if (filename_exists(updated.root_dir_block, name)) {
        fprintf(stderr, "Entry '%s' already exists.\n", name);
        free(jbuf);
        close(handle);
        exit(EXIT_FAILURE);
    }

    uint32_t inode_cnt = sb.inode_total;
    uint32_t new_inode_idx = locate_free_inode(updated.inode_bitmap, inode_cnt);

    uint32_t free_slot_idx = 0;
    if (!find_empty_dir_slot(updated.root_dir_block, &free_slot_idx)) {
        fprintf(stderr, "No directory slots available.\n");
        free(jbuf);
        close(handle);
        exit(EXIT_FAILURE);
    }

    set_bit(updated.inode_bitmap, new_inode_idx);

    time_t current_time = time(NULL);
    struct inode_layout root_inode;
    read_inode_from_block(updated.inode_block_a, 0, &root_inode);

    if (root_inode.file_type != 2) {
        fprintf(stderr, "Root is not a directory.\n");
        free(jbuf);
        close(handle);
        exit(EXIT_FAILURE);
    }

    if (root_inode.direct_ptrs[0] != DATA_REGION_START) {
        fprintf(stderr, "Unexpected root directory block: %u\n", root_inode.direct_ptrs[0]);
        free(jbuf);
        close(handle);
        exit(EXIT_FAILURE);
    }

    struct inode_layout new_inode;
    memset(&new_inode, 0, sizeof(new_inode));
    new_inode.file_type = 1;
    new_inode.link_count = 1;
    new_inode.file_size = 0;
    new_inode.create_time = (uint32_t)current_time;
    new_inode.modify_time = (uint32_t)current_time;

    uint32_t target_block = block_containing_inode(new_inode_idx);
    if (target_block == INODE_TABLE_START) {
        write_inode_to_block(updated.inode_block_a, new_inode_idx, &new_inode);
    } else if (target_block == INODE_TABLE_START + 1U) {
        write_inode_to_block(updated.inode_block_b, new_inode_idx, &new_inode);
    } else {
        fprintf(stderr, "Unexpected inode block for inode %u.\n", new_inode_idx);
        free(jbuf);
        close(handle);
        exit(EXIT_FAILURE);
    }

    struct dir_entry_layout *dir_entries = (struct dir_entry_layout *)updated.root_dir_block;
    dir_entries[free_slot_idx].inode_ref = new_inode_idx;
    memset(dir_entries[free_slot_idx].filename, 0, NAME_LEN);
    strncpy(dir_entries[free_slot_idx].filename, name, NAME_LEN - 1U);
    dir_entries[free_slot_idx].filename[NAME_LEN - 1U] = '\0';

    root_inode.file_size += sizeof(struct dir_entry_layout);
    root_inode.modify_time = (uint32_t)current_time;
    write_inode_to_block(updated.inode_block_a, 0, &root_inode);

    int inode_bitmap_changed = memcmp(updated.inode_bitmap, current.inode_bitmap, BLOCK_SIZE) != 0;
    int inode_block_a_changed = memcmp(updated.inode_block_a, current.inode_block_a, BLOCK_SIZE) != 0;
    int inode_block_b_changed = memcmp(updated.inode_block_b, current.inode_block_b, BLOCK_SIZE) != 0;
    int root_dir_changed = memcmp(updated.root_dir_block, current.root_dir_block, BLOCK_SIZE) != 0;

    size_t required_space = commit_record_total_size();
    if (inode_bitmap_changed) required_space += data_record_total_size();
    if (inode_block_a_changed) required_space += data_record_total_size();
    if (inode_block_b_changed) required_space += data_record_total_size();
    if (root_dir_changed) required_space += data_record_total_size();

    if ((size_t)jhdr->used_space + required_space > jcap) {
        fprintf(stderr, "Journal full: need %zu bytes, have %zu bytes. Run install first.\n",
                required_space, jcap - (size_t)jhdr->used_space);
        free(jbuf);
        close(handle);
        exit(EXIT_FAILURE);
    }

    if (inode_bitmap_changed) append_data_record(jbuf, jcap, INODE_MAP_BLOCK, updated.inode_bitmap);
    if (inode_block_a_changed) append_data_record(jbuf, jcap, INODE_TABLE_START, updated.inode_block_a);
    if (inode_block_b_changed) append_data_record(jbuf, jcap, INODE_TABLE_START + 1U, updated.inode_block_b);
    if (root_dir_changed) append_data_record(jbuf, jcap, DATA_REGION_START, updated.root_dir_block);
    append_commit_record(jbuf, jcap);

    write_full_journal(handle, jbuf, jcap);

    printf("create: logged metadata for '%s' using inode %u. Home metadata unchanged.\n",
           name, new_inode_idx);

    free(jbuf);
    if (close(handle) < 0) error_exit("close image");
}

static void show_usage(const char *program_name) {
    fprintf(stderr,
            "Usage:\n"
            "  %s create <name>   [default image: " DEFAULT_FILE "]\n"
            "  %s install         [default image: " DEFAULT_FILE "]\n"
            "\n"
            "Set VSFS_IMAGE=/path/to/image to use a different image.\n",
            program_name, program_name);
    exit(EXIT_FAILURE);
}

int main(int argc, char *argv[]) {
    const char *image_file = getenv("VSFS_IMAGE");
    if (!image_file || image_file[0] == '\0') image_file = DEFAULT_FILE;

    if (argc < 2) show_usage(argv[0]);

    if (strcmp(argv[1], "create") == 0) {
        if (argc != 3) show_usage(argv[0]);
        execute_create(image_file, argv[2]);
        return 0;
    }

    if (strcmp(argv[1], "install") == 0) {
        if (argc != 2) show_usage(argv[0]);
        execute_install(image_file);
        return 0;
    }

    show_usage(argv[0]);
    return 1;
}
