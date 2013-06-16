#include "sdio.h"

#include <cassert>


const char* const Sdio::STATUS_MSG[] =
{
    "Command response CRC error",
    "Data CDC error",
    "Command response timeout",
    "Data timeout",
    "Transmit FIFO underrun",
    "Receive FIFO underrun",
    "Command response received",
    "Command sent",
    "Data end",
    "Start bit error",
    "Data block transferred",
    "Command in progress",
    "Data transmit in progress",
    "Data receive in progress",
    "Transmit FIFO half empty",
    "Receive FIFO half full",
    "Transmit FIFO full",
    "Receive FIFO full",
    "Transmit FIFO empty",
    "Receive FIFO empty",
    "Data available in transmit FIFO",
    "Data available in receive FIFO",
    "SDIO interrupt",
    "CE-ATA command completion signal received"
};

Sdio::Sdio(System::BaseAddress base, int supplyVoltage) :
    mBase(reinterpret_cast<volatile SDIO*>(base)),
    mVolt(supplyVoltage),
    mDebug(true),
    mRca(0)
{
    static_assert(sizeof(SDIO) == 0x100, "Struct has wrong size, compiler problem.");
    enable(false);
}

void Sdio::enable(bool enable)
{
    if (enable)
    {
        setClock(CLOCK_IDENTIFICATION);
        mBase->POWER.PWRCTRL = 3;
        mBase->DTIMER = 48 * 1000 * 1000;  // 1s timeout
        mBase->CMD.bits.CPSMEN = 1;
        SDIO::__CLKCR clkcr;
        clkcr.value = mBase->CLKCR.value;
        clkcr.bits.CLKEN = 1;
        clkcr.bits.PWRSAV = 0;
        mBase->CLKCR.value = clkcr.value;
    }
    else
    {
        mBase->CMD.bits.CPSMEN = 0;
        mBase->POWER.PWRCTRL = 0;
    }
}

void Sdio::setClock(unsigned clock)
{
    unsigned div = (PLL_CLOCK + clock - 1) / clock;
    if (div <= 1)
    {
        mBase->CLKCR.bits.BYPASS = 1;
    }
    else
    {
        SDIO::__CLKCR clkcr;
        clkcr.value = mBase->CLKCR.value;
        clkcr.bits.BYPASS = 0;
        clkcr.bits.CLKDIV = div - 2;
        mBase->CLKCR.value = clkcr.value;
    }
    if (mDebug) printf("Setting clock to %luHz.\n", this->clock());
}

uint32_t Sdio::clock()
{
    if (mBase->CLKCR.bits.BYPASS)
    {
        return PLL_CLOCK;
    }
    else
    {
        return PLL_CLOCK / (mBase->CLKCR.bits.CLKDIV + 2);
    }
}

void Sdio::printHostStatus()
{
    uint32_t v = mBase->STA.value;
    printf("SDIO:");
    int count = 0;
    for (unsigned i = 0; i < sizeof(STATUS_MSG) / sizeof(STATUS_MSG[0]); ++i)
    {
        if ((v & (1 << i)) != 0)
        {
            if (count != 0) printf(",");
            printf(" '%s'", STATUS_MSG[i]);
            ++count;
        }
    }
    printf("\n");
}

void Sdio::resetCard()
{
    sendCommand(0, 0, Response::None);
}

bool Sdio::initCard()
{
    bool v2 = false;
    resetCard();
    // Send CMD8
    Result result = interfaceCondition();
    // if the card responds it is a v2 or later
    if (result == Result::Ok) v2 = true;
    // Send ACMD41, to check voltage ranges
    if (initializeCard(v2) != Result::Ok)
    {
        System::instance()->printWarning("SDIO", "Card not supported or not a SD/SDHC/SDXC card.");
        return false;
    }
    if (getCardIdentifier() != Result::Ok)
    {
        System::instance()->printWarning("SDIO", "Can't read CID.");
        return false;
    }
    if (getRelativeCardAddress() != Result::Ok)
    {
        System::instance()->printWarning("SDIO", "Can't read RCA.");
        return false;
    }
    if (getCardSpecificData() != Result::Ok)
    {
        System::instance()->printWarning("SDIO", "Can't read CSD.");
        return false;
    }
    if (selectCard() != Result::Ok)
    {
        System::instance()->printWarning("SDIO", "Can't select card.");
        return false;
    }
    if (getCardStatus() != Result::Ok)
    {
        System::instance()->printWarning("SDIO", "Can't read status.");
        return false;
    }
    return true;
}

