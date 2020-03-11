#include "mifaredesfire.h"

#include "common.h"
#include "proxmark3_arm.h"
#include "string.h"
#include "BigBuf.h"
#include "desfire_key.h"
#include "mifareutil.h"
#include "des.h"
#include "cmd.h"
#include "dbprint.h"
#include "fpgaloader.h"
#include "iso14443a.h"
#include "crc16.h"
#include "mbedtls/aes.h"
#include "commonutil.h"

#define MAX_APPLICATION_COUNT 28
#define MAX_FILE_COUNT 16
#define MAX_DESFIRE_FRAME_SIZE 60
#define NOT_YET_AUTHENTICATED 255
#define FRAME_PAYLOAD_SIZE (MAX_DESFIRE_FRAME_SIZE - 5)
#define RECEIVE_SIZE 64

// the block number for the ISO14443-4 PCB
uint8_t pcb_blocknum = 0;
// Deselect card by sending a s-block. the crc is precalced for speed
static  uint8_t deselect_cmd[] = {0xc2, 0xe0, 0xb4};

//static uint8_t __msg[MAX_FRAME_SIZE] = { 0x0A, 0x00, 0x00, /* ..., */ 0x00 };
/*                                       PCB   CID   CMD    PAYLOAD    */
//static uint8_t __res[MAX_FRAME_SIZE];

bool InitDesfireCard() {

    iso14a_card_select_t card;

    iso14443a_setup(FPGA_HF_ISO14443A_READER_LISTEN);
    set_tracing(true);

    if (!iso14443a_select_card(NULL, &card, NULL, true, 0, false)) {
        if (DBGLEVEL >= DBG_ERROR) DbpString("Can't select card");
        OnError(1);
        return false;
    }
    return true;
}

// ARG0 flag enums
enum  {
    NONE       = 0x00,
    INIT       = 0x01,
    DISCONNECT = 0x02,
    CLEARTRACE = 0x04,
    BAR        = 0x08,
} CmdOptions ;

void MifareSendCommand(uint8_t arg0, uint8_t arg1, uint8_t *datain) {

    /* ARG0 contains flags.
        0x01 = init card.
        0x02 = Disconnect
        0x03
    */
    uint8_t flags = arg0;
    size_t datalen = arg1;
    uint8_t resp[RECEIVE_SIZE];
    memset(resp, 0, sizeof(resp));

    if (DBGLEVEL >= 4) {
        Dbprintf(" flags : %02X", flags);
        Dbprintf(" len   : %02X", datalen);
        print_result(" RX    : ", datain, datalen);
    }

    if (flags & CLEARTRACE)
        clear_trace();

    if (flags & INIT) {
        if (!InitDesfireCard())
            return;
    }

    int len = DesfireAPDU(datain, datalen, resp);
    if (DBGLEVEL >= 4)
        print_result("ERR <--: ", resp, len);

    if (!len) {
        OnError(2);
        return;
    }

    // reset the pcb_blocknum,
    pcb_blocknum = 0;

    if (flags & DISCONNECT)
        OnSuccess();

    reply_old(CMD_ACK, 1, len, 0, resp, len);
}

