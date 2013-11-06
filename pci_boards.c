
#ifdef __linux__
#include <pci/pci.h>
#include <sys/mman.h>
#include <sys/io.h>
#elif _WIN32
#include <windows.h>
#include "libpci/pci.h"
#endif
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include "common.h"
#include "spi_eeprom.h"
#include "bitfile.h"
#include "pci_boards.h"

extern board_t boards[MAX_BOARDS];
extern int boards_count;
static int memfd = -1;
struct pci_access *pacc;
static u8 file_buffer[SECTOR_SIZE];

u16 setup_eeprom_5i20[256] = {
0x9030, // DEVICE ID
0x10B5, // VENDOR ID
0x0290, // PCI STATUS
0x0000, // PCI COMMAND
0x1180, // CLASS CODE (Changed to Data Acq to fool some BIOSs)
0x0000, // CLASS CODE / REV
0x3131, // SUBSYSTEM ID
0x10B5, // SUBSYSTEM VENDOR ID

0x0000, // MSB NEW CAPABILITY POINTER
0x0040, // LSB NEW CAPABILITY POINTER
0x0000, // RESERVED
0x0100, // INTERRUPT PIN
0x0000, // MSW OF POWER MANAGEMENT CAPABILITIES
0x0000, // LSW OF POWER MANAGEMANT CAPABILITIES
0x0000, // MSW OF POWER MANAGEMENT DATA / PMCSR BRIDGE SUPPORT EXTENSION
0x0000, // LSW OF POWER MANAGEMENT CONTROL STATUS

0x0000, // MSW OF HOT SWAP CONTROL / STATUS
0x0000, // LSW OF HOTSWAP NEXT CAPABILITY POINTER / HOT SWAP CONTROL
0x0000, // PCI VITAL PRODUCT DATA ADDRESS
0x0000, // PCI VITAL PRODUCT NEXT CAPABILITY POINTER / PCI VITAL PRODUCT DATA CONTROL
0x0FFF, // MSW OF LOCAL ADDRESS SPACE 0 RANGE
0xFF01, // LSW OF LOCAL ADDRESS SPACE 0 RANGE
0x0FFF, // MSW OF LOCAL ADDRESS SPACE 1 RANGE
0xFF01, // LSW OF LOCAL ADDRESS SPACE 1 RANGE

0x0FFF, // MSW OF LOCAL ADDRESS SPACE 2 RANGE
0x0000, // LSW OF LOCAL ADDRESS SPACE 2 RANGE
0x0FFF, // MSW OF LOCAL ADDRESS SPACE 3 RANGE
0x0000, // LSW OF LOCAL ADDRESS SPACE 3 RANGE
0x0000, // MSW OF EXPANSION ROM RANGE
0x0000, // LSW OF EXPANSION ROM RANGE
0x0000, // MSW OF LOCAL ADDRESS SPACE 0 LOCAL BASE ADDRESS (REMAP)
0x0001, // LSW OF LOCAL ADDRESS SPACE 0 LOCAL BASE ADDRESS (REMAP)

0x0000, // MSW OF LOCAL ADDRESS SPACE 1 LOCAL BASE ADDRESS (REMAP)
0x0001, // LSW OF LOCAL ADDRESS SPACE 1 LOCAL BASE ADDRESS (REMAP)
0x0000, // MSW OF LOCAL ADDRESS SPACE 2 LOCAL BASE ADDRESS (REMAP)
0x0001, // LSW OF LOCAL ADDRESS SPACE 2 LOCAL BASE ADDRESS (REMAP)
0x0000, // MSW OF LOCAL ADDRESS SPACE 3 LOCAL BASE ADDRESS (REMAP)
0x0001, // LSW OF LOCAL ADDRESS SPACE 3 LOCAL BASE ADDRESS (REMAP)
0x0000, // MSW OF EXPANSION ROM LOCAL BASE ADDRESS (REMAP)
0x0000, // LSW OF EXPANSION ROM LOCAL BASE ADDRESS (REMAP)

0x0040, // MSW OF LOCAL ADDRESS SPACE 0 BUS DESCRIPTOR
0x0002, // LSW OF LOCAL ADDRESS SPACE 0 BUS DESCRIPTOR
0x0080, // MSW OF LOCAL ADDRESS SPACE 1 BUS DESCRIPTOR
0x0002, // LSW OF LOCAL ADDRESS SPACE 1 BUS DESCRIPTOR
0x0040, // MSW OF LOCAL ADDRESS SPACE 2 BUS DESCRIPTOR
0x0002, // LSW OF LOCAL ADDRESS SPACE 2 BUS DESCRIPTOR
0x0080, // MSW OF LOCAL ADDRESS SPACE 3 BUS DESCRIPTOR
0x0002, // LSW OF LOCAL ADDRESS SPACE 3 BUS DESCRIPTOR

0x0000, // MSW OF EXPANSION ROM BUS DESCRIPTOR
0x0002, // LSW OF EXPANSION ROM BUS DESCRIPTOR
0x0000, // MSW OF CHIP SELECT 0 BASE ADDRESS
0x0000, // LSW OF CHIP SELECT 0 BASE ADDRESS
0x0000, // MSW OF CHIP SELECT 1 BASE ADDRESS
0x0000, // LSW OF CHIP SELECT 1 BASE ADDRESS
0x0000, // MSW OF CHIP SELECT 2 BASE ADDRESS
0x0000, // LSW OF CHIP SELECT 2 BASE ADDRESS

0x0000, // MSW OF CHIP SELECT 3 BASE ADDRESS
0x0000, // LSW OF CHIP SELECT 3 BASE ADDRESS
0x0000, // SERIAL EEPROM WRITE PROTECTED ADDRESS BOUNDARY
0x0041, // LSW OF INTERRUPT CONTROL / STATUS
0x0878, // MSW OF PCI TARGET RESPONSE, SERIAL EEPROM, AND INITIALIZATION CONTROL
0x0000, // LSW OF PCI TARGET RESPONSE, SERIAL EEPROM, AND INITIALIZATION CONTROL
0x024B, // MSW OF GENERAL PURPOSE I/O CONTROL
0x009B, // LSW OF GENERAL PURPOSE I/O CONTROL
};

