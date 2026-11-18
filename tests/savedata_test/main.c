#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include "ps4_savedata.h"

int main(void) {
    printf("[ps4-savedata-test] Running SaveData tests...\n");

    // Set isolated test directory
    const char *test_dir = "/tmp/ps4_recomp_savedata_test";
    char rm_cmd[256];
    snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf %s", test_dir);
    system(rm_cmd);
    setenv("PS4_SAVEDATA_DIR", test_dir, 1);

    // 1. Initialize
    int32_t ret = sceSaveDataInitialize3(NULL);
    assert(ret == 0);
    printf("[+] sceSaveDataInitialize3 succeeded\n");

    // Create a mock GuestContext with memory
    GuestContext ctx = {0};
    ctx.mem_size = 10 * 1024 * 1024;
    ctx.mem_base = (uint8_t *)calloc(1, ctx.mem_size);
    assert(ctx.mem_base != NULL);

    // 2. Setup memory slot 0 (first time, should be 0 existing size)
    OrbisSaveDataMemorySetup2 setupParam = {0};
    setupParam.userId = 0x10000000;
    setupParam.slotId = 0;
    setupParam.memorySize = 4096;

    OrbisSaveDataMemorySetupResult setupResult = {0};
    ret = sceSaveDataSetupSaveDataMemory2(&ctx, &setupParam, &setupResult);
    assert(ret == 0);
    assert(setupResult.existedMemorySize == 0);
    printf("[+] First setup: existedMemorySize = %zu (expected 0)\n", setupResult.existedMemorySize);

    // 3. Write data
    const char test_str[] = "PS4RECOMP_SAVE_SILKSONG_TEST_DATA_2026";
    size_t test_len = strlen(test_str) + 1;

    // Allocate buffer inside guest memory
    uint64_t guest_src_addr = 0x10000;
    memcpy(ctx.mem_base + guest_src_addr, test_str, test_len);

    uint64_t guest_memdata_addr = 0x20000;
    OrbisSaveDataMemoryData *g_data = (OrbisSaveDataMemoryData *)(ctx.mem_base + guest_memdata_addr);
    g_data->buf = guest_src_addr;
    g_data->bufSize = test_len;
    g_data->offset = 64;

    uint64_t guest_setparam_addr = 0x30000;
    OrbisSaveDataMemorySet2 *g_setParam = (OrbisSaveDataMemorySet2 *)(ctx.mem_base + guest_setparam_addr);
    g_setParam->userId = 0x10000000;
    g_setParam->slotId = 0;
    g_setParam->data = guest_memdata_addr;
    g_setParam->dataNum = 1;

    ret = sceSaveDataSetSaveDataMemory2(&ctx, g_setParam);
    assert(ret == 0);
    printf("[+] sceSaveDataSetSaveDataMemory2 succeeded\n");

    // 4. Sync to disk
    OrbisSaveDataMemorySync syncParam = {0};
    syncParam.userId = 0x10000000;
    syncParam.slotId = 0;
    ret = sceSaveDataSyncSaveDataMemory(&ctx, &syncParam);
    assert(ret == 0);
    printf("[+] sceSaveDataSyncSaveDataMemory succeeded\n");

    // 5. Terminate
    ret = sceSaveDataTerminate();
    assert(ret == 0);
    printf("[+] sceSaveDataTerminate succeeded\n");

    // 6. Re-initialize and verify persistence
    ret = sceSaveDataInitialize3(NULL);
    assert(ret == 0);

    memset(&setupResult, 0, sizeof(setupResult));
    ret = sceSaveDataSetupSaveDataMemory2(&ctx, &setupParam, &setupResult);
    assert(ret == 0);
    assert(setupResult.existedMemorySize >= 4096);
    printf("[+] Re-setup: existedMemorySize = %zu (persisted successfully)\n", setupResult.existedMemorySize);

    // 7. Read back data
    uint64_t guest_dst_addr = 0x40000;
    memset(ctx.mem_base + guest_dst_addr, 0, test_len);

    uint64_t guest_getdata_addr = 0x50000;
    OrbisSaveDataMemoryData *g_getdata = (OrbisSaveDataMemoryData *)(ctx.mem_base + guest_getdata_addr);
    g_getdata->buf = guest_dst_addr;
    g_getdata->bufSize = test_len;
    g_getdata->offset = 64;

    uint64_t guest_getparam_addr = 0x60000;
    OrbisSaveDataMemoryGet2 *g_getParam = (OrbisSaveDataMemoryGet2 *)(ctx.mem_base + guest_getparam_addr);
    g_getParam->userId = 0x10000000;
    g_getParam->slotId = 0;
    g_getParam->data = guest_getdata_addr;

    ret = sceSaveDataGetSaveDataMemory2(&ctx, g_getParam);
    assert(ret == 0);

    const char *read_str = (const char *)(ctx.mem_base + guest_dst_addr);
    assert(strcmp(read_str, test_str) == 0);
    printf("[+] Verified read data: '%s'\n", read_str);

    // Transferring mount (ASTRO BOT 01.007 mounts PPSA01325's 'sce_sdmemory'): the SOURCE title's save, read-only, never the
    // running title's own directory (savecrash: our own memory.dat parsed as the Playroom save crashed the boot).
    ret = sceSaveDataInitialize3(NULL);
    assert(ret == 0);
    {
        const char dn[] = "sce_sdmemory";
        const char tid[] = "PPSA01325";
        memcpy(ctx.mem_base + 0x70000, dn, sizeof dn);
        memcpy(ctx.mem_base + 0x70100, tid, sizeof tid);
        SdTransferringMount *tm = (SdTransferringMount *)(ctx.mem_base + 0x71000);
        memset(tm, 0, sizeof *tm);
        tm->userId = 0x10000000;
        tm->dirName = 0x70000;
        tm->titleId = 0x70100;
        // the running title's own save memory exists (setup above): a transferring mount must not open it
        SdMountResult mr;
        memset(&mr, 0, sizeof mr);
        ret = sceSaveDataTransferringMount(&ctx, tm, &mr);
        assert(ret == (int32_t)SD_ERR_NOT_FOUND);
        printf("[+] transferring mount without a PPSA01325 save: NOT_FOUND 0x%x\n", (unsigned)ret);
        // a source save present: mounted read-only from <save root>/transfer/<titleId>/user_<id>/<dir>
        // (save root here = PS4_SAVEDATA_DIR/DefaultGame: the test has no param.sfo)
        char cmd[768];
        snprintf(cmd, sizeof cmd, "mkdir -p %s/DefaultGame/transfer/PPSA01325/user_10000000/sce_sdmemory/sce_sys && "
                 "printf 'x' > %s/DefaultGame/transfer/PPSA01325/user_10000000/sce_sdmemory/sce_sys/param.sfo && "
                 "printf 'PLAYROOM' > %s/DefaultGame/transfer/PPSA01325/user_10000000/sce_sdmemory/memory.dat", test_dir, test_dir, test_dir);
        assert(system(cmd) == 0);
        memset(&mr, 0, sizeof mr);
        ret = sceSaveDataTransferringMount(&ctx, tm, &mr);
        assert(ret == 0);
        assert(mr.mountPoint.data[0] != 0);
        printf("[+] transferring mount of an existing PPSA01325 save: '%s'\n", mr.mountPoint.data);
        // the old own-directory fallback stays available for A/B
        memcpy(ctx.mem_base + 0x70200, mr.mountPoint.data, sizeof mr.mountPoint.data);
        ret = sceSaveDataUmount(&ctx, 0x70200);
        assert(ret == 0);
        setenv("APS5_SAVEDATA_TRANSFER_FALLBACK", "self", 1);
        memset(&mr, 0, sizeof mr);
        ret = sceSaveDataTransferringMount(&ctx, tm, &mr);
        assert(ret == 0);
        printf("[+] APS5_SAVEDATA_TRANSFER_FALLBACK=self mounts the own dir '%s'\n", mr.mountPoint.data);
        unsetenv("APS5_SAVEDATA_TRANSFER_FALLBACK");
    }

    ret = sceSaveDataTerminate();
    assert(ret == 0);

    free(ctx.mem_base);
    system(rm_cmd);
    printf("[ps4-savedata-test] ALL TESTS PASSED!\n");
    return 0;
}