void MifareDesfireGetInformation() {

    int len = 0;
    iso14a_card_select_t card;
    uint8_t resp[PM3_CMD_DATA_SIZE] = {0x00};
    uint8_t dataout[PM3_CMD_DATA_SIZE] = {0x00};

    /*
        1 = PCB                 1
        2 = cid                 2
        3 = desfire command     3
        4-5 = crc               4  key
                                5-6 crc
        PCB == 0x0A because sending CID byte.
        CID == 0x00 first card?
    */
    clear_trace();
    set_tracing(true);
    iso14443a_setup(FPGA_HF_ISO14443A_READER_LISTEN);

    // card select - information
    if (!iso14443a_select_card(NULL, &card, NULL, true, 0, false)) {
        if (DBGLEVEL >= DBG_ERROR) DbpString("Can't select card");
        OnError(1);
        return;
    }

    if (card.uidlen != 7) {
        if (DBGLEVEL >= DBG_ERROR) Dbprintf("Wrong UID size. Expected 7byte got %d", card.uidlen);
        OnError(2);
        return;
    }

    memcpy(dataout, card.uid, 7);

    LED_A_ON();
    LED_B_OFF();
    LED_C_OFF();

    uint8_t cmd[] = {GET_VERSION};
    size_t cmd_len = sizeof(cmd);

    len =  DesfireAPDU(cmd, cmd_len, resp);
    if (!len) {
        print_result("ERROR <--: ", resp, len);
        OnError(3);
        return;
    }

    LED_A_OFF();
    LED_B_ON();
    memcpy(dataout + 7, resp + 3, 7);

    // ADDITION_FRAME 1
    cmd[0] = ADDITIONAL_FRAME;
    len =  DesfireAPDU(cmd, cmd_len, resp);
    if (!len) {
        print_result("ERROR <--: ", resp, len);
        OnError(3);
        return;
    }

    LED_B_OFF();
    LED_C_ON();
    memcpy(dataout + 7 + 7, resp + 3, 7);

    // ADDITION_FRAME 2
    len =  DesfireAPDU(cmd, cmd_len, resp);
    if (!len) {
        print_result("ERROR <--: ", resp, len);
        OnError(3);
        return;
    }

    memcpy(dataout + 7 + 7 + 7, resp + 3, 14);

    reply_old(CMD_ACK, 1, 0, 0, dataout, sizeof(dataout));

    // reset the pcb_blocknum,
    pcb_blocknum = 0;
    OnSuccess();
}