void SetCSHigh(board_t *board) {
    u16 data = inw(board->ctrl_base_addr + PLX9030_CTRL_INIT_OFFSET);

    data = data | PLX9030_EECS_MASK;
    outw(data, board->ctrl_base_addr + PLX9030_CTRL_INIT_OFFSET);
    sleep_ns(4000);
}

void SetCSLow(board_t *board) {
    u16 data = inw(board->ctrl_base_addr + PLX9030_CTRL_INIT_OFFSET);

    data = data & (~PLX9030_EECS_MASK);
    outw(data, board->ctrl_base_addr + PLX9030_CTRL_INIT_OFFSET);
    sleep_ns(4000);
}

void SetDinHigh(board_t *board) {
    u16 data = inw(board->ctrl_base_addr + PLX9030_CTRL_INIT_OFFSET);

    data = data | PLX9030_EEDI_MASK;
    outw(data, board->ctrl_base_addr + PLX9030_CTRL_INIT_OFFSET);
    sleep_ns(4000);
}

void SetDinLow(board_t *board) {
    u16 data = inw(board->ctrl_base_addr + PLX9030_CTRL_INIT_OFFSET);

    data = data & (~PLX9030_EEDI_MASK);
    outw(data, board->ctrl_base_addr + PLX9030_CTRL_INIT_OFFSET);
    sleep_ns(4000);
}

void SetClockHigh(board_t *board) {
    u16 data = inw(board->ctrl_base_addr + PLX9030_CTRL_INIT_OFFSET);

    data = data | PLX9030_EECLK_MASK;
    outw(data, board->ctrl_base_addr + PLX9030_CTRL_INIT_OFFSET);
    sleep_ns(4000);
}

void SetClockLow(board_t *board) {
    u16 data = inw(board->ctrl_base_addr + PLX9030_CTRL_INIT_OFFSET);

    data = data & (~PLX9030_EECLK_MASK);
    outw(data, board->ctrl_base_addr + PLX9030_CTRL_INIT_OFFSET);
    sleep_ns(4000);
}

int DataHighQ(board_t *board) {
    u16 data = inw(board->ctrl_base_addr + PLX9030_CTRL_INIT_OFFSET);

    sleep_ns(4000);
    if ((data & PLX9030_EEDO_MASK) != 0)
        return 1;
    else
        return 0;
}

u16 read_eeprom_word(board_t *board, u8 reg) {
    u8 bit;
    u16 mask;
    u16 tdata;

    SetCSLow(board);
    SetDinLow(board);
    SetClockLow(board);
    SetCSHigh(board);
    // send command first
    mask = EEPROM_93C66_CMD_MASK;
    for (bit = 0; bit < EEPROM_93C66_CMD_LEN; bit++) {
        if ((mask & EEPROM_93C66_CMD_READ) == 0)
            SetDinLow(board);
        else
            SetDinHigh(board);
        mask = mask >> 1;
        SetClockLow(board);
        SetClockHigh(board);
    }
    // then send address
    mask = EEPROM_93C66_ADDR_MASK;
    for (bit = 0; bit < EEPROM_93C66_ADDR_LEN; bit++) {
        if ((mask & reg) == 0)
            SetDinLow(board);
        else
            SetDinHigh(board);
        mask = mask >> 1;
        SetClockLow(board);
        SetClockHigh(board);
    }
    // read dummy 0 bit, if zero assume ok
    if (DataHighQ(board) == 1)
        return 0;
    mask = EEPROM_93C66_DATA_MASK;
    tdata = 0;
    for (bit = 0; bit < EEPROM_93C66_DATA_LEN; bit++) {
        SetClockLow(board);
        SetClockHigh(board);
        if (DataHighQ(board) == 1)
            tdata = tdata | mask;
        mask = mask >> 1;
    }
    SetCSLow(board);
    SetDinLow(board);
    SetClockLow(board);
    return tdata;
}