Sdio::Result Sdio::interfaceCondition() // CMD8
{
    union
    {
        struct
        {
            uint32_t checkPattern : 8;
            uint32_t voltageSupplied : 4;
            uint32_t __RESERVED : 20;
        }   bits;
        uint32_t value;
    }   arg;
    arg.value = 0;
    arg.bits.checkPattern = CHECK_PATTERN;
    arg.bits.voltageSupplied = 1; // 2.7 - 3.6V
    sendCommand(8, arg.value, Response::Short);
    if (mBase->RESP[0] != arg.value) return Result::Error;
    return Result::Ok;
}

Sdio::Result Sdio::initializeCard(bool hcSupport)
{
    union
    {
        struct
        {
            uint32_t __RESERVED0 : 8;
            uint32_t OCR : 16;
            uint32_t switchTo1_8V : 1;
            uint32_t __RESERVED1 : 3;
            uint32_t xcPowerControl : 1;
            uint32_t uhsSupport : 1;
            uint32_t hcSupport : 1;
            uint32_t busy : 1;
        }   bits;
        uint32_t value;
    }   arg;
    arg.value = 0;
    arg.bits.OCR = ocrFromVoltage(mVolt);
    arg.bits.hcSupport = hcSupport ? 1 : 0;
    Result result;
    int timeout = 1000;
    do
    {
        result = sendAppCommand(41, arg.value, Response::ShortNoCrc);
        if (result != Result::Ok) return result;
        System::instance()->usleep(10000);
        --timeout;
    }   while ((mBase->RESP[0] & 0x80000000) == 0 && timeout >= 0);
    if ((mBase->RESP[0] & 0x80000000) == 0) result = Result::Timeout;
    if (mDebug) printOcr();
    return result;
}

Sdio::Result Sdio::getCardIdentifier()
{
    Result result = sendCommand(2, 0, Response::Long);
    if (result == Result::Ok)
    {
        for (int i = 0; i < 4; ++i) mCid.value[i] = mBase->RESP[i];
        if (mDebug)
        {
            printf("   Manufacturer ID : %u\n", mCid.bits.MID);
            printf("OEM/Application ID : %c%c\n", mCid.bits.OID[1], mCid.bits.OID[0]);
            printf("      Product name : %c%c%c%c%c\n", mCid.bits.PNM[4], mCid.bits.PNM[3], mCid.bits.PNM[2], mCid.bits.PNM[1], mCid.bits.PNM[0]);
            printf("  Product revision : %i.%i\n", mCid.bits.PRV >> 4, mCid.bits.PRV & 0xf);
            printf("    Product serial : %u\n", mCid.bits.PSN[3] << 24 | mCid.bits.PSN[2] << 16 | mCid.bits.PSN[1] << 8 | mCid.bits.PSN[0]);
            printf("Manufacturing Date : %u.%u\n", mCid.bits.MDT[0] & 0xf, ((mCid.bits.MDT[0] >> 4) | ((mCid.bits.MDT[1] & 0x0f) << 4)) + 2000);
        }
    }
    return result;
}

Sdio::Result Sdio::getRelativeCardAddress()
{
    Result result = sendCommand(3, 0, Response::Short);
    if (result == Result::Ok)
    {
        mRca = mBase->RESP[0] & 0xffff0000;
        if (mDebug) printf("RCA is %04x\n", mRca);
        checkCardStatus(mBase->RESP[0] & 0x0000ffff);
    }
    return result;
}