void MifareDES_Auth1(uint8_t arg0, uint8_t arg1, uint8_t arg2,  uint8_t *datain) {
    // mode = arg0
    // algo = arg1
    // keyno = arg2
    int len = 0;
    //uint8_t PICC_MASTER_KEY8[8] = { 0x40,0x41,0x42,0x43,0x44,0x45,0x46,0x47};
    uint8_t PICC_MASTER_KEY16[16] = { 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f };
    //uint8_t null_key_data16[16] = {0x00};
    //uint8_t new_key_data8[8]  = { 0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77};
    //uint8_t new_key_data16[16]  = { 0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,0xAA,0xBB,0xCC,0xDD,0xEE,0xFF};

    uint8_t resp[256] = {0x00};

    size_t datalen = datain[0];

    uint8_t cmd[40] = {0x00};
    uint8_t encRndB[16] = {0x00};
    uint8_t decRndB[16] = {0x00};
    uint8_t both[32] = {0x00};

    InitDesfireCard();

    LED_A_ON();
    LED_B_OFF();
    LED_C_OFF();

    // 3 different way to authenticate   AUTH (CRC16) , AUTH_ISO (CRC32) , AUTH_AES (CRC32)
    // 4 different crypto arg1   DES, 3DES, 3K3DES, AES
    // 3 different communication modes,  PLAIN,MAC,CRYPTO

    // des, key 0,
    switch (arg0) {
        case 1: {
            uint8_t keybytes[16];
            uint8_t RndA[8] = {0x00};
            uint8_t RndB[8] = {0x00};

            if (arg1 == 2) {
                if (datain[1] == 0xff) {
                    memcpy(keybytes, PICC_MASTER_KEY16, 16);
                } else {
                    memcpy(keybytes, datain + 1, datalen);
                }
            } else {
                if (arg1 == 1) {
                    if (datain[1] == 0xff) {
                        uint8_t null_key_data8[8] = {0x00};
                        memcpy(keybytes, null_key_data8, 8);
                    } else {
                        memcpy(keybytes, datain + 1, datalen);
                    }
                }
            }

            struct desfire_key defaultkey = {0};
            desfirekey_t key = &defaultkey;

            if (arg1 == 2)
                Desfire_3des_key_new_with_version(keybytes, key);
            else if (arg1 == 1)
                Desfire_des_key_new(keybytes, key);

            cmd[0] = AUTHENTICATE;
            cmd[1] = arg2;  //keynumber
            len = DesfireAPDU(cmd, 2, resp);
            if (!len) {
                if (DBGLEVEL >= DBG_ERROR) {
                    DbpString("Authentication failed. Card timeout.");
                }
                OnError(3);
                return;
            }

            if (resp[2] == 0xaf) {
            } else {
                DbpString("Authentication failed. Invalid key number.");
                OnError(3);
                return;
            }

            memcpy(encRndB, resp + 3, 8);
            if (arg1 == 2)
                tdes_dec(&decRndB, &encRndB, key->data);
            else if (arg1 == 1)
                des_dec(&decRndB, &encRndB, key->data);

            memcpy(RndB, decRndB, 8);
            rol(decRndB, 8);

            // This should be random
            uint8_t decRndA[8] = {0x00};
            memcpy(RndA, decRndA, 8);
            uint8_t encRndA[8] = {0x00};

            if (arg1 == 2)
                tdes_dec(&encRndA, &decRndA, key->data);
            else if (arg1 == 1)
                des_dec(&encRndA, &decRndA, key->data);

            memcpy(both, encRndA, 8);

            for (int x = 0; x < 8; x++) {
                decRndB[x] = decRndB[x] ^ encRndA[x];

            }

            if (arg1 == 2)
                tdes_dec(&encRndB, &decRndB, key->data);
            else if (arg1 == 1)
                des_dec(&encRndB, &decRndB, key->data);

            memcpy(both + 8, encRndB, 8);

            cmd[0] = ADDITIONAL_FRAME;
            memcpy(cmd + 1, both, 16);

            len = DesfireAPDU(cmd, 17, resp);
            if (!len) {
                if (DBGLEVEL >= DBG_ERROR) {
                    DbpString("Authentication failed. Card timeout.");
                }
                OnError(3);
                return;
            }

            if (resp[2] == 0x00) {

                struct desfire_key sessionKey = {0};
                desfirekey_t skey = &sessionKey;
                Desfire_session_key_new(RndA, RndB, key, skey);
                //print_result("SESSION : ", skey->data, 8);

                memcpy(encRndA, resp + 3, 8);

                if (arg1 == 2)
                    tdes_dec(&encRndA, &encRndA, key->data);
                else if (arg1 == 1)
                    des_dec(&encRndA, &encRndA, key->data);

                rol(decRndA, 8);
                for (int x = 0; x < 8; x++) {
                    if (decRndA[x] != encRndA[x]) {
                        DbpString("Authentication failed. Cannot varify PICC.");
                        OnError(4);
                        return;
                    }
                }

                //Change the selected key to a new value.
                /*

                 // Current key is a 3DES key, change it to a DES key
                 if (arg1 == 2) {
                cmd[0] = CHANGE_KEY;
                cmd[1] = arg2;

                uint8_t newKey[16] = {0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77};

                uint8_t first, second;
                uint8_t buff1[8] = {0x00};
                uint8_t buff2[8] = {0x00};
                uint8_t buff3[8] = {0x00};

                memcpy(buff1,newKey, 8);
                memcpy(buff2,newKey + 8, 8);

                compute_crc(CRC_14443_A, newKey, 16, &first, &second);
                memcpy(buff3, &first, 1);
                memcpy(buff3 + 1, &second, 1);

                 tdes_dec(&buff1, &buff1, skey->data);
                 memcpy(cmd+2,buff1,8);

                 for (int x = 0; x < 8; x++) {
                 buff2[x] = buff2[x] ^ buff1[x];
                 }
                 tdes_dec(&buff2, &buff2, skey->data);
                 memcpy(cmd+10,buff2,8);

                 for (int x = 0; x < 8; x++) {
                 buff3[x] = buff3[x] ^ buff2[x];
                 }
                 tdes_dec(&buff3, &buff3, skey->data);
                 memcpy(cmd+18,buff3,8);

                 // The command always times out on the first attempt, this will retry until a response
                 // is recieved.
                 len = 0;
                 while(!len) {
                 len = DesfireAPDU(cmd,26,resp);
                 }

                 } else {
                    // Current key is a DES key, change it to a 3DES key
                    if (arg1 == 1) {
                        cmd[0] = CHANGE_KEY;
                        cmd[1] = arg2;

                        uint8_t newKey[16] = {0x40,0x41,0x42,0x43,0x44,0x45,0x46,0x47,0x48,0x49,0x4a,0x4b,0x4c,0x4d,0x4e,0x4f};

                        uint8_t first, second;
                        uint8_t buff1[8] = {0x00};
                        uint8_t buff2[8] = {0x00};
                        uint8_t buff3[8] = {0x00};

                        memcpy(buff1,newKey, 8);
                        memcpy(buff2,newKey + 8, 8);

                        compute_crc(CRC_14443_A, newKey, 16, &first, &second);
                        memcpy(buff3, &first, 1);
                        memcpy(buff3 + 1, &second, 1);

                des_dec(&buff1, &buff1, skey->data);
                memcpy(cmd+2,buff1,8);

                for (int x = 0; x < 8; x++) {
                    buff2[x] = buff2[x] ^ buff1[x];
                }
                des_dec(&buff2, &buff2, skey->data);
                memcpy(cmd+10,buff2,8);

                for (int x = 0; x < 8; x++) {
                    buff3[x] = buff3[x] ^ buff2[x];
                }
                des_dec(&buff3, &buff3, skey->data);
                memcpy(cmd+18,buff3,8);

                // The command always times out on the first attempt, this will retry until a response
                // is recieved.
                len = 0;
                while(!len) {
                    len = DesfireAPDU(cmd,26,resp);
                }
                    }
                 }
                */

                OnSuccess();
                if (arg1 == 2)
                    reply_old(CMD_ACK, 1, 0, 0, skey->data, 16);
                else if (arg1 == 1)
                    reply_old(CMD_ACK, 1, 0, 0, skey->data, 8);
            } else {
                DbpString("Authentication failed.");
                OnError(6);
                return;
            }
        }
        break;
        case 2:
            //SendDesfireCommand(AUTHENTICATE_ISO, &arg2, resp);
            break;
        case 3: {

            //defaultkey
            uint8_t keybytes[16] = {0x00};
            if (datain[1] == 0xff) {
                memcpy(keybytes, PICC_MASTER_KEY16, 16);
            } else {
                memcpy(keybytes, datain + 1, datalen);
            }

            struct desfire_key defaultkey = {0x00};
            desfirekey_t key = &defaultkey;
            Desfire_aes_key_new(keybytes, key);

            mbedtls_aes_context ctx;
            uint8_t IV[16] = {0x00};
            mbedtls_aes_init(&ctx);

            cmd[0] = AUTHENTICATE_AES;
            cmd[1] = 0x00;  //keynumber
            len = DesfireAPDU(cmd, 2, resp);
            if (!len) {
                if (DBGLEVEL >= DBG_ERROR) {
                    DbpString("Authentication failed. Card timeout.");
                }
                OnError(3);
                return;
            }

            memcpy(encRndB, resp + 3, 16);

            // dekryptera tagnonce.
            if (mbedtls_aes_setkey_dec(&ctx, key->data, 128) != 0) {
                if (DBGLEVEL >= 4) {
                    DbpString("mbedtls_aes_setkey_dec failed");
                }
                OnError(7);
                return;
            }
            mbedtls_aes_crypt_cbc(&ctx, MBEDTLS_AES_DECRYPT, 16, IV, encRndB, decRndB);
            rol(decRndB, 16);
            uint8_t nonce[16] = {0x00};
            memcpy(both, nonce, 16);
            memcpy(both + 16, decRndB, 16);
            uint8_t encBoth[32] = {0x00};
            if (mbedtls_aes_setkey_enc(&ctx, key->data, 128) != 0) {
                if (DBGLEVEL >= 4) {
                    DbpString("mbedtls_aes_setkey_enc failed");
                }
                OnError(7);
                return;
            }
            mbedtls_aes_crypt_cbc(&ctx, MBEDTLS_AES_ENCRYPT, 32, IV, both, encBoth);

            cmd[0] = ADDITIONAL_FRAME;
            memcpy(cmd + 1, encBoth, 32);

            len = DesfireAPDU(cmd, 33, resp);  // 1 + 32 == 33
            if (!len) {
                if (DBGLEVEL >= DBG_ERROR) {
                    DbpString("Authentication failed. Card timeout.");
                }
                OnError(3);
                return;
            }

            if (resp[2] == 0x00) {
                // Create AES Session key
                struct desfire_key sessionKey = {0};
                desfirekey_t skey = &sessionKey;
                Desfire_session_key_new(nonce, decRndB, key, skey);
                print_result("SESSION : ", skey->data, 16);
            } else {
                DbpString("Authentication failed.");
                OnError(7);
                return;
            }

            break;
        }
    }

    OnSuccess();
    reply_old(CMD_ACK, 1, len, 0, resp, len);
}

