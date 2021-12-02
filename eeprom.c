//
//    Copyright (C) 2013-2014 Michael Geszkiewicz
//
//    This program is free software; you can redistribute it and/or modify
//    it under the terms of the GNU General Public License as published by
//    the Free Software Foundation; either version 2 of the License, or
//    (at your option) any later version.
//
//    This program is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//    GNU General Public License for more details.
//
//    You should have received a copy of the GNU General Public License
//    along with this program; if not, write to the Free Software
//    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
//

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <strings.h>
#include <openssl/md5.h>
#include "types.h"
#include "eeprom.h"
#include "eeprom_local.h"
#include "eeprom_remote.h"
#include "boards.h"
#include "bitfile.h"

u8 boot_block[BOOT_BLOCK_SIZE] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xAA, 0x99, 0x55, 0x66, 0x31, 0xE1,
    0xFF, 0xFF, 0x32, 0x61, 0x00, 0x00, 0x32, 0x81,
    0x0B, 0x08, 0x32, 0xA1, 0x00, 0x00, 0x32, 0xC1,
    0x0B, 0x01, 0x30, 0xA1, 0x00, 0x00, 0x33, 0x01,
    0x21, 0x00, 0x32, 0x01, 0x00, 0x1F, 0x30, 0xA1,
    0x00, 0x0E, 0x20, 0x00, 0x20, 0x00, 0x20, 0x00,
    0x20, 0x00, 0x20, 0x00, 0x20, 0x00, 0x20, 0x00
};

u8 page_buffer[PAGE_SIZE];
u8 file_buffer[SECTOR_SIZE];

spi_eeprom_dev_t eeprom_access;

char *eeprom_get_flash_type(u8 flash_id) {
    switch (flash_id) {
        case ID_EEPROM_1M:  return "1Mb";
        case ID_EEPROM_2M:  return "2Mb";
        case ID_EEPROM_4M:  return "4Mb";
        case ID_EEPROM_8M:  return "8Mb";
        case ID_EEPROM_16M: return "16Mb";
        case ID_EEPROM_32M: return "32Mb";
        default:            return "unknown";
    }
}

static u32 eeprom_get_flash_size(u8 flash_id) {
    switch (flash_id) {
        case ID_EEPROM_1M:  return 0x100000 / 8;
        case ID_EEPROM_2M:  return 0x200000 / 8;
        case ID_EEPROM_4M:  return 0x400000 / 8;
        case ID_EEPROM_8M:  return 0x800000 / 8;
        case ID_EEPROM_16M: return 0x1000000 / 8;
        case ID_EEPROM_32M: return 0x2000000 / 8;
    }
    return 0;
}

// modify MSB of boot block jmp address to user area
void eeprom_prepare_boot_block(u8 flash_id) {
    switch (flash_id) {
        case ID_EEPROM_1M:  boot_block[25] = 0x01; break;
        case ID_EEPROM_2M:  boot_block[25] = 0x02; break;
        case ID_EEPROM_4M:  boot_block[25] = 0x04; break;
        case ID_EEPROM_8M:  boot_block[25] = 0x08; break;
        case ID_EEPROM_16M: boot_block[25] = 0x10; break;
        case ID_EEPROM_32M: boot_block[25] = 0x20; break;
    }
}

u32 eeprom_calc_user_space(u8 flash_id) {
    switch (flash_id) {
        case ID_EEPROM_1M:  return 0x10000; break;
        case ID_EEPROM_2M:  return 0x20000; break;
        case ID_EEPROM_4M:  return 0x40000; break;
        case ID_EEPROM_8M:  return 0x80000; break;
        case ID_EEPROM_16M: return 0x100000; break;
        case ID_EEPROM_32M: return 0x200000; break;
        default: return 0x80000; break;
    }
}

static int check_boot(llio_t *self) {
    int i;

    eeprom_access.read_page(self, 0x0, &page_buffer);
    for (i = 0; i < BOOT_BLOCK_SIZE; i++) {
        if (boot_block[i] != page_buffer[i]) {
            return -1;
        }
    }
    return 0;
}