Sdio::Result Sdio::getCardSpecificData()
{
    static const int CSD_LEN = 16;
    static const int TIME_TABLE[16] = { 0, 1000, 1200, 1300, 1500, 2000, 2500, 3000, 3500, 4000, 4500, 5000, 5500, 6000, 7000, 8000 };
    static const int CURRENT_MIN[8] = { 0, 1, 5, 10, 25, 35, 60, 100 };
    static const int CURRENT_MAX[8] = { 1, 5, 10, 25, 35, 45, 80, 200 };
    static const char* const FILE_FORMAT[4] = { "HDD", "Floppy", "Universal", "Others/Unknown" };

    Result result = sendCommand(9, mRca, Response::Long);
    if (result == Result::Ok)
    {
        uint8_t csd[16];
        for (int i = 0; i < 4; ++i)
        {
            uint32_t v = mBase->RESP[i];
            for (int j = 0; j < 4; ++j) csd[i * 4 + j] = v >> ((3 - j) * 8);
        }

        // CSD_STRUCTURE
        int csdVersion = getBits(csd, CSD_LEN, 127, 126) + 1;
        if (mDebug) printf("\nCSD (v%i):\n---------\n", csdVersion);

        // N_AC(max) = 100 * ((TAAC * f_interface) + (100 * NSAC))
        mCsd.mTaac = pow10(getBits(csd, CSD_LEN, 114, 112) - 3) * TIME_TABLE[getBits(csd, CSD_LEN, 118, 115)];
        mCsd.mNsac = getBits(csd, CSD_LEN, 111, 104);
        int NAC = 100 * ((static_cast<float>(mCsd.mTaac) * static_cast<float>(clock()) / 1000000000.0f) + (100 * mCsd.mNsac));
        if (mDebug) printf("T_AAC is %uns, N_SAC is %u, N_AC is %i clocks = %lums.\n", mCsd.mTaac, mCsd.mNsac, NAC, 1000 * NAC / clock());

        // TRAN_SPEED
        mCsd.mTransferRate = 100 * pow10(getBits(csd, CSD_LEN, 98, 96)) * TIME_TABLE[getBits(csd, CSD_LEN, 102, 99)];
        if (mDebug) printf("Maximum transfer rate is %u Hz.\n", mCsd.mTransferRate);
        setClock(mCsd.mTransferRate);

        // CCC
        mCsd.mCommandClass = getBits(csd, CSD_LEN, 95, 84);
        if (mDebug)
        {
            static const char* const COMMAND_CLASS[] = { "basic", 0, "block read", 0, "block write", "erase", "write protection", "lock card", "application specific", "I/O mode", "switch", "extension" };
            printf("Supported command classes:");
            for (int i = 0; i <= 11; ++i)
            {
                if (mCsd.mCommandClass & (1 << i))
                {
                    if (COMMAND_CLASS[i] != 0) printf(" '%s'", COMMAND_CLASS[i]);
                    else printf(" %i", i);
                }
            }
            printf("\n");
        }

        // READ_BL_LEN
        mCsd.mReadBlockSize = 1 << getBits(csd, CSD_LEN, 83, 80);
        // READ_BL_PARITAL
        mCsd.mPartialBlockRead = getBits(csd, CSD_LEN, 79, 79);
        // READ_BL_MISALIGN
        mCsd.mReadBlockMisalign = getBits(csd, CSD_LEN, 77, 77);
        // WRITE_BL_LEN
        mCsd.mWriteBlockSize = 1 << getBits(csd, CSD_LEN, 25, 22);
        // WRITE_BL_PARTIAL
        mCsd.mPartialBlockWrite = getBits(csd, CSD_LEN, 21, 21);
        // WRITE_BL_MISALIGN
        mCsd.mWriteBlockMisalign = getBits(csd, CSD_LEN, 78, 78);
        if (mDebug)
        {
            printf("Max block size is %u for reading and %u for writing.\n", mCsd.mReadBlockSize, mCsd.mWriteBlockSize);
            printf("Partial reading is %ssupported, partial writing is %ssupported.\n", mCsd.mPartialBlockRead ? "" : "NOT ", mCsd.mPartialBlockWrite ? "" : "NOT ");
            printf("Misaligned reading is %ssupported, misaligned writing is %ssupported.\n", mCsd.mReadBlockMisalign ? "" : "NOT ", mCsd.mWriteBlockMisalign ? "" : "NOT ");
        }

        // DSR_IMP
        mCsd.mDriverStageImplemented = getBits(csd, CSD_LEN, 76, 76);

        if (csdVersion == 1)
        {
            // C_SIZE & C_SIZE_MULT
            mCsd.mBlockCount = (getBits(csd, CSD_LEN, 73, 62) + 1) * (1 << (getBits(csd, CSD_LEN, 49, 47) + 2));
        }
        else if (csdVersion == 2)
        {

            mCsd.mBlockCount = (getBits(csd, CSD_LEN, 69, 48) + 1) * 1024;
        }
        if (mDebug) printf("Card has %u blocks with size %u and is therefore %luMB.\n", mCsd.mBlockCount, mCsd.mReadBlockSize, static_cast<uint32_t>(((static_cast<uint64_t>(mCsd.mBlockCount) * static_cast<uint64_t>(mCsd.mReadBlockSize)) >> 20)));
        if (csdVersion == 1)
        {
            // VDD_R_CURR_MIN
            mCsd.mReadCurrentMin = CURRENT_MIN[getBits(csd, CSD_LEN, 61, 59)];
            // VDD_R_CURR_MAX
            mCsd.mReadCurrentMax = CURRENT_MAX[getBits(csd, CSD_LEN, 58, 56)];
            // VDD_W_CURR_MIN
            mCsd.mWriteCurrentMin = CURRENT_MIN[getBits(csd, CSD_LEN, 55, 53)];
            // VDD_W_CURR_MAX
            mCsd.mWriteCurrentMax = CURRENT_MAX[getBits(csd, CSD_LEN, 52, 50)];
            if (mDebug) printf("Card requires [%u, %u]mA for reading and [%u, %u]mA for writing.\n", mCsd.mReadCurrentMin, mCsd.mReadCurrentMax, mCsd.mWriteCurrentMin, mCsd.mWriteCurrentMax);
        }
        // ERASE_BLK_EN
        mCsd.mEraseSingleBlock = getBits(csd, CSD_LEN, 46, 46);
        // SECTOR_SIZE = number of write blocks
        mCsd.mEraseBlockSize = (getBits(csd, CSD_LEN, 45, 39) + 1) * mCsd.mWriteBlockSize;
        if (mDebug) printf("Card can erase sectors in multiple of %u bytes.\n", mCsd.mEraseSingleBlock ? 512 : mCsd.mEraseBlockSize);
        // WP_GRP_SIZE
        mCsd.mWriteProtectGroupSize = (getBits(csd, CSD_LEN, 38, 32) + 1) * mCsd.mEraseBlockSize;
        // WP_GRP_ENABLE
        mCsd.mWriteProtectGroupEnabled = getBits(csd, CSD_LEN, 31, 31);
        if (mDebug)
        {
            if (mCsd.mWriteProtectGroupEnabled) printf("Card can write protect groups of size %i.\n", mCsd.mWriteProtectGroupSize);
            else printf("Card can NOT write protect groups.\n");
        }
        // R2W_FACTOR
        mCsd.mReadToWriteFactor = 1 << getBits(csd, CSD_LEN, 28, 26);
        if (mDebug) printf("Writing is %i times slower than reading.\n", mCsd.mReadToWriteFactor);
        // FILE_FORMAT_GRP
        mCsd.mFileFormatGroup = getBits(csd, CSD_LEN, 15, 15);
        // COPY
        mCsd.mCopy = getBits(csd, CSD_LEN, 14, 14);
        if (mDebug) printf("Card is %s.\n", mCsd.mCopy ? "copy" : "original");
        // PERM_WRITE_PROTECT
        mCsd.mPermanentlyWriteProtected = getBits(csd, CSD_LEN, 13, 13);
        if (mDebug) printf("Card is %spermanently write protected.\n", mCsd.mPermanentlyWriteProtected ? "" : "NOT ");
        // TMP_WRITE_PROTECT
        mCsd.mTemporarilyWriteProtected = getBits(csd, CSD_LEN, 12, 12);
        if (mDebug) printf("Card is %stemporaily write protected.\n", mCsd.mTemporarilyWriteProtected ? "" : "NOT ");
        // FILE_FORMAT
        mCsd.mFileFormat = getBits(csd, CSD_LEN, 11, 10);
        if (mDebug) printf("File format is %s.\n", mCsd.mFileFormatGroup ? "Reserved" : FILE_FORMAT[mCsd.mFileFormat]);

    }
    return result;
}

