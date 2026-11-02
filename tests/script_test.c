#define _POSIX_C_SOURCE 200809L
#include "shell.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef struct { char *text; size_t length, capacity; int done, status; } Result;
static void append(Result *r,const char *s,size_t n){if(r->length+n+1>r->capacity){size_t c=r->capacity?r->capacity:1024;while(c<r->length+n+1)c*=2;char*p=realloc(r->text,c);assert(p);r->text=p;r->capacity=c;}memcpy(r->text+r->length,s,n);r->length+=n;r->text[r->length]=0;}
static void drain(int fd){char b[256];while(read(fd,b,sizeof(b))>0){}}
static Result run(Shell*s,int wake,const char*line){Result r={0};assert(shell_submit(s,line)==0);while(!r.done){struct pollfd p={.fd=wake,.events=POLLIN};int n;do{n=poll(&p,1,5000);}while(n<0&&errno==EINTR);assert(n>0);drain(wake);ShellEvent e;while(shell_pop_event(s,&e)){if(e.type==SHELL_EVENT_OUTPUT)append(&r,e.text,e.length);if(e.type==SHELL_EVENT_COMMAND_DONE){r.done=1;r.status=e.status;}shell_event_dispose(&e);}}if(!r.text)r.text=strdup("");assert(r.text);return r;}
static void write_script(const char*path,const char*text){int fd=open(path,O_WRONLY|O_CREAT|O_TRUNC,0700);assert(fd>=0);size_t n=strlen(text),off=0;while(off<n){ssize_t k=write(fd,text+off,n-off);assert(k>0);off+=(size_t)k;}close(fd);}
static void expect(Result*r,const char*s){if(strstr(r->text,s)==NULL)fprintf(stderr,"expected [%s], got [%s] status=%d\n",s,r->text,r->status);assert(strstr(r->text,s)!=NULL);free(r->text);}
static void pause_ms(long ms){struct timespec t={.tv_sec=ms/1000,.tv_nsec=ms%1000*1000000L};while(nanosleep(&t,&t)<0&&errno==EINTR){}}