static void write_boot(llio_t *self) {
    printf("Erasing sector 0 for boot block\n");
    eeprom_access.erase_sector(self, BOOT_ADDRESS);
    memset(&file_buffer, 0, PAGE_SIZE);
    memcpy(&file_buffer, &boot_block, BOOT_BLOCK_SIZE);
    eeprom_access.write_page(self, 0x0, &file_buffer);
    printf("BootBlock installed\n");
}

int start_programming(llio_t *self, u32 start_address, int fsize) {
    board_t *board = self->board;
    u32 sec_addr;
    int esectors, sector, max_sectors;
    struct timeval tv1, tv2;

    esectors = (fsize - 1) / SECTOR_SIZE;
    if (board->fallback_support == 1) {
        if (start_address == FALLBACK_ADDRESS) {
            max_sectors = eeprom_calc_user_space(board->flash_id) / SECTOR_SIZE - 1;
        } else {
            max_sectors = eeprom_calc_user_space(board->flash_id) / SECTOR_SIZE;
        }
    } else {
        max_sectors = eeprom_get_flash_size(board->flash_id) / SECTOR_SIZE;
    }
    if (esectors > max_sectors) {
        printf("File Size too large to fit\n");
        return -1;
    }
    printf("EEPROM sectors to write: %d, max sectors in area: %d\n", esectors + 1, max_sectors);
    sec_addr = start_address;
    printf("Erasing EEPROM sectors starting from 0x%X...\n", (unsigned int) start_address);
    printf("  |");
    fflush(stdout);
    gettimeofday(&tv1, NULL);
    for (sector = 0; sector <= esectors; sector++) {
        eeprom_access.erase_sector(self, sec_addr);
        sec_addr = sec_addr + SECTOR_SIZE;
        printf("E");
        fflush(stdout);
    }
    if (board->llio.verbose == 1) {
        gettimeofday(&tv2, NULL);
        printf("\n  Erasing time: %.2f seconds", (double) (tv2.tv_usec - tv1.tv_usec) / 1000000 +
         (double) (tv2.tv_sec - tv1.tv_sec));
    }
    printf("\n");
    return 0;
}

int eeprom_write(llio_t *self, char *bitfile_name, u32 start_address, int fix_boot_flag) {
    board_t *board = self->board;
    int bytesread, i;
    u32 eeprom_addr;
    char part_name[32];
    struct stat file_stat;
    FILE *fp;
    struct timeval tv1, tv2;

    if (stat(bitfile_name, &file_stat) != 0) {
        printf("Can't find file %s\n", bitfile_name);
        return -1;
    }

    fp = fopen(bitfile_name, "rb");
    if (fp == NULL) {
        printf("Can't open file %s: %s\n", bitfile_name, strerror(errno));
        return -1;
    }
    if (print_bitfile_header(fp, (char*) &part_name, board->llio.verbose) == -1) {
        fclose(fp);
        return -1;
    }
    if (board->recover == 0) {
        if (strchr(board->llio.fpga_part_number, '|') == NULL) {
            if (strcmp(part_name, board->llio.fpga_part_number) != 0) {
                printf("Error: wrong bitfile destination device: %s, should be %s\n", part_name, board->llio.fpga_part_number);
                fclose(fp);
                return -1;
            }
        }
    }
// if board doesn't support fallback there is no boot block
    if (board->fallback_support == 1) {
        if (check_boot(self) == -1) {
            if (fix_boot_flag) {
                write_boot(self);
                if (check_boot(self) == -1) {
                    printf("Failed to write valid boot sector\n");
                    fclose(fp);
                    return -1;
                }
            } else {
                printf("Error: BootSector is invalid\n");
                fclose(fp);
                return -1;
            }
        } else {
            printf("Boot sector OK\n");
        }
    }

    if (eeprom_access.start_programming(self, start_address, file_stat.st_size) == -1) {
        fclose(fp);
        return -1;
    }
    printf("Programming EEPROM sectors starting from 0x%X...\n", (unsigned int) start_address);
    printf("  |");
    fflush(stdout);
    gettimeofday(&tv1, NULL);
    eeprom_addr = start_address;
    while (!feof(fp)) {
        bytesread = fread(&file_buffer, 1, 8192, fp);
        i = 0;
        while (i < bytesread) {
            eeprom_access.write_page(self, eeprom_addr, &file_buffer[i]);
            i += PAGE_SIZE;
            eeprom_addr += PAGE_SIZE;
        }
        printf("W");
        fflush(stdout);
    }

    fclose(fp);
    printf("\n");
    if (board->llio.verbose == 1) {
        gettimeofday(&tv2, NULL);
        printf("  Programming time: %.2f seconds\n", (double) (tv2.tv_usec - tv1.tv_usec) / 1000000 +
         (double) (tv2.tv_sec - tv1.tv_sec));
    }
    printf("Board configuration updated successfully.\n");
    return 0;
}

