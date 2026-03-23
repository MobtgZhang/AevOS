/*
 * mkfs_aevos — Create an AevOS bootable disk image with GPT, EFI SP, and data partition.
 *
 * Usage: mkfs_aevos -o aevos.img -s 256M -k kernel.elf -b aevos_boot.efi [-m model.gguf]
 *
 * Host Linux tool — uses standard C library.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#define SECTOR_SIZE          512
#define GPT_HEADER_LBA       1
#define GPT_ENTRIES_LBA      2
#define GPT_ENTRY_SIZE       128
#define GPT_ENTRIES_COUNT    128
#define GPT_ENTRIES_SECTORS  ((GPT_ENTRIES_COUNT * GPT_ENTRY_SIZE + SECTOR_SIZE - 1) / SECTOR_SIZE)

#define EFI_PART_START_LBA   2048
#define EFI_PART_SIZE_MB     64
#define AEVOS_MAGIC          0x41494F5346530000ULL   /* "AIOSFS\0\0" */
#define FAT32_SECTORS_PER_CLUSTER 8

#define MBR_SIGNATURE        0xAA55
#define GPT_SIGNATURE        0x5452415020494645ULL    /* "EFI PART" */

typedef struct {
    uint8_t  boot_indicator;
    uint8_t  start_chs[3];
    uint8_t  os_type;
    uint8_t  end_chs[3];
    uint32_t start_lba;
    uint32_t size_lba;
} __attribute__((packed)) mbr_part_entry_t;

typedef struct {
    uint8_t  boot_code[440];
    uint32_t disk_signature;
    uint16_t reserved;
    mbr_part_entry_t partitions[4];
    uint16_t signature;
} __attribute__((packed)) mbr_t;

typedef struct {
    uint64_t signature;
    uint32_t revision;
    uint32_t header_size;
    uint32_t header_crc32;
    uint32_t reserved;
    uint64_t my_lba;
    uint64_t alternate_lba;
    uint64_t first_usable_lba;
    uint64_t last_usable_lba;
    uint8_t  disk_guid[16];
    uint64_t partition_entry_lba;
    uint32_t num_partition_entries;
    uint32_t partition_entry_size;
    uint32_t partition_entries_crc32;
} __attribute__((packed)) gpt_header_t;

typedef struct {
    uint8_t  type_guid[16];
    uint8_t  unique_guid[16];
    uint64_t first_lba;
    uint64_t last_lba;
    uint64_t attributes;
    uint16_t name[36];
} __attribute__((packed)) gpt_entry_t;

typedef struct {
    uint64_t magic;
    uint32_t version;
    uint32_t block_size;
    uint64_t total_blocks;
    uint64_t free_blocks;
    uint64_t root_inode;
    uint64_t inode_count;
    uint8_t  label[64];
    uint8_t  reserved[384];
} __attribute__((packed)) aevosfs_super_t;

/* FAT32 BPB for the EFI System Partition */
typedef struct {
    uint8_t  jmp[3];
    uint8_t  oem_name[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  num_fats;
    uint16_t root_entry_count;
    uint16_t total_sectors_16;
    uint8_t  media_type;
    uint16_t fat_size_16;
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    uint32_t fat_size_32;
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster;
    uint16_t fs_info_sector;
    uint16_t backup_boot_sector;
    uint8_t  reserved1[12];
    uint8_t  drive_number;
    uint8_t  reserved2;
    uint8_t  boot_sig;
    uint32_t volume_serial;
    uint8_t  volume_label[11];
    uint8_t  fs_type[8];
    uint8_t  boot_code[420];
    uint16_t signature;
} __attribute__((packed)) fat32_bpb_t;

typedef struct {
    uint8_t  name[11];
    uint8_t  attr;
    uint8_t  nt_reserved;
    uint8_t  create_time_tenth;
    uint16_t create_time;
    uint16_t create_date;
    uint16_t access_date;
    uint16_t first_cluster_hi;
    uint16_t write_time;
    uint16_t write_date;
    uint16_t first_cluster_lo;
    uint32_t file_size;
} __attribute__((packed)) fat32_dirent_t;

/* ── Utilities ── */

static uint64_t parse_size(const char *s)
{
    char *end;
    uint64_t val = strtoull(s, &end, 10);
    if (*end == 'M' || *end == 'm')
        val *= 1024 * 1024;
    else if (*end == 'G' || *end == 'g')
        val *= 1024 * 1024 * 1024;
    else if (*end == 'K' || *end == 'k')
        val *= 1024;
    return val;
}

static uint32_t crc32_tab[256];
static bool crc32_initialized = false;

static void crc32_init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++) {
            if (c & 1)
                c = 0xEDB88320 ^ (c >> 1);
            else
                c >>= 1;
        }
        crc32_tab[i] = c;
    }
    crc32_initialized = true;
}

