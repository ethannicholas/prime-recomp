// GameCube memory card (EXI device) backed by a raw image file.
#include "../runtime.h"
#include "../platform.h"
#include "memcard.h"
#include <cstdio>

enum : uint8_t {
    CMD_ID = 0x00, CMD_READ_ARRAY = 0x52, CMD_SET_INT = 0x81, CMD_READ_STATUS = 0x83, CMD_READ_ID = 0x85,
    CMD_WAKE = 0x87, CMD_SLEEP = 0x88, CMD_CLEAR_STATUS = 0x89, CMD_SECTOR_ERASE = 0xF1,
    CMD_PAGE_PROGRAM = 0xF2, CMD_CHIP_ERASE = 0xF4,
};
enum : uint8_t { ST_BUSY = 0x80, ST_UNLOCKED = 0x40, ST_SLEEP = 0x20, ST_ERASE_ERR = 0x10, ST_PROG_ERR = 0x08, ST_READY = 0x01 };

MemCard::MemCard(const std::string& path, uint32_t size_mbit) : path_(path), size_mbit_(size_mbit) {
    data_.assign((size_t)size_mbit * 1024 * 1024 / 8, 0xFF);
    if (FILE* f = fopen(path.c_str(), "rb")) {
        size_t n = fread(data_.data(), 1, data_.size(), f);
        fclose(f);
        LOG(LOG_EXI, "memcard: loaded %s (%zu bytes)", path.c_str(), n);
    } else {
        LOG(LOG_EXI, "memcard: creating formatted card %s", path.c_str());
        format();
        dirty_ = true;
        flush();
    }
    status_ = ST_UNLOCKED | ST_READY;
}

static void put16(uint8_t* p, uint16_t v) { p[0] = v >> 8; p[1] = (uint8_t)v; }

// CARD library checksum: 16-bit sums of big-endian words (0xFFFF folds to 0).
static void card_checksum(const uint8_t* p, size_t len, uint16_t& cs, uint16_t& csi) {
    cs = csi = 0;
    for (size_t i = 0; i + 1 < len; i += 2) {
        uint16_t w = (uint16_t)((p[i] << 8) | p[i + 1]);
        cs += w;
        csi += (uint16_t)~w;
    }
    if (cs == 0xFFFF) cs = 0;
    if (csi == 0xFFFF) csi = 0;
}

// Equivalent of CARDFormat with formatTime = 0 and an all-zero SRAM flash ID,
// which makes the serial all zeros (see VerifyID in the CARD library).
void MemCard::format() {
    const size_t B = 0x2000;
    memset(data_.data(), 0xFF, data_.size());
    uint16_t cs, csi;
    // Block 0: header
    uint8_t* h = &data_[0];
    memset(h, 0, 0x26);
    put16(h + 0x20, 0);                      // device ID
    put16(h + 0x22, (uint16_t)size_mbit_);   // size in Mbit
    put16(h + 0x24, 0);                      // encoding: ANSI
    card_checksum(h, 0x1FC, cs, csi);
    put16(h + 0x1FC, cs);
    put16(h + 0x1FE, csi);
    // Blocks 1-2: directory (127 empty entries)
    for (int k = 0; k < 2; k++) {
        uint8_t* d = &data_[B * (1 + k)];
        memset(d, 0xFF, B);
        put16(d + 0x1FFA, (uint16_t)k);  // update counter
        card_checksum(d, 0x1FFC, cs, csi);
        put16(d + 0x1FFC, cs);
        put16(d + 0x1FFE, csi);
    }
    // Blocks 3-4: block allocation table
    uint16_t total = (uint16_t)(data_.size() / B);
    for (int k = 0; k < 2; k++) {
        uint8_t* b = &data_[B * (3 + k)];
        memset(b, 0, B);
        put16(b + 0x04, (uint16_t)k);        // update counter
        put16(b + 0x06, (uint16_t)(total - 5));  // free blocks
        put16(b + 0x08, 4);                  // last allocated
        card_checksum(b + 4, B - 4, cs, csi);
        put16(b + 0x00, cs);
        put16(b + 0x02, csi);
    }
}

