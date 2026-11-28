#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#define GATEWAY_DATABASE_PATH ":memory:"
#include "../gateway-core/src/db_manager.c"
static int rows(const char *table) {
    char sql[128];sqlite3_stmt *s=NULL;
    snprintf(sql,sizeof(sql),"SELECT count(*) FROM %s;",table);
    assert(sqlite3_prepare_v2(g_db,sql,-1,&s,NULL)==SQLITE_OK);
    assert(sqlite3_step(s)==SQLITE_ROW);int n=sqlite3_column_int(s,0);sqlite3_finalize(s);return n;
}
int main(void) {
    const char *mac="01:02:03:04:05:06:07:08",*other="11:12:13:14:15:16:17:18";
    assert(init_database()==0 && insert_new_device(mac,"test")==0 && insert_new_device(other,"test")==0);
    assert(record_collection_temp(mac,42,-2.5f)==1);
    assert(record_collection_mag(mac,42,0,-93.3f,71.7f)==1);
    assert(record_collection_sound(mac,42,1,67)==1);
    assert(record_collection_sound(mac,42,0,68)==1);
    for(unsigned i=0;i<3;i++) {
        assert(record_collection_temp(mac,42,-2.5f)==0);
        assert(record_collection_mag(mac,42,0,-93.3f,71.7f)==0);
        assert(record_collection_sound(mac,42,1,67)==0);
        assert(record_collection_sound(mac,42,0,68)==0);
    }
    assert(rows("temperature_data")==1 && rows("magnetic_data")==1 && rows("sound_data")==2);
    assert(rows("collection_values")==1);
    /* Stable duplicate timestamp; a new batch with the same value is real data. */
    assert(sqlite3_exec(g_db,"UPDATE collection_values SET timestamp=12345;",NULL,NULL,NULL)==SQLITE_OK);
    assert(record_collection_sound(mac,42,1,67)==0);
    CollectionHistoryRecord_t *history=NULL;
    assert(get_collection_history(mac,"digital",30,&history)==1 && history[0].timestamp==12345);free(history);
    assert(record_collection_sound(mac,43,1,67)==1);
    assert(record_collection_sound(other,42,1,67)==1);
    assert(record_collection_sound(mac,42,1,69)==1); /* corrected value is not silently lost */
    assert(rows("sound_data")==5);
    /* A failed history write rolls back the batch value and shadow together. */
    assert(sqlite3_exec(g_db,"CREATE TRIGGER reject_sound BEFORE INSERT ON sound_data BEGIN SELECT RAISE(ABORT,'injected test failure'); END;",NULL,NULL,NULL)==SQLITE_OK);
    assert(record_collection_sound(mac,44,1,70)==-1);
    float values[6]={0};unsigned mask=0;
    assert(!get_collection_values(mac,44,&values[0],NULL,NULL,NULL,NULL,NULL,&mask));
    assert(sqlite3_exec(g_db,"DROP TRIGGER reject_sound;",NULL,NULL,NULL)==SQLITE_OK);
    assert(record_collection_sound(mac,44,1,70)==1);
    assert(record_collection_sound(mac,44,1,70)==0);
    assert(record_collection_temp("missing",1,0)==-1);
    int32_t features[3][5]={{1234}};
    assert(vibration_summary_matches(mac,42,26667,100,features)==0);
    assert(save_vibration_batch(mac,42,26667,100,NULL,0,features)==0);
    assert(vibration_summary_matches(mac,42,26667,100,features)==1);
    features[2][4]=999;
    assert(vibration_summary_matches(mac,42,26667,100,features)==0);
    assert(delete_device(mac)==0 && insert_new_device(mac,"test")==0);
    assert(vibration_summary_matches(mac,42,26667,100,features)==0);
    assert(record_collection_sound(mac,42,1,67)==1);
    close_database();
    puts("Collection ingest: no duplicate history rows, stable timestamp, new batches/corrections, SQL rollback/retry, durable summary and deletion PASS");
    return 0;
}