void pci_bridge_eeprom_setup_read(board_t *board) {
    int i;
    char *bridge_name = "Unknown";

    if (board->dev->device_id == DEVICEID_PLX9030)
        bridge_name = "PLX9030";
    else if (board->dev->device_id == DEVICEID_PLX9054)
        bridge_name = "PLX9054";
    else if (board->dev->device_id == DEVICEID_PLX9056)
        bridge_name = "PLX9056";

    printf("%s PCI bridge setup EEPROM:\n", bridge_name);
    for (i = 0; i < EEPROM_93C66_SIZE; i++) {
        if ((i > 0) && ((i % 16) == 0))
            printf("\n");
        if ((i % 16) == 0)
            printf("  %02X: ", i);
        printf("%04X ", read_eeprom_word(board, i));
    }
    printf("\n");
}

static int plx9030_program_fpga(llio_t *self, char *bitfile_name) {
    board_t *board = self->private;
    int bindex, bytesread;
    u32 status, control;
    char part_name[32];
    struct stat file_stat;
    FILE *fp;

    if (stat(bitfile_name, &file_stat) != 0) {
        printf("Can't find file %s\n", bitfile_name);
        return -1;
    }
    fp = fopen(bitfile_name, "rb");
    if (fp == NULL) {
        printf("Can't open file %s: %s\n", bitfile_name, strerror(errno));
        return -1;
    }
    if (print_bitfile_header(fp, (char*) &part_name) == -1) {
        fclose(fp);
        return -1;
    }
    // set /WRITE low for data transfer, and turn on LED
    status = inl(board->ctrl_base_addr + PLX9030_CTRL_STAT_OFFSET);
    control = status & ~PLX9030_WRITE_MASK & ~PLX9030_LED_MASK;
    outl(control, board->ctrl_base_addr + PLX9030_CTRL_STAT_OFFSET);

    printf("Programming FPGA...\n");
    printf("  |");
    fflush(stdout);
    // program the FPGA
    while (!feof(fp)) {
        bytesread = fread(&file_buffer, 1, 8192, fp);
        bindex = 0;
        while (bindex < bytesread) {
            outb(bitfile_reverse_bits(file_buffer[bindex]), board->data_base_addr);
            bindex++;
        }
        printf("W");
        fflush(stdout);
    }

    printf("\n");
    fclose(fp);

    // all bytes transferred, make sure FPGA is all set up now
    status = inl(board->ctrl_base_addr + PLX9030_CTRL_STAT_OFFSET);
    if (!(status & PLX9030_INIT_MASK)) {
    // /INIT goes low on CRC error
        printf("FPGA asserted /INIT: CRC error\n");
        goto fail;
    }
    if (!(status & PLX9030_DONE_MASK)) {
        printf("FPGA did not assert DONE\n");
        goto fail;
    }

    // turn off write enable and LED
    control = status | PLX9030_WRITE_MASK | PLX9030_LED_MASK;
    outl(control, board->ctrl_base_addr + PLX9030_CTRL_STAT_OFFSET);

    return 0;


fail:
    // set /PROGRAM low (reset device), /WRITE high and LED off
    status = inl(board->ctrl_base_addr + PLX9030_CTRL_STAT_OFFSET);
    control = status & ~PLX9030_PROGRAM_MASK;
    control |= PLX9030_WRITE_MASK | PLX9030_LED_MASK;
    outl(control, board->ctrl_base_addr + PLX9030_CTRL_STAT_OFFSET);
    return -EIO;
}

static int plx9030_reset(llio_t *self) {
    board_t *board = self->private;
    u32 status;
    u32 control;

    status = inl(board->ctrl_base_addr + PLX9030_CTRL_STAT_OFFSET);

    // set /PROGRAM bit low to reset the FPGA
    control = status & ~PLX9030_PROGRAM_MASK;

    // set /WRITE and /LED high (idle state)
    control |= PLX9030_WRITE_MASK | PLX9030_LED_MASK;

    // and write it back
    outl(control, board->ctrl_base_addr + PLX9030_CTRL_STAT_OFFSET);

    // verify that /INIT and DONE went low
    status = inl(board->ctrl_base_addr + PLX9030_CTRL_STAT_OFFSET);
    if (status & (PLX9030_DONE_MASK | PLX9030_INIT_MASK)) {
        printf("FPGA did not reset: /INIT = %d, DONE = %d\n",
            (status & PLX9030_INIT_MASK ? 1 : 0),
            (status & PLX9030_DONE_MASK ? 1 : 0)
        );
        return -EIO;
    }

    // set /PROGRAM high, let FPGA come out of reset
    control = status | PLX9030_PROGRAM_MASK;
    outl(control, board->ctrl_base_addr + PLX9030_CTRL_STAT_OFFSET);

    // wait for /INIT to go high when it finishes clearing memory
    // This should take no more than 100uS.  If we assume each PCI read
    // takes 30nS (one PCI clock), that is 3300 reads.  Reads actually
    // take several clocks, but even at a microsecond each, 3.3mS is not
    // an excessive timeout value
    {
        int count = 3300;

        do {
            status = inl(board->ctrl_base_addr + PLX9030_CTRL_STAT_OFFSET);
            if (status & PLX9030_INIT_MASK) break;
        } while (count-- > 0);

        if (count == 0) {
            printf("FPGA did not come out of /INIT\n");
            return -EIO;
        }
    }

    return 0;
}