int eeprom_verify(llio_t *self, char *bitfile_name, u32 start_address) {
    board_t *board = self->board;
    int bytesread, i, bindex;
    u32 eeprom_addr;
    char part_name[32];
    struct stat file_stat;
    FILE *fp;
    struct timeval tv1, tv2;

    if (stat(bitfile_name, &file_stat) != 0) {
        printf("Can't find file %s\n", bitfile_name);
        return -1;
    }

    fp = fopen(bitfile_name, "rb");
    if (fp == NULL) {
        printf("Can't open file %s: %s\n", bitfile_name, strerror(errno));
        return -1;
    }
    if (print_bitfile_header(fp, (char*) &part_name, board->llio.verbose) == -1) {
        fclose(fp);
        return -1;
    }
    if (strchr(board->llio.fpga_part_number, '|') == NULL) {
        if (strcmp(part_name, board->llio.fpga_part_number) != 0) {
            printf("Error: wrong bitfile destination device: %s, should be %s\n", part_name, board->llio.fpga_part_number);
            fclose(fp);
            return -1;
        }
    }
// if board doesn't support fallback there is no boot block
    if (board->fallback_support == 1) {
        if (check_boot(self) == -1) {
            printf("Error: BootSector is invalid\n");
            fclose(fp);
            return -1;
        } else {
            printf("Boot sector OK\n");
        }
    }

    printf("Verifying EEPROM sectors starting from 0x%X...\n", (unsigned int) start_address);
    printf("  |");
    fflush(stdout);
    gettimeofday(&tv1, NULL);
    eeprom_addr = start_address;
    while (!feof(fp)) {
        bytesread = fread(&file_buffer, 1, 8192, fp);
        bindex = 0;
        while (bindex < bytesread) {
            eeprom_access.read_page(self, eeprom_addr, &page_buffer);
            for (i = 0; i < PAGE_SIZE; i++, bindex++) {
                if (file_buffer[bindex] != page_buffer[i]) {
                   printf("\nError at 0x%X expected: 0x%X but read: 0x%X\n", eeprom_addr + i, file_buffer[bindex], page_buffer[i]);
                   return -1;
                }
            }
            eeprom_addr += PAGE_SIZE;
        }
        printf("V");
        fflush(stdout);
    }

    fclose(fp);
    printf("\n");
    if (board->llio.verbose == 1) {
        gettimeofday(&tv2, NULL);
        printf("  Verification time: %.2f seconds\n", (double) (tv2.tv_usec - tv1.tv_usec) / 1000000 +
         (double) (tv2.tv_sec - tv1.tv_sec));
    }
    printf("Board configuration verified successfully.\n");
    return 0;
}