Sdio::Result Sdio::selectCard(bool select)
{
    Result result = sendCommand(7, mRca, Response::ShortNoCrc);;
    if (result == Result::Ok) checkCardStatus(mBase->RESP[0]);
    return result;
}

Sdio::Result Sdio::getCardStatus()
{
    Result result = sendCommand(13, mRca, Response::Short);;
    if (result == Result::Ok) checkCardStatus(mBase->RESP[0]);
    return result;
}

Sdio::Result Sdio::getCardConfiguration()
{
    Result result = sendAppCommand(51, 0, Response::Short);
    if (result == Result::Ok)
    {

    }
    return result;
}

Sdio::Result Sdio::sendCommand(uint8_t cmd, uint32_t arg, Response response)
{
    bool longResponse = response == Response::Long || response == Response::LongNoCrc;
    bool waitResponse = response != Response::None;
    bool ignoreCrc = response == Response::ShortNoCrc || response == Response::LongNoCrc;

    Result result = Result::Ok;
    // Wait for previous command to finish
    while (mBase->STA.bits.CMDACT != 0) ;
    // clear all status bits
    mBase->ICR.value = 0x00c007ff;
    if (mDebug) printf("SEND %i(%08lx) with %s\n", cmd, arg, toString(response));
    mBase->ARG = arg;
    mBase->CMD.bits.CMD_WAIT = (longResponse ? 0x80 : 0) | (waitResponse ? 0x40 : 0) | (cmd & 0x3f);
    if (waitResponse)
    {
        // Wait for command to finish or timeout or CRC fail
        while (mBase->STA.bits.CMDREND == 0 && mBase->STA.bits.CCRCFAIL == 0 && mBase->STA.bits.CTIMEOUT == 0) ;
        if (mBase->STA.bits.CTIMEOUT) result = Result::Timeout;
        if (mBase->STA.bits.CCRCFAIL && !ignoreCrc) result = Result::CrcError;
    }
    if (result != Result::Ok) printHostStatus();
    else if (mDebug && waitResponse) printf("RESP: %08lx\n", mBase->RESP[0]);
    return result;
}

