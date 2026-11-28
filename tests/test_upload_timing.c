#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define GATEWAY_DATABASE_PATH "upload-timing.db"
#include "../gateway-core/src/db_manager.c"

static unsigned learned;
static uint32_t timing_unit(const char *mac,unsigned raw) {
    UploadTimingRecord_t t;int found=get_upload_timing(mac,raw,&t);assert(found>=0);
    return found?t.mean_unit_ms:0U;
}
static void timing_sample(const char *mac,uint32_t batch,unsigned raw,unsigned blocks,uint64_t elapsed) {
    UploadTimingRecord_t t;assert(save_upload_timing(mac,batch,raw,blocks,elapsed,&t)==1);learned++;
}
#define UPLOAD_ADMISSION_UNIT_MS(mac,raw) timing_unit(mac,raw)
#define UPLOAD_ADMISSION_SAMPLE(e,elapsed) timing_sample((e)->mac,(e)->batch,(e)->raw_required,(e)->blocks,elapsed)
#include "upload_admission.h"

static const char *a="01:02:03:04:05:06:07:08",*b="11:12:13:14:15:16:17:18",*c="21:22:23:24:25:26:27:28";
static void finish(UploadAdmission *q,const char *mac,uint32_t batch,unsigned raw,unsigned blocks,uint64_t start,uint32_t elapsed) {
    assert(!upload_request_kind(q,mac,batch,blocks,raw,start));
    if(raw){upload_raw_seen(q,mac,batch);upload_progress(q,mac,batch,blocks,start+elapsed);}
    upload_finish(q,mac,batch,start+elapsed,"verified success");
}
int main(void) {
    assert(!init_database());assert(!insert_new_device(a,"A") && !insert_new_device(b,"B") && !insert_new_device(c,"C"));
    UploadTimingRecord_t t;assert(!get_upload_timing(a,1,&t));
    UploadAdmission q={0};assert(!upload_request(&q,a,1,27,0));
    assert(q.entries[0].expected_end==86000);upload_remove(&q,a);
    memset(&q,0,sizeof(q));finish(&q,a,2,1,10,100000,20000);
    assert(get_upload_timing(a,1,&t)==1 && t.mean_unit_ms==2000 && t.samples==1 && t.last_duration_ms==20000);
    finish(&q,b,3,1,10,200000,80000);
    assert(get_upload_timing(b,1,&t)==1 && t.mean_unit_ms==8000 && t.samples==1);
    finish(&q,a,4,0,10,300000,1500);
    assert(get_upload_timing(a,0,&t)==1 && t.mean_unit_ms==1500);
    assert(get_upload_timing(a,1,&t)==1 && t.mean_unit_ms==2000 && t.samples==1);
    finish(&q,a,5,1,20,400000,80000);
    assert(get_upload_timing(a,1,&t)==1 && t.mean_unit_ms==2500 && t.samples==2 && t.last_blocks==20);
    /* A duplicate completion/receipt must not create a timing sample. */
    upload_finish(&q,a,5,490000,"duplicate");assert(learned==4);
    assert(save_upload_timing(a,5,1,20,1,&t)==0 && t.samples==2 && t.mean_unit_ms==2500);

    /* Real pending batch durations, per MAC and size, determine sleep replies. */
    assert(!upload_request(&q,a,6,10,500000));
    assert(q.entries[0].expected_end==530000);
    assert(upload_request(&q,b,7,10,500000)==31000);
    assert(upload_request(&q,c,8,10,500000)==116000); /* A 30s + B 85s + 1s */
    uint64_t order=q.entries[1].order;
    assert(upload_request(&q,b,7,10,500010)>0 && q.entries[1].order==order);
    upload_progress(&q,a,6,2,510000); /* Live progress adapts the current owner. */
    assert(upload_request(&q,b,7,10,510000)==46000);
    upload_release_reason(&q,a,6,520000,"failed");
    assert(get_upload_timing(a,1,&t)==1 && t.samples==2 && t.mean_unit_ms==2500);
    assert(upload_request(&q,c,8,10,520000)>0); /* Sleeping head B is preserved. */
    assert(!upload_request(&q,b,7,10,520000));
    upload_revoke(&q,b,7,520100,"rejected");
    assert(get_upload_timing(b,1,&t)==1 && t.samples==1);
    assert(upload_request(&q,c,8,10,521000)>0); /* Old lease drain margin. */
    memset(&q,0,sizeof(q));assert(!upload_request(&q,b,9,10,600000));
    upload_expire(&q,660001);assert(get_upload_timing(b,1,&t)==1 && t.samples==1);
    memset(&q,0,sizeof(q));assert(!upload_request(&q,a,10,10,700000));
    upload_progress(&q,a,10,10,710000); /* Durable replay is not an actual raw transfer. */
    upload_finish(&q,a,10,710000,"already stored");
    assert(get_upload_timing(a,1,&t)==1 && t.samples==2);

    /* A failed statistics write does not overwrite any previous observation. */
    assert(sqlite3_exec(g_db,"CREATE TRIGGER timing_fail BEFORE UPDATE ON upload_timing BEGIN SELECT RAISE(ABORT,'test'); END;",NULL,NULL,NULL)==SQLITE_OK);
    assert(save_upload_timing(a,11,1,10,30000,&t)==-1);
    assert(get_upload_timing(a,1,&t)==1 && t.samples==2);
    assert(sqlite3_exec(g_db,"DROP TRIGGER timing_fail;",NULL,NULL,NULL)==SQLITE_OK);

    /* Simulated process restart: only statistics survive, never queue/owner. */
    memset(&q,0,sizeof(q));assert(!upload_request(&q,a,12,10,800000));
    assert(upload_request(&q,b,13,10,800000)>0);close_database();
    memset(&q,0,sizeof(q));assert(!init_database());
    assert(get_upload_timing(a,1,&t)==1 && t.samples==2 && t.mean_unit_ms==2500);
    assert(!upload_owner(&q,a,12,0) && !upload_owner(&q,b,13,0));
    assert(!upload_request(&q,b,14,20,1000) && q.entries[0].expected_end==166000);
    assert(!delete_device(a) && !get_upload_timing(a,1,&t) && !get_upload_timing(a,0,&t));
    assert(get_upload_timing(b,1,&t)==1 && t.samples==1);
    close_database();
    puts("Upload timing PASS: per-MAC EWMA, raw/features isolation, size scaling, FIFO/live progress, success-only training, duplicate/SQL failure, persistence without leases and deletion");return 0;
}