void MemCard::flush() {
    if (!dirty_) return;
    size_t slash = path_.find_last_of("/\\");
    if (slash != std::string::npos) plat_make_dirs(path_.substr(0, slash));
    std::string tmp = path_ + ".tmp";
    if (FILE* f = fopen(tmp.c_str(), "wb")) {
        fwrite(data_.data(), 1, data_.size(), f);
        fclose(f);
        // Windows' rename() fails if the destination exists, so go through the
        // platform layer, which replaces it.
        plat_replace_file(tmp.c_str(), path_.c_str());
        dirty_ = false;
    } else {
        // This used to do nothing at all when the path was not writable, which is how an
        // app with "/" for a working directory came up with a blank card every launch.
        static bool warned = false;
        if (!warned) {
            warned = true;
            LOG(LOG_EXI, "memcard: cannot write %s -- saves will not persist", tmp.c_str());
        }
    }
}

void MemCard::select() {
    pos_ = 0;
    cmd_ = 0;
    addr_ = 0;
    prog_.clear();
}

bool MemCard::deselect() {
    bool irq = false;
    if (pos_) LOG(LOG_EXI, "memcard cmd %02X bytes=%u addr=%05X", cmd_, pos_, addr_);
    switch (cmd_) {
    case CMD_SECTOR_ERASE:
        if (pos_ >= 3) {
            uint32_t a = addr_ & ~0x1FFFu & (uint32_t)(data_.size() - 1);
            memset(&data_[a], 0xFF, 0x2000);
            dirty_ = true;
            irq = true;
        }
        break;
    case CMD_PAGE_PROGRAM:
        if (pos_ >= 5 && !prog_.empty()) {
            uint32_t a = addr_ & (uint32_t)(data_.size() - 1);
            for (size_t i = 0; i < prog_.size(); i++) data_[(a & ~0x7Fu) | ((a + i) & 0x7F)] = prog_[i];
            dirty_ = true;
            irq = true;
        }
        break;
    case CMD_CHIP_ERASE:
        memset(data_.data(), 0xFF, data_.size());
        dirty_ = true;
        irq = true;
        break;
    }
    if (dirty_) flush();
    pos_ = 0;
    cmd_ = 0;
    return irq && int_enabled_;
}

uint8_t MemCard::transfer(uint8_t in, bool reading) {
    uint8_t out = 0xFF;
    if (pos_ == 0) {
        cmd_ = in;
        if (cmd_ == CMD_CLEAR_STATUS) status_ &= ~(ST_ERASE_ERR | ST_PROG_ERR);
        if (cmd_ == CMD_WAKE) status_ &= ~ST_SLEEP;
        if (cmd_ == CMD_SLEEP) status_ |= ST_SLEEP;
        pos_++;
        return out;
    }
    switch (cmd_) {
    case CMD_ID:
        // byte 1 is a dummy cycle, then a 32-bit ID giving the size in Mbit.
        if (pos_ >= 2) out = (uint8_t)(size_mbit_ >> (24 - 8 * ((pos_ - 2) & 3)));
        break;
    case CMD_READ_STATUS:
        out = status_;
        break;
    case CMD_READ_ID:
        out = pos_ == 1 ? 0xC2 : pos_ == 2 ? 0x21 : 0x00;
        break;
    case CMD_SET_INT:
        if (pos_ == 1) int_enabled_ = in & 1;
        break;
    case CMD_READ_ARRAY:
        if (pos_ <= 4) {
            switch (pos_) {
            case 1: addr_ = (uint32_t)(in & 3) << 17; break;
            case 2: addr_ |= (uint32_t)in << 9; break;
            case 3: addr_ |= (uint32_t)(in & 3) << 7; break;
            case 4: addr_ |= in & 0x7F; break;
            }
        } else if (reading) {  // latency dummy bytes are writes; data comes on reads
            out = data_[addr_ & (data_.size() - 1)];
            addr_++;
        }
        break;
    case CMD_SECTOR_ERASE:
        if (pos_ == 1) addr_ = (uint32_t)(in & 3) << 17;
        if (pos_ == 2) addr_ |= (uint32_t)in << 9;
        break;
    case CMD_PAGE_PROGRAM:
        if (pos_ <= 4) {
            switch (pos_) {
            case 1: addr_ = (uint32_t)(in & 3) << 17; break;
            case 2: addr_ |= (uint32_t)in << 9; break;
            case 3: addr_ |= (uint32_t)(in & 3) << 7; break;
            case 4: addr_ |= in & 0x7F; break;
            }
        } else if (prog_.size() < 128) {
            prog_.push_back(in);
        }
        break;
    default:
        break;
    }
    pos_++;
    return out;
}
