#include "NativeEngineConfiguration.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
 const char *models[]={NULL,"","iPhone16,1","iPhone16,2","iPhone15,2","iPad16,1","arm64"};
 const char *invalid[]={NULL,"","0","-1","9","3junk","invalid","999999999999999999999999999999999"};
 for(unsigned m=0;m<sizeof models/sizeof *models;m++){
  unsigned expected=2;
  for(unsigned v=0;v<sizeof invalid/sizeof *invalid;v++)assert(pg_engine_core_configuration(invalid[v],models[m],6)==expected);
  for(unsigned i=1;i<=8;i++){char text[4];snprintf(text,sizeof text,"%u",i);assert(pg_engine_core_configuration(text,models[m],6)==i);}
 }
 assert(pg_engine_default_cores("iPhone16,1",0)==2);
 assert(pg_engine_default_cores("iPhone16,1",2)==2);
 assert(pg_engine_default_cores("iPhone16,1",3)==2);
 assert(pg_engine_default_cores("iPhone16,1",4)==2);
 assert(pg_engine_default_cores("iPhone16,1",6)==2);
 assert(pg_engine_core_configuration("2","iPhone16,1",6)==2);
 puts("PASS: established two-core default on every device, explicit overrides, invalid values, and failed CPU detection");
 return 0;
}