int main(void){
 char dir[]="/tmp/terminator-script-XXXXXX";assert(mkdtemp(dir));
 char path[512],subdir[512];snprintf(path,sizeof(path),"%s/source.c",dir);write_script(path,"source\n");snprintf(subdir,sizeof(subdir),"%s/sub",dir);assert(mkdir(subdir,0700)==0);
 char script[2048];
 snprintf(path,sizeof(path),"%s/sample.sh",dir);
 write_script(path,"#!/bin/bash\n# task 11 sample\nif [ -f \"$1\" ]; then\n echo \"File exists, compiling...\"\n echo \"$1\"\nelse\n echo \"Source file missing\"\nfi\n");
 int wake[2];assert(pipe(wake)==0);assert(fcntl(wake[0],F_SETFL,O_NONBLOCK)==0);assert(fcntl(wake[1],F_SETFL,O_NONBLOCK)==0);Shell*s=shell_create(wake[1]);assert(s);
 snprintf(script,sizeof(script),"cd %s",dir);Result r=run(s,wake[0],script);free(r.text);
 r=run(s,wake[0],"./sample.sh source.c");assert(strstr(r.text,"File exists, compiling..."));assert(strstr(r.text,"source.c"));free(r.text);
 r=run(s,wake[0],"bash sample.sh missing.c");expect(&r,"Source file missing");

 snprintf(path,sizeof(path),"%s/isolated.sh",dir);write_script(path,"PRIVATE_SCRIPT_VALUE=private; cd /tmp; echo \"$PRIVATE_SCRIPT_VALUE\"; exit 7; echo SHOULD_NOT_RUN\n");
 r=run(s,wake[0],"./isolated.sh");assert(r.status==7);expect(&r,"private");
 r=run(s,wake[0],"pwd");expect(&r,dir);
 r=run(s,wake[0],"bash isolated.sh");assert(r.status==7);assert(strstr(r.text,"private"));free(r.text);r=run(s,wake[0],"pwd");expect(&r,dir);

 snprintf(path,sizeof(path),"%s/set-source.sh",dir);write_script(path,"SOURCE_VALUE=persist; cd sub\n");
 r=run(s,wake[0],"source set-source.sh");assert(r.status==0);free(r.text);
 snprintf(path,sizeof(path),"%s/check-source.sh",dir);write_script(path,"echo \"$SOURCE_VALUE\"; pwd\n");
 r=run(s,wake[0],"source ../check-source.sh");assert(strstr(r.text,"persist"));assert(strstr(r.text,subdir));free(r.text);
 snprintf(script,sizeof(script),"cd %s",dir);r=run(s,wake[0],script);free(r.text);

 snprintf(path,sizeof(path),"%s/grammar.sh",dir);
 write_script(path,"N=2\nif [ \"$N\" -eq 2 ]; then\n if [[ -d sub ]]; then\n  echo branch-one\n fi\nelif [[ \"$N\" -gt 3 ]]; then\n echo wrong\nelse\n echo wrong-too\nfi\nfalse; echo $?\necho \"semi;colon\"\necho '$N'\necho \"$(printf inner)\"\necho \"$(printf \"$(printf nested)\")\"\necho \"$(printf '%s' 'paren ) literal')\"\necho `printf tick`\necho `printf \\`printf deep\\``\necho `printf backtick-multi\n`\necho \"$(printf multi\n)\"\necho $(printf hi | tr a-z A-Z)\necho \"$(cd /tmp; pwd)\"; pwd\necho \"$(SUBST_VAR=inside; echo $SUBST_VAR)\"; echo \"$SUBST_VAR\"\nprintf hello | tr a-z A-Z > pipe.out; cat pipe.out\n");
 r=run(s,wake[0],"bash grammar.sh");if(!strstr(r.text,"branch-one"))fprintf(stderr,"grammar output: %s status %d\n",r.text,r.status);assert(strstr(r.text,"branch-one"));assert(strstr(r.text,"\n1\n"));assert(strstr(r.text,"semi;colon"));assert(strstr(r.text,"$N"));assert(strstr(r.text,"inner"));assert(strstr(r.text,"nested"));assert(strstr(r.text,"tick"));assert(strstr(r.text,"deep"));assert(strstr(r.text,"backtick-multi"));assert(strstr(r.text,"multi"));assert(strstr(r.text,"HI"));assert(strstr(r.text,"/tmp"));assert(strstr(r.text,dir));assert(strstr(r.text,"inside"));assert(strstr(r.text,"HELLO"));free(r.text);

 snprintf(path,sizeof(path),"%s/args.sh",dir);write_script(path,"echo \"$1|$2|$#|$@\"\nprintf '<%s>' \"$@\"\n");
 r=run(s,wake[0],"bash args.sh one two");if(!strstr(r.text,"<one><two>"))fprintf(stderr,"args output: [%s]\n",r.text);assert(strstr(r.text,"one|two|2|one two"));assert(strstr(r.text,"<one><two>"));free(r.text);

 /* Quoted $@ must retain each positional argument as one word, including
  * empty strings, spaces, quotes, backslashes, and dollar signs. */
 snprintf(path,sizeof(path),"%s/args-boundaries.sh",dir);
 write_script(path,"printf '<%s>' \"$@\"\n");
 r=run(s,wake[0],"bash args-boundaries.sh first \"two words\" \"\" \"a\\\"b\" '$literal' \\\\tail");
 assert(strcmp(r.text,"<first><two words><><a\"b><$literal><\\tail>")==0);free(r.text);

 /* then/else bodies on the same line are valid, and semicolons inside
  * quoted strings remain data while unquoted semicolons separate commands. */
 snprintf(path,sizeof(path),"%s/inline-if.sh",dir);
 write_script(path,"if [ 1 -eq 1 ]; then echo inline-then; else echo wrong; fi\nif [[ 1 -gt 2 ]]; then echo wrong; elif [ 2 -eq 2 ]; then echo inline-elif; else echo wrong; fi\nif [[ 1 -gt 2 ]]; then echo wrong; else echo 'literal;semicolon'; fi\n");
 r=run(s,wake[0],"bash inline-if.sh");assert(strstr(r.text,"inline-then"));assert(strstr(r.text,"inline-elif"));assert(strstr(r.text,"literal;semicolon"));assert(strstr(r.text,"wrong")==NULL);free(r.text);

 /* A syntax error in a later nested substitution must be reported at its
  * physical source line and prevent the earlier command from running. */
 snprintf(path,sizeof(path),"%s/bad-nested-late.sh",dir);
 write_script(path,"echo MUST_NOT_RUN\nif [ 1 -eq 1 ]; then\n echo \"$(printf ok\n echo | | cat\n)\"\nfi\n");
 r=run(s,wake[0],"bash bad-nested-late.sh");assert(r.status==2);assert(strstr(r.text,"MUST_NOT_RUN")==NULL);assert(strstr(r.text,"bad-nested-late.sh:4: syntax error:"));free(r.text);

 snprintf(path,sizeof(path),"%s/bad-after-multiline-substitution.sh",dir);
 write_script(path,"echo MUST_NOT_RUN\necho \"$(printf first\n)\" \"$(printf second\n echo | | cat\n)\"\n");
 r=run(s,wake[0],"bash bad-after-multiline-substitution.sh");assert(r.status==2);assert(strstr(r.text,"MUST_NOT_RUN")==NULL);assert(strstr(r.text,"bad-after-multiline-substitution.sh:4: syntax error:"));free(r.text);

 snprintf(path,sizeof(path),"%s/bad-shebang-line.sh",dir);
 write_script(path,"#!/bin/bash\necho MUST_NOT_RUN\necho | | cat\n");
 r=run(s,wake[0],"./bad-shebang-line.sh");assert(r.status==2);assert(strstr(r.text,"MUST_NOT_RUN")==NULL);assert(strstr(r.text,"bad-shebang-line.sh:3: syntax error:"));free(r.text);

 /* Exit ends only the sourced invocation; the interactive worker remains. */
 snprintf(path,sizeof(path),"%s/source-exit.sh",dir);write_script(path,"echo before-source-exit\nexit 9\necho MUST_NOT_RUN\n");
 r=run(s,wake[0],"source source-exit.sh");assert(r.status==9);assert(strstr(r.text,"before-source-exit"));assert(strstr(r.text,"MUST_NOT_RUN")==NULL);free(r.text);
 r=run(s,wake[0],"echo interactive-still-alive");expect(&r,"interactive-still-alive");

 snprintf(path,sizeof(path),"%s/no-export.sh",dir);write_script(path,"TERMINATOR_PRIVATE_SCRIPT_VALUE=hidden; printenv TERMINATOR_PRIVATE_SCRIPT_VALUE; echo still-running\n");
 r=run(s,wake[0],"bash no-export.sh");assert(strstr(r.text,"still-running"));assert(strstr(r.text,"hidden")==NULL);free(r.text);
 snprintf(path,sizeof(path),"%s/assignment.sh",dir);write_script(path,"BASE=\"two words\"; COPY=$BASE; echo \"$COPY\"\nif [ \"$(printf 2)\" -eq 2 ]; then echo substitution-test; fi\n");
 r=run(s,wake[0],"bash assignment.sh");if(!strstr(r.text,"two words"))fprintf(stderr,"assignment output [%s] status %d\n",r.text,r.status);assert(strstr(r.text,"two words"));assert(strstr(r.text,"substitution-test"));free(r.text);
 snprintf(path,sizeof(path),"%s/stderr-subst.sh",dir);write_script(path,"echo \"$(terminator-command-that-does-not-exist)\"; echo after-error\n");
 r=run(s,wake[0],"bash stderr-subst.sh");assert(strstr(r.text,"terminator-command-that-does-not-exist"));assert(strstr(r.text,"after-error"));free(r.text);

 snprintf(path,sizeof(path),"%s/bad-late.sh",dir);write_script(path,"echo MUST_NOT_RUN\nif [ -f x ]; then\n echo nope\n");
 r=run(s,wake[0],"bash bad-late.sh");assert(r.status==2);assert(strstr(r.text,"bad-late.sh:1: syntax error: missing 'fi'")||strstr(r.text,"missing 'fi'"));assert(strstr(r.text,"MUST_NOT_RUN")==NULL);free(r.text);
 snprintf(path,sizeof(path),"%s/bad-early.sh",dir);write_script(path,"echo MUST_NOT_RUN\necho | | cat\n");r=run(s,wake[0],"bash bad-early.sh");assert(r.status==2);assert(strstr(r.text,"MUST_NOT_RUN")==NULL);free(r.text);
 snprintf(path,sizeof(path),"%s/bad-assignment.sh",dir);write_script(path,"echo MUST_NOT_RUN\nBAD=two words\n");r=run(s,wake[0],"bash bad-assignment.sh");assert(r.status==2);assert(strstr(r.text,"MUST_NOT_RUN")==NULL);free(r.text);
 r=run(s,wake[0],"source does-not-exist.sh");assert(r.status!=0);expect(&r,"does-not-exist.sh");
 r=run(s,wake[0],"bash sub");assert(r.status!=0);expect(&r,"sub");

 snprintf(path,sizeof(path),"%s/cancel.sh",dir);write_script(path,"sleep 30\necho MUST_NOT_RUN\n");
 assert(shell_submit(s,"bash cancel.sh")==0);pause_ms(200);assert(shell_signal_foreground(s,0)==0);r=(Result){0};while(!r.done){struct pollfd p={.fd=wake[0],.events=POLLIN};assert(poll(&p,1,5000)>0);drain(wake[0]);ShellEvent e;while(shell_pop_event(s,&e)){if(e.type==SHELL_EVENT_OUTPUT)append(&r,e.text,e.length);if(e.type==SHELL_EVENT_COMMAND_DONE){r.done=1;r.status=e.status;}shell_event_dispose(&e);}}assert(r.text==NULL||strstr(r.text,"MUST_NOT_RUN")==NULL);free(r.text);

 snprintf(path,sizeof(path),"%s/close.sh",dir);write_script(path,"sleep 30\n");assert(shell_submit(s,"source close.sh")==0);pause_ms(200);shell_destroy(s);close(wake[0]);close(wake[1]);
 puts("script interpreter tests passed");return 0;
}
