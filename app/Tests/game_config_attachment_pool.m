#import <Foundation/Foundation.h>
#include <assert.h>
#include <stdlib.h>
extern NSData *PGGameConfigRewrite(NSData *, NSString *, long, long *);

static NSData *fixture(NSString *entries) {
  NSMutableString *s = [[NSString stringWithFormat:@"<?xml version=\"1.0\"?><root>\n%@\n</root>\n", entries] mutableCopy];
  [s appendString:[@" " stringByPaddingToLength:512 withString:@" " startingAtIndex:0]];
  return [s dataUsingEncoding:NSUTF8StringEncoding];
}
static NSUInteger count(NSString *text, NSString *token) { return [text componentsSeparatedByString:token].count - 1; }
int main(int argc, const char **argv) { @autoreleasepool {
  assert(argc == 2); long minimum = strtol(argv[1], NULL, 10); assert(minimum == 500);
  NSData *input = fixture(@"<Any><Item><PoolName>AttachmentExtension</PoolName><PoolSize value=\"290\" /></Item></Any>\n<pc><Item><PoolName>AttachmentExtension</PoolName><PoolSize value=\"348\" /></Item><Item><PoolName>ScriptStore</PoolName><PoolSize value=\"1400\" /></Item></pc>");
  long was = -1; NSData *result = PGGameConfigRewrite(input,@"AttachmentExtension",minimum,&was);
  assert(result && result.length == input.length && was == 348);
  NSString *text = [[NSString alloc] initWithData:result encoding:NSUTF8StringEncoding];
  assert(count(text,@"<PoolSize value=\"500\" />") == 2);
  assert([text containsString:@"<PoolSize value=\"1400\" />"]);
  assert([[[NSXMLParser alloc] initWithData:result] parse]);
  assert(!PGGameConfigRewrite(result,@"AttachmentExtension",minimum,&was));
  assert(was == 500); // Repeated loads do not grow the pool or the file.
  input = fixture(@"<Item><PoolName>AttachmentExtension</PoolName><PoolSize value=\"700\" /></Item><Item><PoolName>AttachmentExtension</PoolName><PoolSize value=\"290\" /></Item>");
  result = PGGameConfigRewrite(input,@"AttachmentExtension",minimum,&was);
  text = [[NSString alloc] initWithData:result encoding:NSUTF8StringEncoding];
  assert(result.length == input.length && was == 700);
  assert(count(text,@"<PoolSize value=\"700\" />") == 1 && count(text,@"<PoolSize value=\"500\" />") == 1);
  assert(!PGGameConfigRewrite(fixture(@"<Item><PoolName>OtherPool</PoolName><PoolSize value=\"348\" /></Item>"),@"AttachmentExtension",minimum,&was));
  unsigned char invalid[] = {255,254};
  assert(!PGGameConfigRewrite([NSData dataWithBytes:invalid length:2],@"AttachmentExtension",minimum,&was));
  puts("Actual gameconfig rewrite: Any+pc, fixed length, higher custom value, XML validity and repeated reads PASS");
}}
