# CSE 321 - Operating Systems [Fall 2025]

![Course](https://img.shields.io/badge/Course-CSE%20321-blue)
![Topic](https://img.shields.io/badge/Topic-Metadata%20Journaling-0aa6a6)
![Language](https://img.shields.io/badge/Language-C-00599C?logo=c&logoColor=white)
![Platform](https://img.shields.io/badge/Platform-Linux-lightgrey?logo=linux&logoColor=black)
![License](https://img.shields.io/badge/License-MIT-2ea44f)

A small VSFS-like file-system project that implements **metadata redo journaling** in C. The program logs file-creation metadata updates into a fixed 16-block journal and later installs committed transactions into their home locations on the disk image.

---

## Table of contents

- [Repository highlights](#repository-highlights)
- [Project overview](#project-overview)
- [Disk image layout](#disk-image-layout)
- [Journal format](#journal-format)
- [How to run](#how-to-run)
- [Testing with validator](#testing-with-validator)
- [Viewing the image in Okteta](#viewing-the-image-in-okteta)
- [Project Group Members](#project-group-members)

---

## Repository highlights

- Implements `./journal create <name>` and `./journal install`.
- Uses a 16-block append-only redo journal inside `vsfs.img`.
- Logs full metadata block images as DATA records.
- Uses COMMIT records to seal transactions.
- Replays only committed transactions during install.
- Keeps home metadata unchanged during `create`.
- Supports multiple committed create transactions before `install`, until journal space is full.
- Includes `mkfs`/`mkfs.c` for creating a fresh VSFS image.
- Includes `validator`/`validator.c` for checking file-system consistency.

---

## Project overview

This project works with a pre-built VSFS-like disk image named `vsfs.img`. The goal is to make metadata updates crash-consistent by writing changes to a journal first.

The core idea is:

```text
Describe changes  ->  Commit  ->  Install
```

### `create <name>`

The create command does **not** directly modify the inode bitmap, inode table, or root directory block in their home locations. Instead, it:

1. Reads the current metadata.
2. Finds a free inode.
3. Finds an empty root directory entry slot.
4. Builds updated metadata blocks in memory.
5. Appends DATA records for changed metadata blocks.
6. Appends a COMMIT record.
7. Writes only the journal area back to `vsfs.img`.

### `install`

The install command:

1. Reads the journal sequentially.
2. Groups DATA records until a COMMIT record is found.
3. Replays committed metadata block images to their home block numbers.
4. Ignores incomplete tail transactions.
5. Clears the journal after replay.

---

## Disk image layout

Block size: **4096 bytes**

| Component | Blocks | Block index |
|---|---:|---:|
| Superblock | 1 | 0 |
| Journal | 16 | 1-16 |
| Inode bitmap | 1 | 17 |
| Data bitmap | 1 | 18 |
| Inode table | 2 | 19-20 |
| Data blocks | 64 | 21-84 |

Total blocks: **85**

---

## Journal format

The journal starts at block 1 and spans 16 blocks.

```c
struct journal_header {
    uint32_t magic;        // 0x4A524E4C, "JRNL"
    uint32_t nbytes_used;  // bytes currently used in the journal
};
```

Each journal record starts with:

```c
struct rec_header {
    uint16_t type;  // REC_DATA or REC_COMMIT
    uint16_t size;  // total record size in bytes
};
```

A DATA record stores one complete metadata block image:

```c
struct data_record {
    struct rec_header hdr;
    uint32_t block_no;
    uint8_t data[4096];
};
```

A COMMIT record seals the current transaction:

```c
struct commit_record {
    struct rec_header hdr;
};
```

---

## How to run

Create a fresh VSFS image:

```bash
./mkfs
```

Create a file entry by journaling metadata only:

```bash
./journal create hello.txt
```

At this point, the home metadata blocks are still unchanged. The new metadata exists in the journal as committed redo records.

Install committed journal transactions:

```bash
./journal install
```

After install, the journaled metadata is written to the home inode bitmap, inode table, and root directory block. The journal is then cleared.

---

## Testing with validator

Run the validator on a fresh image:

```bash
./mkfs
./validator
```

Expected output:

```text
Filesystem 'vsfs.img' is consistent.
```

Test one create transaction:

```bash
./journal create hello.txt
./validator
./journal install
./validator
```

Test multiple create transactions before install:

```bash
./mkfs
./journal create a.txt
./journal create b.txt
./journal create c.txt
./journal install
./validator
```

Expected final result: the file system remains consistent and the root directory contains `a.txt`, `b.txt`, and `c.txt`.

---

## Viewing the image in Okteta

Open the disk image:

```bash
okteta vsfs.img
```

Useful offsets:

| Region | Block | Decimal offset | Hex offset |
|---|---:|---:|---:|
| Superblock | 0 | 0 | `0x00000` |
| Journal start | 1 | 4096 | `0x01000` |
| Inode bitmap | 17 | 69632 | `0x11000` |
| Data bitmap | 18 | 73728 | `0x12000` |
| Inode table start | 19 | 77824 | `0x13000` |
| Root directory data block | 21 | 86016 | `0x15000` |

What to check after `create`:

- Go to `0x01000`.
- The first 4 bytes should show the journal magic in little-endian order.
- `nbytes_used` should be larger than 8.
- DATA records should contain full 4096-byte metadata block images.
- A COMMIT record should appear after the DATA records.
- Home metadata at `0x11000`, `0x13000`, and `0x15000` should still show the old state.

What to check after `install`:

- Journal header should remain, but `nbytes_used` should reset to 8.
- The inode bitmap, inode table, and root directory block should now contain the installed metadata updates.

---

## Project Group Members
[Faishal Monir](https://github.com/Faishal-Monir) | [Umma Salma Mim](https://github.com/ummasalmamim)