static uint32_t crc32_calc(const void *data, size_t len)
{
    if (!crc32_initialized)
        crc32_init();
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++)
        crc = crc32_tab[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFF;
}

static void utf8_to_utf16(const char *src, uint16_t *dst, int max)
{
    int i = 0;
    while (*src && i < max - 1) {
        dst[i++] = (uint16_t)(uint8_t)*src++;
    }
    while (i < max)
        dst[i++] = 0;
}

static void generate_guid(uint8_t guid[16])
{
    static uint32_t seed = 0xDEADBEEF;
    for (int i = 0; i < 16; i++) {
        seed = seed * 1103515245 + 12345;
        guid[i] = (uint8_t)(seed >> 16);
    }
    guid[6] = (guid[6] & 0x0F) | 0x40;
    guid[8] = (guid[8] & 0x3F) | 0x80;
}

/* EFI System Partition type GUID: C12A7328-F81F-11D2-BA4B-00A0C93EC93B */
static const uint8_t ESP_TYPE_GUID[16] = {
    0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
    0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B
};

/* Linux filesystem type GUID for data partition */
static const uint8_t LINUX_FS_GUID[16] = {
    0xAF, 0x3D, 0xC6, 0x0F, 0x83, 0x84, 0x72, 0x47,
    0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4
};

/* ── Disk writing ── */

static FILE *disk_fp;
static uint64_t disk_size;

static bool disk_create(const char *path, uint64_t size)
{
    disk_fp = fopen(path, "wb+");
    if (!disk_fp) {
        fprintf(stderr, "Error: cannot create '%s': %s\n",
                path, strerror(errno));
        return false;
    }
    disk_size = size;

    uint8_t zero[SECTOR_SIZE];
    memset(zero, 0, SECTOR_SIZE);
    uint64_t sectors = size / SECTOR_SIZE;
    for (uint64_t i = 0; i < sectors; i++) {
        if (fwrite(zero, SECTOR_SIZE, 1, disk_fp) != 1) {
            fprintf(stderr, "Error: failed to write sector %lu\n",
                    (unsigned long)i);
            fclose(disk_fp);
            return false;
        }
    }

    return true;
}

static bool disk_write_sector(uint64_t lba, const void *data)
{
    if (fseek(disk_fp, lba * SECTOR_SIZE, SEEK_SET) != 0)
        return false;
    return fwrite(data, SECTOR_SIZE, 1, disk_fp) == 1;
}

static bool disk_write_at(uint64_t offset, const void *data, size_t len)
{
    if (fseek(disk_fp, offset, SEEK_SET) != 0)
        return false;
    return fwrite(data, 1, len, disk_fp) == len;
}

