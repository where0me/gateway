#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#define GATEWAY_DATABASE_PATH "join-visibility.db"
#include "../gateway-core/src/db_manager.c"

static int visible(const char *mac)
{
    DeviceRecord_t *records=NULL;int count=get_confirmed_devices(&records),found=0;
    assert(count>=0);
    for(int i=0;i<count;i++)if(!strcmp(records[i].mac,mac))found++;
    free(records);return found;
}
static void sql(const char *query)
{
    char *error=NULL;int rc=sqlite3_exec(g_db,query,NULL,NULL,&error);
    if(rc!=SQLITE_OK)fprintf(stderr,"SQL error: %s\n",error);
    sqlite3_free(error);assert(rc==SQLITE_OK);
}
int main(void)
{
    const char *unknown="01:02:03:04:05:06:07:08";
    const char *ready="11:12:13:14:15:16:17:18";
    const char *history="21:22:23:24:25:26:27:28";
    const char *signal="31:32:33:34:35:36:37:38";
    remove(GATEWAY_DATABASE_PATH);assert(init_database()==0);
    const char *macs[]={unknown,ready,history,signal};
    for(unsigned i=0;i<4;i++)assert(insert_new_device(macs[i],"HY-Sensor")==0);
    assert(update_device_online(unknown,"1234")==0);
    assert(update_device_online(ready,"2345")==0);
    assert(update_device_online(history,"3456")==0);
    assert(update_device_online(signal,"4567")==0);
    assert(update_device_link_telemetry(signal,170,-40,4)==0);
    assert(!visible(unknown) && !visible(signal));
    assert(confirm_device_join(unknown,0x7777)==0 && !visible(unknown));
    assert(confirm_device_join(unknown,0x1234)==1 && !visible(unknown));
    assert(save_device_interview(unknown,"{\"state\":\"ready\"}")==0);
    assert(update_device_link_telemetry(unknown,150,-45,4)==0);
    assert(authorize_device(unknown)==1 && visible(unknown));
    assert(confirm_device_join(unknown,0x1234)==0); /* Idempotent. */
    assert(update_device_offline(unknown)==0 && visible(unknown));
    assert(save_device_interview(unknown,"{\"state\":\"failed\"}")==0);
    assert(visible(unknown)); /* Re-interview failure never hides an established node. */
    close_database();assert(init_database()==0 && visible(unknown));
    assert(delete_device(unknown)==0 && !visible(unknown));
    assert(insert_new_device(unknown,"Unknown Device")==0 && !visible(unknown));
    assert(update_device_online(unknown,"1234")==0);
    assert(save_device_interview(unknown,"{\"state\":\"failed\"}")==0);
    assert(save_device_interview(ready,"{\"state\":\"ready\",\"protocol\":3}")==0);
    assert(upsert_collection_temp(history,1,25.0f)==0);
    /* Recreate an old database without the new column. */
    sql("ALTER TABLE devices DROP COLUMN join_confirmed;");
    sql("ALTER TABLE devices DROP COLUMN user_authorized;");
    close_database();assert(init_database()==0);
    assert(!visible(unknown) && !visible(signal));
    assert(visible(ready) && visible(history));
    assert(is_device_in_db(unknown)); /* Hidden discovery retained for routing. */
    assert(confirm_device_join(unknown,0x1234)==1 && !visible(unknown));
    assert(update_device_model_name(unknown,"HY-Sensor")==0);
    assert(save_device_interview(unknown,"{\"state\":\"ready\"}")==0);
    assert(update_device_link_telemetry(unknown,150,-45,4)==0);
    assert(authorize_device(unknown)==1 && visible(unknown));
    close_database();assert(init_database()==0);
    assert(visible(unknown) && visible(ready) && visible(history) && !visible(signal));
    close_database();assert(remove(GATEWAY_DATABASE_PATH)==0);
    puts("PASS: announce-only hidden; address-bound confirmation; persistence/offline; delete/re-add; conservative legacy migration");
    return 0;
}