Sdio::Result Sdio::sendAppCommand(uint8_t cmd, uint32_t arg, Response response)
{
    mBase->ARG = 0;
    Result result = sendCommand(55, 0, Response::Short);
    if (result == Result::Ok)
    {
        if (checkCardStatus(mBase->RESP[0]))
        {
            result = sendCommand(cmd, arg, response);
        }
        else
        {
            result = Result::Error;
        }
    }
    return result;
}

bool Sdio::checkCardStatus(uint32_t status)
{
    static const uint32_t AKE_SEQ_ERROR = 1 << 3;
    static const uint32_t APP_CMD = 1 << 5;
    static const uint32_t READY_FOR_DATA = 1 << 8;
    static const uint32_t CURRENT_STATE_MASK = 15 << 9;
    static const int CURRENT_STATE_SHIFT = 9;
    static const uint32_t ERASE_RESET = 1 << 13;
    static const uint32_t CARD_ECC_DISABLED = 1 << 14;
    static const uint32_t WP_ERASE_SKIP = 1 << 15;
    static const uint32_t CSD_OVERWRITE = 1 << 16;
    static const uint32_t ERROR = 1 << 19;
    static const uint32_t CC_ERROR = 1 << 20;
    static const uint32_t CARD_ECC_FAILED = 1 << 21;
    static const uint32_t ILLEGAL_COMMAND = 1 << 22;
    static const uint32_t COM_CRC_ERROR = 1 << 23;
    static const uint32_t LOCK_UNLOCK_FAILED = 1 << 24;
    static const uint32_t CARD_IS_LOCKED = 1 << 25;
    static const uint32_t WP_VIOLATION = 1 << 26;
    static const uint32_t ERASE_PARAM = 1 << 27;
    static const uint32_t ERASE_SEQ_ERROR = 1 << 28;
    static const uint32_t BLOCK_LEN_ERROR = 1 << 29;
    static const uint32_t ADDRESS_ERROR = 1 << 30;
    static const uint32_t OUT_OF_RANGE = 1 << 31;
    static const char* const CURRENT_STATE[16] = { "Idle", "Ready", "Identification", "Standby", "Transfer", "Data", "Receive", "Programm", "Disabled", "9", "10", "11", "12", "13", "14", "15" };
    static const char* const STATE[32] = { 0, 0, 0, "AKE_SEQ_ERROR", 0, 0, 0, 0,
                                           "READY_FOR_DATA", 0, 0, 0, 0, "ERASE_RESET", "CARD_ECC_DISABLED", "WP_ERASE_SKIP",
                                           "CSD_OVERWRITE", 0, 0, "ERROR", "CC_ERROR", "CARD_ECC_FAILED", "ILLEGAL_COMMAND", "COM_CRC_ERROR",
                                           "LOCK_UNLOCK_FAILED", "CARD_IS_LOCKED", "WP_VIOLATION", "ERASE_PARAM", "ERASE_SEQ_ERROR", "BLOCK_LEN_ERROR", "ADDRESS_ERROR", "OUT_OF_RANGE" };
    if ((status & (AKE_SEQ_ERROR | WP_ERASE_SKIP | CSD_OVERWRITE | ERROR |
            CC_ERROR | CARD_ECC_FAILED | ILLEGAL_COMMAND | COM_CRC_ERROR |
            COM_CRC_ERROR | LOCK_UNLOCK_FAILED | WP_VIOLATION | ERASE_PARAM |
            ERASE_SEQ_ERROR | BLOCK_LEN_ERROR | ADDRESS_ERROR | OUT_OF_RANGE)) != 0)
    {
        printf("SD ERROR:");
        for (int i = 0; i < 32; ++i)
        {
            if ((status & (1 << i)) != 0 && STATE[i] != 0) printf(" %s", STATE[i]);
        }
        printf("\nCard state is %s\n", CURRENT_STATE[(status & CURRENT_STATE_MASK) >> CURRENT_STATE_SHIFT]);
        return false;
    }
    bool isAppCmd = (status & APP_CMD) != 0;
    if (mDebug) printf("SD(%s):%s:%s%s%s%s.\n", CURRENT_STATE[(status & CURRENT_STATE_MASK) >> CURRENT_STATE_SHIFT],
                                isAppCmd ? "APP_CMD" : "REGULAR_CMD",
                                ((status & READY_FOR_DATA) != 0) ? " 'READY_FOR_DATA'" : "",
                                ((status & ERASE_RESET) != 0) ? " 'ERASE_RESET'" : "",
                                ((status & CARD_ECC_DISABLED) != 0) ? " 'CARD_ECC_DISABLED'" : "",
                                ((status & CARD_IS_LOCKED) != 0) ? " 'CARD_IS_LOCKED'" : ""
                           );

    return true;
}