static bool disk_write_file(uint64_t offset, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Error: cannot open '%s': %s\n",
                path, strerror(errno));
        return false;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t buf[4096];
    uint64_t pos = offset;
    long remaining = size;

    while (remaining > 0) {
        size_t chunk = (remaining > (long)sizeof(buf)) ?
                       sizeof(buf) : (size_t)remaining;
        if (fread(buf, 1, chunk, f) != chunk) {
            fprintf(stderr, "Error: reading '%s'\n", path);
            fclose(f);
            return false;
        }
        if (!disk_write_at(pos, buf, chunk)) {
            fclose(f);
            return false;
        }
        pos += chunk;
        remaining -= (long)chunk;
    }

    fclose(f);
    return true;
}

/* ── Protective MBR ── */

static void write_protective_mbr(uint64_t total_sectors)
{
    mbr_t mbr;
    memset(&mbr, 0, sizeof(mbr));

    mbr.partitions[0].os_type   = 0xEE;
    mbr.partitions[0].start_lba = 1;
    if (total_sectors - 1 > 0xFFFFFFFF)
        mbr.partitions[0].size_lba = 0xFFFFFFFF;
    else
        mbr.partitions[0].size_lba = (uint32_t)(total_sectors - 1);
    mbr.partitions[0].start_chs[1] = 0x02;
    mbr.partitions[0].end_chs[0] = 0xFF;
    mbr.partitions[0].end_chs[1] = 0xFF;
    mbr.partitions[0].end_chs[2] = 0xFF;

    mbr.signature = MBR_SIGNATURE;
    disk_write_sector(0, &mbr);
}

/* ── GPT ── */

static void write_gpt(uint64_t total_sectors,
                      uint64_t esp_start, uint64_t esp_end,
                      uint64_t data_start, uint64_t data_end)
{
    gpt_entry_t entries[GPT_ENTRIES_COUNT];
    memset(entries, 0, sizeof(entries));

    /* Partition 1: EFI System Partition */
    memcpy(entries[0].type_guid, ESP_TYPE_GUID, 16);
    generate_guid(entries[0].unique_guid);
    entries[0].first_lba = esp_start;
    entries[0].last_lba  = esp_end;
    entries[0].attributes = 0;
    utf8_to_utf16("EFI System", entries[0].name, 36);

    /* Partition 2: AevOS data */
    memcpy(entries[1].type_guid, LINUX_FS_GUID, 16);
    generate_guid(entries[1].unique_guid);
    entries[1].first_lba = data_start;
    entries[1].last_lba  = data_end;
    entries[1].attributes = 0;
    utf8_to_utf16("AevOS Data", entries[1].name, 36);

    uint32_t entries_crc = crc32_calc(entries, sizeof(entries));

    uint64_t backup_lba = total_sectors - 1;
    uint64_t last_usable = total_sectors - GPT_ENTRIES_SECTORS - 2;
    uint64_t first_usable = GPT_ENTRIES_LBA + GPT_ENTRIES_SECTORS;

    gpt_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.signature             = GPT_SIGNATURE;
    hdr.revision              = 0x00010000;
    hdr.header_size           = 92;
    hdr.my_lba                = GPT_HEADER_LBA;
    hdr.alternate_lba         = backup_lba;
    hdr.first_usable_lba      = first_usable;
    hdr.last_usable_lba       = last_usable;
    generate_guid(hdr.disk_guid);
    hdr.partition_entry_lba    = GPT_ENTRIES_LBA;
    hdr.num_partition_entries  = GPT_ENTRIES_COUNT;
    hdr.partition_entry_size   = GPT_ENTRY_SIZE;
    hdr.partition_entries_crc32 = entries_crc;
    hdr.header_crc32 = 0;
    hdr.header_crc32 = crc32_calc(&hdr, hdr.header_size);

    disk_write_sector(GPT_HEADER_LBA, &hdr);

    uint8_t entry_buf[SECTOR_SIZE];
    const uint8_t *raw = (const uint8_t *)entries;
    for (uint32_t i = 0; i < GPT_ENTRIES_SECTORS; i++) {
        memcpy(entry_buf, raw + i * SECTOR_SIZE, SECTOR_SIZE);
        disk_write_sector(GPT_ENTRIES_LBA + i, entry_buf);
    }

    /* Backup GPT entries at end of disk */
    uint64_t backup_entries_lba = backup_lba - GPT_ENTRIES_SECTORS;
    for (uint32_t i = 0; i < GPT_ENTRIES_SECTORS; i++) {
        memcpy(entry_buf, raw + i * SECTOR_SIZE, SECTOR_SIZE);
        disk_write_sector(backup_entries_lba + i, entry_buf);
    }

    /* Backup GPT header */
    gpt_header_t backup_hdr = hdr;
    backup_hdr.my_lba        = backup_lba;
    backup_hdr.alternate_lba = GPT_HEADER_LBA;
    backup_hdr.partition_entry_lba = backup_entries_lba;
    backup_hdr.header_crc32 = 0;
    backup_hdr.header_crc32 = crc32_calc(&backup_hdr, backup_hdr.header_size);
    disk_write_sector(backup_lba, &backup_hdr);
}

