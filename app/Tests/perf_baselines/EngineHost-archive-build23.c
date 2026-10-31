/*
 * Fixed pre-batch production archive code from ylt410/GTA build 23.
 * Source commit: 72f81231b12828b5fb001f609b6dcb723207e230
 * Extracted verbatim from app/Source/EngineHost.c. The production integration
 * harness supplies only the host/runtime stubs around these real functions.
 */
static uint32_t pg_rpf_walk(uint64_t entries,uint64_t names,unsigned shift,uint32_t index,char *path,size_t length,int depth,const char *find,uint64_t *found){
 const uint64_t *e=pg_pointer(&env,entries+16ull*index,16);
 const char *name=pg_string(&env,names+((uint64_t)(e[0]&0xFFFF)<<shift));
 size_t n=length;
 if(index){n+=snprintf(path+length,length<480?512-length:0,"%s%s",length?"/":"",name);}
 if(((e[0]>>40)&0x7FFFFF)==0x7FFFFF){
  uint32_t first=(uint32_t)e[1],count=(uint32_t)(e[1]>>32),total=1;
  if(depth>32||count>200000)return total;
  for(uint32_t i=0;i<count;i++)total+=pg_rpf_walk(entries,names,shift,first+i,path,n,depth+1,find,found);
  path[length]=0;return total;
 }
 if(find&&!strcasecmp(path,find)&&!*found)*found=entries+16ull*index;
 path[length]=0;return 1;
}

void pg_packfile_mounted(w2c_game *instance,u64 packfile,u64 name,u32 result){
 (void)instance;
 if(!result)return;
 const char *label=pg_string(&env,name);
 pg_game_index(label,packfile);
 size_t labelLength=strlen(label);
 if(labelLength>8&&!strcasecmp(label+labelLength-7,"dlc.rpf")&&(label[labelLength-8]=='/'||label[labelLength-8]=='\\')){   // "platform:\dlcPacks\<pack>\dlc.rpf"
  uint64_t entries=*(uint64_t *)pg_pointer(&env,packfile+32,8),names=*(uint64_t *)pg_pointer(&env,packfile+16,8);
  unsigned shift=*(uint8_t *)pg_pointer(&env,packfile+184,1);
  int renamed=entries&&names?pg_audio_rel_walk(entries,names,shift,0,0):0;
  if(getenv("PG_TEST_MODS")){char test[400];snprintf(test,sizeof test,"TEST: dlc pack %.300s mounted, %d audio .rel renamed (entries %s).",label,renamed,entries&&names?"ok":"missing");pg_log(test);}
  static _Atomic int told;
  if(renamed&&atomic_fetch_add(&told,1)<16){char line[400];snprintf(line,sizeof line,"Mods: %.300s has PC audio data (.rel only); %d file%s read as the game's own.",label,renamed,renamed==1?"":"s");pg_log(line);}
 }
 const PGModPiece *pieces=NULL;int count=pg_mods_for_archive(label,&pieces);
 if(getenv("PG_TEST_MODS")&&strstr(label,"vehicles.rpf")){char test[400];snprintf(test,sizeof test,"TEST: mounted %.300s, %d mod files.",label,count);pg_log(test);}
 if(!count)return;
 uint64_t entries=*(uint64_t *)pg_pointer(&env,packfile+32,8),names=*(uint64_t *)pg_pointer(&env,packfile+16,8);
 unsigned shift=*(uint8_t *)pg_pointer(&env,packfile+184,1);
 if(!entries||!names)return;
 char line[700];int applied=0;
 for(int i=0;i<count;i++){
  char path[512]={0};uint64_t found=0;
  pg_rpf_walk(entries,names,shift,0,path,0,0,pieces[i].inner,&found);
  uint64_t block=pieces[i].offset/512;
  if(!found){if(atomic_fetch_add(&pg_mods_reported_missing,1)>=24)continue;snprintf(line,sizeof line,"Mods: %.200s has no %.200s to replace; new files inside game archives aren't supported, ignored.",label,pieces[i].inner);pg_log(line);continue;}
  if(block>=0x7FFFFF){snprintf(line,sizeof line,"Mods: %.200s is too large to hold %.200s; ignored.",label,pieces[i].inner);pg_log(line);continue;}
  uint64_t *e=pg_pointer(&env,found,16);
  uint64_t nameOffset=e[0]&0xFFFF;
  if(getenv("PG_TEST_MODS")){snprintf(line,sizeof line,"TEST: %s entry was %016llx %016llx (offset %llu).",pieces[i].inner,(unsigned long long)e[0],(unsigned long long)e[1],(unsigned long long)((e[0]>>40)&0x7FFFFF)*512);pg_log(line);}
  if(pieces[i].resource){
   e[0]=nameOffset|((pieces[i].length&0xFFFFFF)<<16)|(block<<40)|(1ull<<63);
   e[1]=(uint64_t)pieces[i].low|((uint64_t)pieces[i].high<<32);
  } else {
   e[0]=nameOffset|(block<<40);   // stored size 0: stored as is
   e[1]=(uint64_t)(uint32_t)pieces[i].length;
  }
  applied++;
  if(getenv("PG_TEST_MODS")){snprintf(line,sizeof line,"TEST: %s entry now %016llx %016llx (slot at %llu, %llu bytes).",pieces[i].inner,(unsigned long long)e[0],(unsigned long long)e[1],(unsigned long long)pieces[i].offset,(unsigned long long)pieces[i].length);pg_log(line);
   if(i==0){pg_mod_test_entry=found;pg_mod_test_value=e[0];}}
 }
 // The streaming system reloads archive tables often (ReInit); the first openings are enough in the log.
 static _Atomic int reported;
 if(atomic_fetch_add(&reported,1)<24){snprintf(line,sizeof line,"Mods: %.300s opened with %d of %d mod file%s.",label,applied,count,count==1?"":"s");pg_log(line);}
}