// 3 different ISO ways to send data to a DESFIRE (direct, capsuled, capsuled ISO)
// cmd  =  cmd bytes to send
// cmd_len = length of cmd
// dataout = pointer to response data array
int DesfireAPDU(uint8_t *cmd, size_t cmd_len, uint8_t *dataout) {

    size_t len = 0;
    size_t wrappedLen = 0;
    uint8_t wCmd[PM3_CMD_DATA_SIZE] = {0x00};
    uint8_t resp[MAX_FRAME_SIZE];
    uint8_t par[MAX_PARITY_SIZE];

    wrappedLen = CreateAPDU(cmd, cmd_len, wCmd);

    if (DBGLEVEL >= 4)
        print_result("WCMD <--: ", wCmd, wrappedLen);

    ReaderTransmit(wCmd, wrappedLen, NULL);

    len = ReaderReceive(resp, par);
    if (!len) {
        if (DBGLEVEL >= 4) Dbprintf("fukked");
        return false; //DATA LINK ERROR
    }
    // if we received an I- or R(ACK)-Block with a block number equal to the
    // current block number, toggle the current block number
    else if (len >= 4 // PCB+CID+CRC = 4 bytes
             && ((resp[0] & 0xC0) == 0 // I-Block
                 || (resp[0] & 0xD0) == 0x80) // R-Block with ACK bit set to 0
             && (resp[0] & 0x01) == pcb_blocknum) { // equal block numbers
        pcb_blocknum ^= 1;  //toggle next block
    }

    memcpy(dataout, resp, len);
    return len;
}