/* ── FAT32 EFI System Partition ── */

static void write_fat32_esp(uint64_t esp_start_lba, uint64_t esp_sectors,
                            const char *boot_efi_path,
                            const char *kernel_path,
                            const char *model_path)
{
    uint64_t esp_offset = esp_start_lba * SECTOR_SIZE;

    uint32_t reserved_sects = 32;
    uint32_t fat_size = (uint32_t)((esp_sectors + 127) / 128);
    if (fat_size < 1) fat_size = 1;

    fat32_bpb_t bpb;
    memset(&bpb, 0, sizeof(bpb));
    bpb.jmp[0] = 0xEB; bpb.jmp[1] = 0x58; bpb.jmp[2] = 0x90;
    memcpy(bpb.oem_name, "AevOS   ", 8);
    bpb.bytes_per_sector      = SECTOR_SIZE;
    bpb.sectors_per_cluster   = FAT32_SECTORS_PER_CLUSTER;
    bpb.reserved_sectors      = reserved_sects;
    bpb.num_fats              = 2;
    bpb.root_entry_count      = 0;
    bpb.total_sectors_16      = 0;
    bpb.media_type            = 0xF8;
    bpb.fat_size_16           = 0;
    bpb.sectors_per_track     = 63;
    bpb.num_heads             = 255;
    bpb.hidden_sectors        = (uint32_t)esp_start_lba;
    bpb.total_sectors_32      = (uint32_t)esp_sectors;
    bpb.fat_size_32           = fat_size;
    bpb.ext_flags             = 0;
    bpb.fs_version            = 0;
    bpb.root_cluster          = 2;
    bpb.fs_info_sector        = 1;
    bpb.backup_boot_sector    = 6;
    bpb.drive_number          = 0x80;
    bpb.boot_sig              = 0x29;
    bpb.volume_serial         = 0xA105DA7A;
    memcpy(bpb.volume_label, "AevOS EFI  ", 11);
    memcpy(bpb.fs_type, "FAT32   ", 8);
    bpb.signature             = 0xAA55;

    disk_write_at(esp_offset, &bpb, sizeof(bpb));

    /* FAT table: cluster 0 and 1 are reserved, cluster 2 = root dir (EOC) */
    uint32_t fat_offset_bytes = (uint32_t)(esp_offset + reserved_sects * SECTOR_SIZE);
    uint32_t fat_entry_0 = 0x0FFFFFF8;
    uint32_t fat_entry_1 = 0x0FFFFFFF;
    uint32_t fat_root    = 0x0FFFFFFF;
    disk_write_at(fat_offset_bytes, &fat_entry_0, 4);
    disk_write_at(fat_offset_bytes + 4, &fat_entry_1, 4);
    disk_write_at(fat_offset_bytes + 8, &fat_root, 4);

    /* Second FAT copy */
    uint32_t fat2_offset = fat_offset_bytes + fat_size * SECTOR_SIZE;
    disk_write_at(fat2_offset, &fat_entry_0, 4);
    disk_write_at(fat2_offset + 4, &fat_entry_1, 4);
    disk_write_at(fat2_offset + 8, &fat_root, 4);

    /* Root directory in cluster 2 */
    uint32_t data_start = reserved_sects + 2 * fat_size;
    uint64_t root_offset = esp_offset + (uint64_t)data_start * SECTOR_SIZE;

    int next_cluster = 3;
    int dir_idx = 0;

    /* Write /EFI/BOOT directory entries */
    fat32_dirent_t volume_label;
    memset(&volume_label, 0, sizeof(volume_label));
    memcpy(volume_label.name, "AevOS EFI  ", 11);
    volume_label.attr = 0x08;
    disk_write_at(root_offset + dir_idx * sizeof(fat32_dirent_t),
                  &volume_label, sizeof(volume_label));
    dir_idx++;

    if (boot_efi_path) {
        fat32_dirent_t entry;
        memset(&entry, 0, sizeof(entry));
        memcpy(entry.name, "BOOTX64 EFI", 11);
        entry.attr = 0x20;
        entry.first_cluster_lo = (uint16_t)(next_cluster & 0xFFFF);
        entry.first_cluster_hi = (uint16_t)((next_cluster >> 16) & 0xFFFF);

        struct stat st;
        if (stat(boot_efi_path, &st) == 0)
            entry.file_size = (uint32_t)st.st_size;

        disk_write_at(root_offset + dir_idx * sizeof(fat32_dirent_t),
                      &entry, sizeof(entry));
        dir_idx++;

        uint64_t file_offset = esp_offset +
            (uint64_t)(data_start + (next_cluster - 2) * FAT32_SECTORS_PER_CLUSTER) *
            SECTOR_SIZE;
        disk_write_file(file_offset, boot_efi_path);

        uint32_t file_clusters = ((uint32_t)st.st_size +
            FAT32_SECTORS_PER_CLUSTER * SECTOR_SIZE - 1) /
            (FAT32_SECTORS_PER_CLUSTER * SECTOR_SIZE);
        for (uint32_t c = 0; c < file_clusters; c++) {
            uint32_t val = (c == file_clusters - 1) ?
                           0x0FFFFFFF : (uint32_t)(next_cluster + c + 1);
            disk_write_at(fat_offset_bytes + (next_cluster + c) * 4,
                          &val, 4);
            disk_write_at(fat2_offset + (next_cluster + c) * 4,
                          &val, 4);
        }
        next_cluster += (int)file_clusters;
    }

    if (kernel_path) {
        fat32_dirent_t entry;
        memset(&entry, 0, sizeof(entry));
        memcpy(entry.name, "KERNEL  ELF", 11);
        entry.attr = 0x20;
        entry.first_cluster_lo = (uint16_t)(next_cluster & 0xFFFF);
        entry.first_cluster_hi = (uint16_t)((next_cluster >> 16) & 0xFFFF);

        struct stat st;
        if (stat(kernel_path, &st) == 0)
            entry.file_size = (uint32_t)st.st_size;

        disk_write_at(root_offset + dir_idx * sizeof(fat32_dirent_t),
                      &entry, sizeof(entry));
        dir_idx++;

        uint64_t file_offset = esp_offset +
            (uint64_t)(data_start + (next_cluster - 2) * FAT32_SECTORS_PER_CLUSTER) *
            SECTOR_SIZE;
        disk_write_file(file_offset, kernel_path);

        uint32_t file_clusters = ((uint32_t)st.st_size +
            FAT32_SECTORS_PER_CLUSTER * SECTOR_SIZE - 1) /
            (FAT32_SECTORS_PER_CLUSTER * SECTOR_SIZE);
        for (uint32_t c = 0; c < file_clusters; c++) {
            uint32_t val = (c == file_clusters - 1) ?
                           0x0FFFFFFF : (uint32_t)(next_cluster + c + 1);
            disk_write_at(fat_offset_bytes + (next_cluster + c) * 4,
                          &val, 4);
            disk_write_at(fat2_offset + (next_cluster + c) * 4,
                          &val, 4);
        }
        next_cluster += (int)file_clusters;
    }

    if (model_path) {
        fat32_dirent_t entry;
        memset(&entry, 0, sizeof(entry));
        memcpy(entry.name, "MODEL   GUF", 11);
        entry.attr = 0x20;
        entry.first_cluster_lo = (uint16_t)(next_cluster & 0xFFFF);
        entry.first_cluster_hi = (uint16_t)((next_cluster >> 16) & 0xFFFF);

        struct stat st;
        if (stat(model_path, &st) == 0)
            entry.file_size = (uint32_t)st.st_size;

        disk_write_at(root_offset + dir_idx * sizeof(fat32_dirent_t),
                      &entry, sizeof(entry));
        dir_idx++;

        uint64_t file_offset = esp_offset +
            (uint64_t)(data_start + (next_cluster - 2) * FAT32_SECTORS_PER_CLUSTER) *
            SECTOR_SIZE;
        disk_write_file(file_offset, model_path);

        uint32_t file_clusters = ((uint32_t)st.st_size +
            FAT32_SECTORS_PER_CLUSTER * SECTOR_SIZE - 1) /
            (FAT32_SECTORS_PER_CLUSTER * SECTOR_SIZE);
        for (uint32_t c = 0; c < file_clusters; c++) {
            uint32_t val = (c == file_clusters - 1) ?
                           0x0FFFFFFF : (uint32_t)(next_cluster + c + 1);
            disk_write_at(fat_offset_bytes + (next_cluster + c) * 4,
                          &val, 4);
            disk_write_at(fat2_offset + (next_cluster + c) * 4,
                          &val, 4);
        }
    }

    /* Write boot.json config */
    const char *boot_json =
        "{\n"
        "  \"kernel\": \"kernel.elf\",\n"
        "  \"n_threads\": 4,\n"
        "  \"use_gpu\": false,\n"
        "  \"target_fps\": 60,\n"
        "  \"screen_width\": 1920,\n"
        "  \"screen_height\": 1080\n"
        "}\n";

    fat32_dirent_t json_entry;
    memset(&json_entry, 0, sizeof(json_entry));
    memcpy(json_entry.name, "BOOT    JSN", 11);
    json_entry.attr = 0x20;
    json_entry.first_cluster_lo = (uint16_t)(next_cluster & 0xFFFF);
    json_entry.first_cluster_hi = (uint16_t)((next_cluster >> 16) & 0xFFFF);
    json_entry.file_size = (uint32_t)strlen(boot_json);
    disk_write_at(root_offset + dir_idx * sizeof(fat32_dirent_t),
                  &json_entry, sizeof(json_entry));

    uint64_t json_offset = esp_offset +
        (uint64_t)(data_start + (next_cluster - 2) * FAT32_SECTORS_PER_CLUSTER) *
        SECTOR_SIZE;
    disk_write_at(json_offset, boot_json, strlen(boot_json));
    uint32_t json_eoc = 0x0FFFFFFF;
    disk_write_at(fat_offset_bytes + next_cluster * 4, &json_eoc, 4);
    disk_write_at(fat2_offset + next_cluster * 4, &json_eoc, 4);
}

