#include <stdio.h>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#define GATEWAY_DATABASE_PATH ":memory:"
#include "../gateway-core/src/db_manager.c"

int main(void)
{
    const char *mac = "01:02:03:04:05:06:07:08", *other = "11:12:13:14:15:16:17:18";
    CollectionHistoryRecord_t *rows = NULL;
    assert(init_database() == 0);
    assert(insert_new_device(mac, "test") == 0 && insert_new_device(other, "other") == 0);
    assert(get_collection_history(mac, "temperature", 30, &rows) == 0 && !rows);
    /* Duplicate uplinks update the same point. Zero and negative values are real data. */
    for (uint32_t batch = 1; batch <= 40; ++batch) {
        assert(upsert_collection_temp(mac, batch, (float)batch) == 0);
        assert(upsert_collection_temp(mac, batch, (float)batch) == 0);
        if (batch % 2 == 0) assert(upsert_collection_sound(mac, batch, 1, (float)batch) == 0);
        if (batch % 3 == 0) assert(upsert_collection_sound(mac, batch, 0, (float)batch) == 0);
        assert(upsert_collection_mag(mac, batch, -(float)batch, 0, (float)batch) == 0);
    }
    assert(upsert_collection_temp(other, 999, 100) == 0);
    /* Controlled timestamps, including records older than the 12h scalar history. */
    assert(sqlite3_exec(g_db, "UPDATE collection_values SET timestamp=1000+batch_id;", NULL,NULL,NULL) == SQLITE_OK);
    assert(get_collection_history(mac, "temperature", 999, &rows) == 30);
    assert(rows[0].batch_id == 11 && rows[29].batch_id == 40);
    assert(rows[0].timestamp == 1011 && rows[29].values[0] == 40);
    assert(!(rows[0].present_mask & (1U << 4))); free(rows);
    assert(get_collection_history(mac, "digital", 30, &rows) == 20);
    assert(rows[0].batch_id == 2 && rows[19].values[4] == 40); free(rows);
    assert(get_collection_history(mac, "ultrasonic", 30, &rows) == 13);
    assert(rows[0].batch_id == 3 && rows[12].values[5] == 39); free(rows);
    assert(get_collection_history(mac, "magnetic", 30, &rows) == 30);
    assert(rows[0].values[1] == -11 && rows[0].values[2] == 0 && rows[0].values[3] == 11); free(rows);
    assert(get_collection_history(mac, "temperature", 1, &rows) == 1 && rows[0].batch_id == 40); free(rows);
    assert(get_collection_history("missing", "temperature", 30, &rows) == 0 && !rows);
    assert(get_collection_history(mac, "invalid", 30, &rows) == -1 && !rows);
    assert(get_collection_history(mac, "temperature", 0, &rows) == -1 && !rows);
    assert(get_collection_history("' OR 1=1 --", "temperature", 30, &rows) == 0 && !rows);
    assert(delete_device(mac) == 0);
    assert(get_collection_history(mac, "temperature", 30, &rows) == 0 && !rows);
    close_database();
    puts("Collection history: latest 30 distinct batches, all metrics, missing/zero/negative data, isolation and deletion PASS");
    return 0;
}