static void plx9030_fixup_LASxBRD_READY(llio_t *self) {
    board_t *board = self->private;
    int offsets[] = {PLX9030_LAS0BRD_OFFSET, PLX9030_LAS1BRD_OFFSET, PLX9030_LAS2BRD_OFFSET, PLX9030_LAS3BRD_OFFSET};
    int i;

    for (i = 0; i < 4; i ++) {
        u32 val;
        int addr = board->ctrl_base_addr + offsets[i];

        val = inl(addr);
        if (!(val & PLX9030_LASxBRD_READY)) {
            printf("LAS%dBRD #READY is off, enabling now\n", i);
            val |= PLX9030_LASxBRD_READY;
            outl(val, addr);
        }
    }
}

static int plx905x_program_fpga(llio_t *self, char *bitfile_name) {
    board_t *board = self->private;
    int bindex, bytesread, i;
    u32 status;
    char part_name[32];
    struct stat file_stat;
    FILE *fp;

    if (stat(bitfile_name, &file_stat) != 0) {
        printf("Can't find file %s\n", bitfile_name);
        return -1;
    }
    fp = fopen(bitfile_name, "rb");
    if (fp == NULL) {
        printf("Can't open file %s: %s\n", bitfile_name, strerror(errno));
        return -1;
    }
    if (print_bitfile_header(fp, (char*) &part_name) == -1) {
        fclose(fp);
        return -1;
    }
    printf("Programming FPGA...\n");
    printf("  |");
    fflush(stdout);
    // program the FPGA
    while (!feof(fp)) {
        bytesread = fread(&file_buffer, 1, 8192, fp);
        bindex = 0;
        while (bindex < bytesread) {
            outb(bitfile_reverse_bits(file_buffer[bindex]), board->data_base_addr);
            bindex++;
        }
        printf("W");
        fflush(stdout);
    }

    printf("\n");
    fclose(fp);


    // all bytes transferred, make sure FPGA is all set up now
    for (i = 0; i < PLX905X_DONE_WAIT; i++) {
        status = inl(board->ctrl_base_addr + PLX905X_CTRL_STAT_OFFSET);
        if (status & PLX905X_DONE_MASK) break;
    }
    if (i >= PLX905X_DONE_WAIT) {
        printf("Error: Not /DONE; programming not completed.\n");
        return -EIO;
    }

    return 0;
}

static int plx905x_reset(llio_t *self) {
    board_t *board = self->private;
    int i;
    u32 status, control;

    // set GPIO bits to GPIO function
    status = inl(board->ctrl_base_addr + PLX905X_CTRL_STAT_OFFSET);
    control = status | PLX905X_DONE_ENABLE | PLX905X_PROG_ENABLE;
    outl(control, board->ctrl_base_addr + PLX905X_CTRL_STAT_OFFSET);

    // Turn off /PROGRAM bit and insure that DONE isn't asserted
    outl(control & ~PLX905X_PROGRAM_MASK, board->ctrl_base_addr + PLX905X_CTRL_STAT_OFFSET);

    status = inl(board->ctrl_base_addr + PLX905X_CTRL_STAT_OFFSET);
    if (status & PLX905X_DONE_MASK) {
        // Note that if we see DONE at the start of programming, it's most
        // likely due to an attempt to access the FPGA at the wrong I/O
        // location.
        printf("/DONE status bit indicates busy at start of programming\n");
        return -EIO;
    }

    // turn on /PROGRAM output bit
    outl(control | PLX905X_PROGRAM_MASK, board->ctrl_base_addr + PLX905X_CTRL_STAT_OFFSET);

    // Delay for at least 100 uS. to allow the FPGA to finish its reset
    // sequencing.  3300 reads is at least 100 us, could be as long as a
    // few ms
    for (i = 0; i < 3300; i++) {
        status = inl(board->ctrl_base_addr + PLX905X_CTRL_STAT_OFFSET);
    }

    return 0;
}