// CreateAPDU
size_t CreateAPDU(uint8_t *datain, size_t len, uint8_t *dataout) {

    size_t cmdlen = MIN(len + 4, PM3_CMD_DATA_SIZE - 1);

    uint8_t cmd[cmdlen];
    memset(cmd, 0, cmdlen);

    cmd[0] = 0x0A;  //  0x0A = send cid,  0x02 = no cid.
    cmd[0] |= pcb_blocknum; // OR the block number into the PCB
    cmd[1] = 0x00;  //  CID: 0x00 //TODO: allow multiple selected cards

    memcpy(cmd + 2, datain, len);
    AddCrc14A(cmd, len + 2);

    memcpy(dataout, cmd, cmdlen);

    return cmdlen;
}

// crc_update(&desfire_crc32, 0, 1); /* CMD_WRITE */
// crc_update(&desfire_crc32, addr, addr_sz);
// crc_update(&desfire_crc32, byte, 8);
// uint32_t crc = crc_finish(&desfire_crc32);

void OnSuccess() {
    pcb_blocknum = 0;
    ReaderTransmit(deselect_cmd, 3, NULL);
    if (mifare_ultra_halt()) {
        if (DBGLEVEL >= DBG_ERROR) Dbprintf("Halt error");
    }
    switch_off();
}

void OnError(uint8_t reason) {
    reply_old(CMD_ACK, 0, reason, 0, 0, 0);
    OnSuccess();
}