int flash_backup(llio_t *self, char *bitfile_name) {
    board_t *board = self->board;
    uint i, page_num;
    u32 eeprom_addr, eeprom_pages;
    //char part_name[32];
    //struct stat file_stat;
    FILE *fp;
    struct timeval tv1, tv2;
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char tmp_name[50];
    MD5_CTX md5ctx;
    unsigned char md5out[16];
    char md5str[33];
    char md5file_name[255];

    if (eeprom_get_flash_size(board->flash_id) == 0) {
        printf("Unknown type FLASH memory on the %s board\n", board->llio.board_name);
        return -1;
    }

    printf("Creating backup %s FLASH memory on the %s board\n", eeprom_get_flash_type(board->flash_id), board->llio.board_name);

    if (strcasecmp(bitfile_name,"auto") == 0) {
        strcpy(bitfile_name, board->llio.board_name);
        strftime(tmp_name, sizeof(tmp_name)-1, "_flash_backup_%d%m%y_%H%M%S.bin", t);
        strcat(bitfile_name, tmp_name);
        printf("Used auto naming backup file: '%s'\n", bitfile_name);
    }

    fp = fopen(bitfile_name, "wb");
    if (fp == NULL) {
        printf("Can't open file %s: %s\n", bitfile_name, strerror(errno));
        return -1;
    }

    printf("Starting from 0x0...\n");
    printf("  |");
    fflush(stdout);
    gettimeofday(&tv1, NULL);
    eeprom_addr = 0;
    eeprom_pages = eeprom_get_flash_size(board->flash_id) / PAGE_SIZE;
    page_num = 0;

    MD5_Init(&md5ctx);

    for (i = 0; i < eeprom_pages; i++) {
        eeprom_access.read_page(self, eeprom_addr, &page_buffer);

        fwrite(&page_buffer, 1, PAGE_SIZE, fp);
        MD5_Update(&md5ctx, &page_buffer, PAGE_SIZE);

        eeprom_addr += PAGE_SIZE;
        page_num++;
        if(page_num == 32){
            page_num = 0;
            printf("B");
            fflush(stdout);
        }
    }

    fclose(fp);

    MD5_Final(md5out, &md5ctx);
    for (i = 0; i < 16; i++) {
        sprintf(md5str + i * 2, i < 15 ? "%02x" : "%02x%c", md5out[i],'\0');
    }
    printf("\n");
    if (board->llio.verbose == 1) {
        gettimeofday(&tv2, NULL);
        printf("  Backup time: %.2f seconds\n", (double) (tv2.tv_usec - tv1.tv_usec) / 1000000 +
         (double) (tv2.tv_sec - tv1.tv_sec));
    }
    printf("FLASH memory backup file '%s' created successfully.\n", bitfile_name);

    strcpy(md5file_name, bitfile_name);
    strcat(md5file_name, ".md5");
    fp = fopen(md5file_name, "wb");
    if (fp == NULL) {
        printf("Can't open file '%s': %s\n", md5file_name, strerror(errno));
        return -1;
    }
    fprintf(fp, "%s *%s", md5str, bitfile_name);
    fclose(fp);
    printf("Checksum file '%s' created successfully.\n", md5file_name);
    printf("md5: '%s'\n", md5str);
    return 0;
}

void eeprom_init(llio_t *self) {
    board_t *board = self->board;

    switch (board->flash) {
        case BOARD_FLASH_NONE:
            break;
        case BOARD_FLASH_HM2:
        case BOARD_FLASH_IO:
        case BOARD_FLASH_GPIO:
        case BOARD_FLASH_EPP:
            open_spi_access_local(self);
            break;
        case BOARD_FLASH_REMOTE:
            open_spi_access_remote(self);
            break;
    }
}

void eeprom_cleanup(llio_t *self) {
    board_t *board = self->board;

    switch (board->flash) {
        case BOARD_FLASH_NONE:
            break;
        case BOARD_FLASH_HM2:
        case BOARD_FLASH_IO:
        case BOARD_FLASH_GPIO:
        case BOARD_FLASH_EPP:
            close_spi_access_local(self);
            break;
        case BOARD_FLASH_REMOTE:
            close_spi_access_remote(self);
            break;
    }
}