uint32_t Sdio::ocrFromVoltage(int volt)
{
    uint32_t ocr = 0;
    int minVolt = 35;
    for (int i = 15; i >= 7; --i)
    {
        if (volt >= minVolt && volt <= (minVolt + 1)) ocr |= 1 << i;
        minVolt -= 1;
    }
    return ocr;
}

void Sdio::voltageFromOcr(uint32_t ocr, int& minVoltage, int& maxVoltage)
{
    minVoltage = 0, maxVoltage = 0;
    for (int i = 15; i < 24; ++i)
    {
        if ((ocr & (1 << i)) != 0)
        {
            if (minVoltage == 0) minVoltage = i - 15 + 27;
            maxVoltage = i - 15 + 28;
        }
    }
}

void Sdio::printOcr()
{
    uint32_t ocr = mBase->RESP[0];
    printf("Card is ");
    if ((ocr & 0x80000000) == 0) printf("BUSY\n");
    else
    {
        printf("READY, type is ");
        if (ocr & 0x40000000) printf("HC/XC");
        else printf("SC");
        int min, max;
        voltageFromOcr(ocr, min, max);
        printf(" and supports %i.%i-%i.%iV.\n", min / 10, min % 10, max / 10, max % 10);
    }
}

const char *Sdio::toString(Sdio::Response response)
{
    switch (response)
    {
    case Sdio::Response::None:          return "None";
    case Sdio::Response::Short:         return "Short";
    case Sdio::Response::Long:          return "Long";
    case Sdio::Response::ShortNoCrc:    return "ShortNoCrc";
    case Sdio::Response::LongNoCrc:     return "LongNoCrc";
    }
    return "UNKNOWN_RESPONSE";
}

uint32_t Sdio::getBits(uint8_t *field, int fieldSize, int highestBit, int lowestBit)
{
    int byteOffset = fieldSize - highestBit / 8 - 1;
    int bitOffset = highestBit % 8;
    int bitCount = highestBit - lowestBit + 1;
    if (bitCount > 32) assert("Bit count greater than 32 not allowed.");
    uint32_t ret = 0;
    while (bitCount > 0)
    {
        int shift = bitCount - bitOffset - 1;
        if (shift > 0) ret |= (field[byteOffset] << shift);
        else ret |= field[byteOffset] >> -shift;
        bitCount -= bitOffset + 1;
        ++byteOffset;
        bitOffset = 7;
    }
    ret &= 0xffffffff >> (31 - highestBit + lowestBit);
    return ret;
}