#ifdef _WIN32
static void pci_fix_bar_lengths(struct pci_dev *dev) {
    int i;

    for (i = 0; i < 6; i++) {
        u32 saved_bar, size;

        if (dev->base_addr == 0)
            continue;

        saved_bar = pci_read_long(dev, PCI_BASE_ADDRESS_0 + i*4);
        pci_write_long(dev, PCI_BASE_ADDRESS_0 + i*4, 0xFFFFFFFF);
        size = pci_read_long(dev, PCI_BASE_ADDRESS_0 + i*4);
        if (size & PCI_BASE_ADDRESS_SPACE_IO)
            size = ~(size & PCI_BASE_ADDRESS_IO_MASK) & 0xFF;
        else
            size = ~(size & PCI_BASE_ADDRESS_MEM_MASK);
        pci_write_long(dev, PCI_BASE_ADDRESS_0 + i*4, saved_bar);

        dev->size[i] = size + 1;
    }
}
#endif

int pci_read(llio_t *self, u32 addr, void *buffer, int size) {
    board_t *board = self->private;

    memcpy(buffer, (board->base + addr), size);
//    printf("READ %X, (%X + %X), %d\n", buffer, board->base, addr, size);
    return 0;
}

int pci_write(llio_t *self, u32 addr, void *buffer, int size) {
    board_t *board = self->private;

    memcpy((board->base + addr), buffer, size);
    return 0;
}

int pci_program_flash(llio_t *self, char *bitfile_name, u32 start_address) {
    return eeprom_write_area(self, bitfile_name, start_address);
}

int pci_verify_flash(llio_t *self, char *bitfile_name, u32 start_address) {
    return eeprom_verify_area(self, bitfile_name, start_address);
}

void pci_boards_init(board_access_t *access) {
    int eno;

    pacc = pci_alloc();
    pci_init(pacc);            // inicjowanie biblioteki libpci

#ifdef __linux__
    if (seteuid(0) != 0) {
        printf("%s need root privilges (or setuid root)", __func__);
        return;
    }
    memfd = open("/dev/mem", O_RDWR);
    eno = errno;
    seteuid(getuid());
    if (memfd < 0) {
        printf("%s can't open /dev/mem: %s", __func__, strerror(eno));
    }
#elif _WIN32
//    init_winio32();
#endif
}

