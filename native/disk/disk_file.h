/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Drake Stapleton (AIEN / aien-dev)
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * disk_file.h -- hosted file-backed disk backend for tests (libc). Never part
 * of a freestanding image.
 *
 * Power-cut injection: disk_file_arm_cut(f, n) lets exactly n more logical
 * blocks of writes reach the file (in order, block by block); every later
 * write or flush returns DISK_EPOWER and changes nothing, as if power was
 * lost after block n. disk_file_disarm restores normal operation (the
 * "reboot"). Counters record writes and flushes for tests.
 */
#ifndef AIENOS_DISK_FILE_H
#define AIENOS_DISK_FILE_H

#include "disk.h"

typedef struct {
    int fd;
    uint32_t block_size;
    uint64_t block_count;
    int cut_armed;
    uint64_t cut_remaining; /* blocks still allowed through */
    uint64_t blocks_written, flushes;
} disk_file;

/* Open (create == 1: create/truncate to block_count blocks of zeros) a file
 * as a device of block_size (512 or 4096). Fills *dev. */
int disk_file_open(disk_file *f, disk_dev *dev, const char *path, uint32_t block_size,
                   uint64_t block_count, int create);
void disk_file_close(disk_file *f);
void disk_file_arm_cut(disk_file *f, uint64_t blocks_allowed);
void disk_file_disarm(disk_file *f);

#endif /* AIENOS_DISK_FILE_H */