/* ── AevOSFS superblock ── */

static void write_aevosfs_super(uint64_t data_start_lba, uint64_t data_sectors)
{
    aevosfs_super_t sb;
    memset(&sb, 0, sizeof(sb));

    sb.magic        = AEVOS_MAGIC;
    sb.version      = 1;
    sb.block_size   = 4096;
    sb.total_blocks = (data_sectors * SECTOR_SIZE) / sb.block_size;
    sb.free_blocks  = sb.total_blocks - 16;
    sb.root_inode   = 1;
    sb.inode_count  = 0;
    memcpy(sb.label, "AevOS-DATA", 10);

    disk_write_at(data_start_lba * SECTOR_SIZE, &sb, sizeof(sb));
}

/* ── Main ── */

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s -o <output.img> -s <size> -k <kernel.elf> "
        "-b <boot.efi> [-m <model.gguf>]\n"
        "\n"
        "Options:\n"
        "  -o  Output disk image path\n"
        "  -s  Disk size (e.g. 256M, 1G)\n"
        "  -k  Kernel ELF binary\n"
        "  -b  UEFI boot application\n"
        "  -m  LLM model file (optional)\n"
        "\n", prog);
}

int main(int argc, char *argv[])
{
    const char *output_path = NULL;
    const char *size_str    = NULL;
    const char *kernel_path = NULL;
    const char *boot_path   = NULL;
    const char *model_path  = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)
            output_path = argv[++i];
        else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc)
            size_str = argv[++i];
        else if (strcmp(argv[i], "-k") == 0 && i + 1 < argc)
            kernel_path = argv[++i];
        else if (strcmp(argv[i], "-b") == 0 && i + 1 < argc)
            boot_path = argv[++i];
        else if (strcmp(argv[i], "-m") == 0 && i + 1 < argc)
            model_path = argv[++i];
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        }
    }

    if (!output_path || !size_str || !kernel_path || !boot_path) {
        usage(argv[0]);
        return 1;
    }

    uint64_t disk_sz = parse_size(size_str);
    if (disk_sz < 128 * 1024 * 1024) {
        fprintf(stderr, "Error: disk size must be at least 128MB\n");
        return 1;
    }

    printf("=== mkfs_aevos — AevOS Disk Image Creator ===\n");
    printf("Output:  %s\n", output_path);
    printf("Size:    %lu bytes (%.1f MB)\n",
           (unsigned long)disk_sz, (double)disk_sz / (1024.0 * 1024.0));
    printf("Kernel:  %s\n", kernel_path);
    printf("Boot:    %s\n", boot_path);
    if (model_path)
        printf("Model:   %s\n", model_path);

    printf("\nCreating disk image...\n");
    if (!disk_create(output_path, disk_sz)) {
        return 1;
    }

    uint64_t total_sectors = disk_sz / SECTOR_SIZE;

    uint64_t esp_start  = EFI_PART_START_LBA;
    uint64_t esp_sectors = (uint64_t)EFI_PART_SIZE_MB * 1024 * 1024 / SECTOR_SIZE;
    uint64_t esp_end    = esp_start + esp_sectors - 1;

    uint64_t data_start = esp_end + 1;
    uint64_t data_end   = total_sectors - GPT_ENTRIES_SECTORS - 2;
    uint64_t data_sectors = data_end - data_start + 1;

    printf("Writing protective MBR...\n");
    write_protective_mbr(total_sectors);

    printf("Writing GPT...\n");
    write_gpt(total_sectors, esp_start, esp_end, data_start, data_end);

    printf("Writing FAT32 ESP (partition 1: LBA %lu-%lu)...\n",
           (unsigned long)esp_start, (unsigned long)esp_end);
    write_fat32_esp(esp_start, esp_sectors,
                    boot_path, kernel_path, model_path);

    printf("Writing AevOSFS superblock (partition 2: LBA %lu-%lu)...\n",
           (unsigned long)data_start, (unsigned long)data_end);
    write_aevosfs_super(data_start, data_sectors);

    fclose(disk_fp);

    printf("\nDisk image '%s' created successfully!\n", output_path);
    printf("  Partition 1: EFI System Partition (FAT32, %u MB)\n",
           EFI_PART_SIZE_MB);
    printf("  Partition 2: AevOS Data (AevOSFS, %lu MB)\n",
           (unsigned long)(data_sectors * SECTOR_SIZE / (1024 * 1024)));

    return 0;
}