void pci_boards_scan(board_access_t *access) {
    struct pci_dev *dev;
    board_t *board;

    pci_scan_bus(pacc);
    for (dev = pacc->devices; dev != NULL; dev = dev->next) {
        board = &boards[boards_count];
        // first run - fill data struct
        pci_fill_info(dev, PCI_FILL_IDENT | PCI_FILL_IRQ | PCI_FILL_BASES | PCI_FILL_ROM_BASE | PCI_FILL_SIZES | PCI_FILL_CLASS);

        if (dev->vendor_id == VENDORID_MESAPCI) {
            if (dev->device_id == DEVICEID_MESA4I74) {
                board->type = BOARD_PCI;
                strncpy((char *) board->llio.board_name, "4I74", 4);
                board->llio.num_ioport_connectors = 3;
                board->llio.pins_per_connector = 24;
                board->llio.ioport_connector_name[0] = "P1";
                board->llio.ioport_connector_name[1] = "P3";
                board->llio.ioport_connector_name[2] = "P4";
                board->llio.fpga_part_number = "6slx9pq144";
                board->llio.num_leds = 0;
                board->llio.read = &pci_read;
                board->llio.write = &pci_write;
                board->llio.program_flash = &pci_program_flash;
                board->llio.verify_flash = &pci_verify_flash;
                board->llio.private = board;
#ifdef __linux__
                iopl(3);
                board->len = dev->size[0];
                board->base = mmap(0, board->len, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, dev->base_addr[0]);
#elif _WIN32
                pci_fix_bar_lengths(dev);
                board->len = dev->size[0];
                board->base = map_memory(dev->base_addr[0], board->len);
#endif
                board->dev = dev;
                eeprom_init(&(board->llio));
                board->flash_id = read_flash_id(&(board->llio));
                board->flash_start_address = eeprom_calc_user_space(board->flash_id);
                board->llio.verbose = access->verbose;

                boards_count++;
            } else if (dev->device_id == DEVICEID_MESA5I25) {
                board->type = BOARD_PCI;
                strncpy((char *) board->llio.board_name, "5I25", 4);
                board->llio.num_ioport_connectors = 2;
                board->llio.pins_per_connector = 17;
                board->llio.ioport_connector_name[0] = "P3";
                board->llio.ioport_connector_name[1] = "P2";
                board->llio.fpga_part_number = "6slx9pq144";
                board->llio.num_leds = 2;
                board->llio.read = &pci_read;
                board->llio.write = &pci_write;
                board->llio.program_flash = &pci_program_flash;
                board->llio.verify_flash = &pci_verify_flash;
                board->llio.private = board;
#ifdef __linux__
                iopl(3);
                board->len = dev->size[0];
                board->base = mmap(0, board->len, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, dev->base_addr[0]);
#elif _WIN32
                pci_fix_bar_lengths(dev);
                board->len = dev->size[0];
                board->base = map_memory(dev->base_addr[0], board->len);
#endif
                board->dev = dev;
                eeprom_init(&(board->llio));
                board->flash_id = read_flash_id(&(board->llio));
                board->flash_start_address = eeprom_calc_user_space(board->flash_id);
                hm2_read_idrom(&(board->llio));
                board->llio.verbose = access->verbose;

                boards_count++;
            } else if (dev->device_id == DEVICEID_MESA6I25) {
                board->type = BOARD_PCI;
                strncpy(board->llio.board_name, "6I25", 4);
                board->llio.num_ioport_connectors = 2;
                board->llio.pins_per_connector = 17;
                board->llio.ioport_connector_name[0] = "P3";
                board->llio.ioport_connector_name[1] = "P2";
                board->llio.fpga_part_number = "6slx9pq144";
                board->llio.num_leds = 2;
                board->llio.read = &pci_read;
                board->llio.write = &pci_write;
                board->llio.program_flash = &pci_program_flash;
                board->llio.verify_flash = &pci_verify_flash;
                board->llio.private = board;
#ifdef __linux__
                iopl(3);
                board->len = dev->size[0];
                board->base = mmap(0, board->len, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, dev->base_addr[0]);
#elif _WIN32
                pci_fix_bar_lengths(dev);
                board->len = dev->size[0];
                board->base = map_memory(dev->base_addr[0], board->len);
#endif
                board->dev = dev;
                board->flash_id = read_flash_id(&(board->llio));
                board->flash_start_address = eeprom_calc_user_space(board->flash_id);
                board->llio.verbose = access->verbose;

                boards_count++;
            }
        } else if (dev->device_id == DEVICEID_PLX9030) {
            u16 ssid = pci_read_word(dev, PCI_SUBSYSTEM_ID);
            if (ssid == SUBDEVICEID_MESA5I20) {
                board->type = BOARD_PCI;
                strncpy(board->llio.board_name, "5I20", 4);
                board->llio.num_ioport_connectors = 3;
                board->llio.pins_per_connector = 24;
                board->llio.ioport_connector_name[0] = "P2";
                board->llio.ioport_connector_name[1] = "P3";
                board->llio.ioport_connector_name[2] = "P4";
                board->llio.fpga_part_number = "2s200pq208";
                board->llio.num_leds = 8;
                board->llio.read = &pci_read;
                board->llio.write = &pci_write;
                board->llio.program_fpga = &plx9030_program_fpga;
                board->llio.reset = &plx9030_reset;
                board->llio.private = board;
#ifdef __linux__
                iopl(3);
                board->len = dev->size[5];
                board->base = mmap(0, board->len, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, dev->base_addr[5]);
#elif _WIN32
                pci_fix_bar_lengths(dev);
                board->len = dev->size[5];
                board->base = map_memory(dev->base_addr[5], board->len);
#endif
                board->ctrl_base_addr = dev->base_addr[1];
                board->data_base_addr = dev->base_addr[2];
                board->dev = dev;
                board->llio.verbose = access->verbose;

                boards_count++;
            } else if (ssid == SUBDEVICEID_MESA4I65) {
                board->type = BOARD_PCI;
                strncpy(board->llio.board_name, "4I65", 4);
                board->llio.num_ioport_connectors = 3;
                board->llio.pins_per_connector = 24;
                board->llio.ioport_connector_name[0] = "P1";
                board->llio.ioport_connector_name[1] = "P3";
                board->llio.ioport_connector_name[2] = "P4";
                board->llio.fpga_part_number = "2s200pq208";
                board->llio.num_leds = 8;
                board->llio.read = &pci_read;
                board->llio.write = &pci_write;
                board->llio.program_fpga = &plx9030_program_fpga;
                board->llio.reset = &plx9030_reset;
                board->llio.private = board;
#ifdef __linux__
                iopl(3);
                board->len = dev->size[5];
                board->base = mmap(0, board->len, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, dev->base_addr[5]);
#elif _WIN32
                pci_fix_bar_lengths(dev);
                board->len = dev->size[5];
                board->base = map_memory(dev->base_addr[5], board->len);
#endif
                board->ctrl_base_addr = dev->base_addr[1];
                board->data_base_addr = dev->base_addr[2];
                board->dev = dev;
                board->llio.verbose = access->verbose;

                boards_count++;
            }
        } else if (dev->device_id == DEVICEID_PLX9054) {
            u16 ssid = pci_read_word(dev, PCI_SUBSYSTEM_ID);
            if ((ssid == SUBDEVICEID_MESA4I68_OLD) || (ssid == SUBDEVICEID_MESA4I68)) {
                board->type = BOARD_PCI;
                strncpy(board->llio.board_name, "4I68", 4);
                board->llio.num_ioport_connectors = 3;
                board->llio.pins_per_connector = 24;
                board->llio.ioport_connector_name[0] = "P1";
                board->llio.ioport_connector_name[1] = "P2";
                board->llio.ioport_connector_name[2] = "P4";
                board->llio.fpga_part_number = "3s400pq208";
                board->llio.num_leds = 4;
                board->llio.read = &pci_read;
                board->llio.write = &pci_write;
                board->llio.program_fpga = &plx905x_program_fpga;
                board->llio.reset = &plx905x_reset;
                board->llio.private = board;
#ifdef __linux__
                iopl(3);
                board->len = dev->size[3];
                board->base = mmap(0, board->len, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, dev->base_addr[3]);
#elif _WIN32
                pci_fix_bar_lengths(dev);
                board->len = dev->size[3];
                board->base = map_memory(dev->base_addr[3], board->len);
#endif
                board->ctrl_base_addr = dev->base_addr[1];
                board->data_base_addr = dev->base_addr[2];
                board->dev = dev;
                board->llio.verbose = access->verbose;

                boards_count++;
            } else if (ssid == SUBDEVICEID_MESA5I21) {
                board->type = BOARD_PCI;
                strncpy(board->llio.board_name, "5I21", 4);
                board->llio.num_ioport_connectors = 2;
                board->llio.pins_per_connector = 32;
                board->llio.ioport_connector_name[0] = "P1";
                board->llio.ioport_connector_name[1] = "P1";
                board->llio.fpga_part_number = "3s400pq208";
                board->llio.num_leds = 8;
                board->llio.read = &pci_read;
                board->llio.write = &pci_write;
                board->llio.program_fpga = &plx905x_program_fpga;
                board->llio.reset = &plx905x_reset;
                board->llio.private = board;
#ifdef __linux__
                iopl(3);
                board->len = dev->size[3];
                board->base = mmap(0, board->len, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, dev->base_addr[3]);
#elif _WIN32
                pci_fix_bar_lengths(dev);
                board->len = dev->size[3];
                board->base = map_memory(dev->base_addr[3], board->len);
#endif
                board->ctrl_base_addr = dev->base_addr[1];
                board->data_base_addr = dev->base_addr[2];
                board->dev = dev;
                board->llio.verbose = access->verbose;

                boards_count++;
            } else if ((ssid == SUBDEVICEID_MESA5I22_10) || (ssid == SUBDEVICEID_MESA5I22_15)) {
                board->type = BOARD_PCI;
                strncpy(board->llio.board_name, "5I22", 4);
                board->llio.num_ioport_connectors = 4;
                board->llio.pins_per_connector = 24;
                board->llio.ioport_connector_name[0] = "P2";
                board->llio.ioport_connector_name[1] = "P3";
                board->llio.ioport_connector_name[2] = "P4";
                board->llio.ioport_connector_name[3] = "P5";
                if (ssid == SUBDEVICEID_MESA5I22_10) {
                    board->llio.fpga_part_number = "3s1000fg320";
                } else if (ssid == SUBDEVICEID_MESA5I22_15) {
                    board->llio.fpga_part_number = "3s1500fg320";
                }
                board->llio.num_leds = 8;
                board->llio.read = &pci_read;
                board->llio.write = &pci_write;
                board->llio.program_fpga = &plx905x_program_fpga;
                board->llio.reset = &plx905x_reset;
                board->llio.private = board;
#ifdef __linux__
                iopl(3);
                board->len = dev->size[3];
                board->base = mmap(0, board->len, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, dev->base_addr[3]);
#elif _WIN32
                pci_fix_bar_lengths(dev);
                board->len = dev->size[3];
                board->base = map_memory(dev->base_addr[3], board->len);
#endif
                board->ctrl_base_addr = dev->base_addr[1];
                board->data_base_addr = dev->base_addr[2];
                board->dev = dev;
                board->llio.verbose = access->verbose;

                boards_count++;
            } else if (ssid == SUBDEVICEID_MESA5I23) {
                board->type = BOARD_PCI;
                strncpy(board->llio.board_name, "5I23", 4);
                board->llio.num_ioport_connectors = 3;
                board->llio.pins_per_connector = 24;
                board->llio.ioport_connector_name[0] = "P2";
                board->llio.ioport_connector_name[1] = "P3";
                board->llio.ioport_connector_name[2] = "P4";
                board->llio.fpga_part_number = "3s400pq208";
                board->llio.num_leds = 2;
                board->llio.read = &pci_read;
                board->llio.write = &pci_write;
                board->llio.program_fpga = &plx905x_program_fpga;
                board->llio.reset = &plx905x_reset;
                board->llio.private = board;
#ifdef __linux__
                iopl(3);
                board->len = dev->size[3];
                board->base = mmap(0, board->len, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, dev->base_addr[3]);
#elif _WIN32
                pci_fix_bar_lengths(dev);
                board->len = dev->size[3];
                board->base = map_memory(dev->base_addr[3], board->len);
#endif
                board->ctrl_base_addr = dev->base_addr[1];
                board->data_base_addr = dev->base_addr[2];
                board->dev = dev;
                board->llio.verbose = access->verbose;

                boards_count++;
            } else if ((ssid == SUBDEVICEID_MESA4I69_16) || (ssid == SUBDEVICEID_MESA4I69_25)) {
                board->type = BOARD_PCI;
                strncpy(board->llio.board_name, "4I69", 4);
                board->llio.num_ioport_connectors = 3;
                board->llio.pins_per_connector = 24;
                board->llio.ioport_connector_name[0] = "P1";
                board->llio.ioport_connector_name[1] = "P3";
                board->llio.ioport_connector_name[2] = "P4";
                if (ssid == SUBDEVICEID_MESA4I69_16) {
                    board->llio.fpga_part_number = "6slx16fg256";
                } else if (ssid == SUBDEVICEID_MESA4I69_25) {
                    board->llio.fpga_part_number = "6slx25fg256";
                }
                board->llio.num_leds = 8;
                board->llio.read = &pci_read;
                board->llio.write = &pci_write;
                board->llio.program_fpga = &plx905x_program_fpga;
                board->llio.reset = &plx905x_reset;
                board->llio.private = board;
#ifdef __linux__
                iopl(3);
                board->len = dev->size[3];
                board->base = mmap(0, board->len, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, dev->base_addr[3]);
#elif _WIN32
                pci_fix_bar_lengths(dev);
                board->len = dev->size[3];
                board->base = map_memory(dev->base_addr[3], board->len);
#endif
                board->ctrl_base_addr = dev->base_addr[1];
                board->data_base_addr = dev->base_addr[2];
                board->dev = dev;
                board->llio.verbose = access->verbose;

                boards_count++;
            }
        } else if (dev->device_id == DEVICEID_PLX9056) {
            u16 ssid = pci_read_word(dev, PCI_SUBSYSTEM_ID);
            if ((ssid == SUBDEVICEID_MESA3X20_10) || (ssid == SUBDEVICEID_MESA3X20_15) || (ssid == SUBDEVICEID_MESA3X20_20)) {
                board->type = BOARD_PCI;
                strncpy(board->llio.board_name, "3x20", 4);
                board->llio.num_ioport_connectors = 6;
                board->llio.pins_per_connector = 24;
                board->llio.ioport_connector_name[0] = "P4";
                board->llio.ioport_connector_name[1] = "P5";
                board->llio.ioport_connector_name[2] = "P6";
                board->llio.ioport_connector_name[3] = "P9";
                board->llio.ioport_connector_name[4] = "P6";
                board->llio.ioport_connector_name[5] = "P7";
                if (ssid == SUBDEVICEID_MESA3X20_10) {
                    board->llio.fpga_part_number = "3s1000fg456";
                } else if (ssid == SUBDEVICEID_MESA3X20_15) {
                    board->llio.fpga_part_number = "3s1500fg456";
                } else if (ssid == SUBDEVICEID_MESA3X20_20) {
                    board->llio.fpga_part_number = "3s2000fg456";
                }
                board->llio.num_leds = 0;
                board->llio.read = &pci_read;
                board->llio.write = &pci_write;
                board->llio.program_fpga = &plx905x_program_fpga;
                board->llio.reset = &plx905x_reset;
                board->llio.private = board;
#ifdef __linux__
                iopl(3);
                board->len = dev->size[3];
                board->base = mmap(0, board->len, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, dev->base_addr[3]);
#elif _WIN32
                pci_fix_bar_lengths(dev);
                board->len = dev->size[3];
                board->base = map_memory(dev->base_addr[3], board->len);
#endif
                board->ctrl_base_addr = dev->base_addr[1];
                board->data_base_addr = dev->base_addr[2];
                board->dev = dev;
                board->llio.verbose = access->verbose;

                boards_count++;
            }
        }
    }
}

void pci_print_info(board_t *board) {
    int i;

    printf("\nPCI device %s at %02X:%02X.%X (%04X:%04X)\n", board->llio.board_name, 
        board->dev->bus, board->dev->dev, board->dev->func, board->dev->vendor_id, board->dev->device_id);
    if (board->llio.verbose == 0)
        return;

    for (i = 0; i < 6; i++) {
        u32 flags = pci_read_long(board->dev, PCI_BASE_ADDRESS_0 + 4*i);
        if (board->dev->base_addr[i] != 0) {
            if (flags & PCI_BASE_ADDRESS_SPACE_IO) {
                printf("  Region %d: I/O at %04X [size=%04X]\n", i, (unsigned int) (board->dev->base_addr[i] & PCI_BASE_ADDRESS_IO_MASK), (unsigned int) board->dev->size[i]);
            }  else {
                printf("  Region %d: Memory at %08X [size=%08X]\n", i, (unsigned int) board->dev->base_addr[i], (unsigned int) board->dev->size[i]);
            }
        }
    }
    if (board->flash_id > 0) {
        printf("  Flash size: %s (id: 0x%02X)\n", eeprom_get_flash_type(board->flash_id), board->flash_id);
    }
}
