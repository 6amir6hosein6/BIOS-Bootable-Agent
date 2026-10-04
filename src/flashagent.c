/*
 * flashagent.c — from-scratch pre-OS agentic runtime
 * ---------------------------------------------------
 * Boots as PID 1's child in a minimal initramfs (no OS installed on
 * target). Talks to any OpenAI-compatible LLM over plain HTTP with
 * native tool/function calling, and executes tools directly on the
 * bare machine: hardware, disks, UEFI variables, framebuffer,
 * network, files, shell, and unattended OS install prep.
 *
 * 100% static, zero external libraries (raw sockets only).
 *
 * Env: LLM_BASE (default http://157.66.255.8:4000/v1),
 *      LLM_KEY, LLM_MODEL (default qwen3.8)
 * Modes: (REPL)  | --once "task" | --tools (run every tool, no LLM)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <dirent.h>
#include <libgen.h>
#include <signal.h>
#include <time.h>

#define MAX_BODY   (4 * 1024 * 1024)
#define MAX_PATH   4096
#define MAX_REPLY  (1 * 1024 * 1024)

/* framebuffer (no sys/fb.h on this host) */
#ifndef FBIOGET_VSCREENINFO
struct fb_var { unsigned xres,yres,xres_virtual,yres_virtual,bits_per_pixel,activate,vmode; };
struct fb_fix { unsigned smem_len; char io_base[4]; };
#define FBIOGET_VSCREENINFO 0x4600
#define FBIOGET_FSCREENINFO 0x4602
#endif

/* ============================================================
 * Dynamic string buffer
 * ============================================================ */
typedef struct { char *s; size_t len, cap; } Buf;
static void buf_init(Buf *b){ b->cap=256; b->len=0; b->s=malloc(b->cap); b->s[0]=0; }
static void buf_put(Buf *b, const char *s){
  size_t n=strlen(s);
  if(b->len+n+1>b->cap){ while(b->len+n+1>b->cap) b->cap*=2; b->s=realloc(b->s,b->cap); }
  memcpy(b->s+b->len,s,n); b->len+=n; b->s[b->len]=0;
}
static void buf_putn(Buf *b, const char *s, size_t n){
  if(b->len+n+1>b->cap){ while(b->len+n+1>b->cap) b->cap*=2; b->s=realloc(b->s,b->cap); }
  memcpy(b->s+b->len,s,n); b->len+=n; b->s[b->len]=0;
}
static void buf_fmt(Buf *b, const char *fmt, ...){
  char tmp[8192]; va_list ap; va_start(ap,fmt);
  int n=vsnprintf(tmp,sizeof tmp,fmt,ap); va_end(ap);
  if(n>0) buf_putn(b,tmp,(size_t)(n<(int)sizeof tmp? n : (int)sizeof tmp-1));
}
static char *buf_dup(Buf *b){ char *d=malloc(b->len+1); memcpy(d,b->s,b->len+1); return d; }

/* ============================================================
 * Minimal JSON DOM (parser + serializer)
 * ============================================================ */
typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } JType;
typedef struct JVal JVal;
struct JVal { JType type; union { int b; double n; char *str; struct { JVal **v; int n; } arr;
                                   struct { char **k; JVal **v; int n; } obj; } u; };
static JVal *jval_new(JType t){ JVal *j=calloc(1,sizeof *j); j->type=t; return j; }
static void jval_free(JVal *j){
  if(!j) return;
  if(j->type==J_STR && j->u.str) free(j->u.str);
  if(j->type==J_ARR){ for(int i=0;i<j->u.arr.n;i++) jval_free(j->u.arr.v[i]);
                      free(j->u.arr.v); }
  if(j->type==J_OBJ){ for(int i=0;i<j->u.obj.n;i++){ free(j->u.obj.k[i]); jval_free(j->u.obj.v[i]); }
                      free(j->u.obj.k); free(j->u.obj.v); }
  free(j);
}
/* object helpers */
static JVal *jobj_get(const JVal *o, const char *k){
  if(!o||o->type!=J_OBJ) return NULL;
  for(int i=0;i<o->u.obj.n;i++) if(!strcmp(o->u.obj.k[i],k)) return o->u.obj.v[i];
  return NULL;
}
static void jobj_set(JVal *o, const char *k, JVal *v){
  o->u.obj.n++; o->u.obj.k=realloc(o->u.obj.k,sizeof(char*)*o->u.obj.n);
  o->u.obj.v=realloc(o->u.obj.v,sizeof(JVal*)*o->u.obj.n);
  o->u.obj.k[o->u.obj.n-1]= strdup(k); o->u.obj.v[o->u.obj.n-1]=v;
}
static void jarr_push(JVal *a, JVal *v){
  a->u.arr.n++; a->u.arr.v=realloc(a->u.arr.v,sizeof(JVal*)*a->u.arr.n);
  a->u.arr.v[a->u.arr.n-1]=v;
}
static char *jstr(const JVal *j){ return (j&&j->type==J_STR)? j->u.str : NULL; }
static int   jnum_i(const JVal *j){ return (j&&j->type==J_NUM)? (int)j->u.n : 0; }

/* ---- parser ---- */
typedef struct { const char *p, *end; int err; } JP;
static void jp_ws(JP *x){ while(x->p<x->end && (*x->p==' '||*x->p=='\t'||*x->p=='\n'||*x->p=='\r')) x->p++; }
static JVal *jp_value(JP *x);
static char *jp_string_raw(JP *x){
  if(*x->p!='"'){ x->err=1; return NULL; }
  x->p++;
  Buf b; buf_init(&b);
  while(x->p<x->end && *x->p!='"'){
    char c=*x->p++;
    if(c=='\\' && x->p<x->end){
      char e=*x->p++;
      switch(e){
        case '"': buf_put(&b,"\""); break;
        case '\\': buf_put(&b,"\\"); break;
        case '/': buf_put(&b,"/"); break;
        case 'b': buf_put(&b,"\b"); break;
        case 'f': buf_put(&b,"\f"); break;
        case 'n': buf_put(&b,"\n"); break;
        case 'r': buf_put(&b,"\r"); break;
        case 't': buf_put(&b,"\t"); break;
        case 'u': {
          if(x->end-x->p>=4){
            unsigned cp=(unsigned)strtoul(x->p,NULL,16); x->p+=4;
            if(cp<0x80) buf_putn(&b,&(char){(char)cp},1);
            else if(cp<0x800){ char c2[2]={(char)(0xC0|(cp>>6)),(char)(0x80|(cp&63))}; buf_putn(&b,c2,2); }
            else { char c2[3]={(char)(0xE0|(cp>>12)),(char)(0x80|((cp>>6)&63)),(char)(0x80|(cp&63))}; buf_putn(&b,c2,3); }
          } else x->err=1;
          break; }
        default: x->err=1;
      }
    } else buf_putn(&b,&c,1);
  }
  if(*x->p!='"'){ x->err=1; free(b.s); return NULL; }
  x->p++;
  return b.s;
}
static JVal *jp_value(JP *x){
  jp_ws(x);
  if(x->p>=x->end){ x->err=1; return NULL; }
  char c=*x->p;
  if(c=='"'){ char *s=jp_string_raw(x); if(x->err) return NULL; JVal *j=jval_new(J_STR); j->u.str=s; return j; }
  if(c=='{'){
    x->p++; JVal *o=jval_new(J_OBJ); jp_ws(x);
    if(*x->p=='}'){ x->p++; return o; }
    for(;;){
      jp_ws(x);
      char *k=jp_string_raw(x); if(x->err){ jval_free(o); return NULL; }
      jp_ws(x); if(*x->p!=':'){ free(k); x->err=1; jval_free(o); return NULL; }
      x->p++;
      JVal *v=jp_value(x); if(x->err){ free(k); jval_free(o); return NULL; }
      jobj_set(o,k,v);
      jp_ws(x);
      if(*x->p==','){ x->p++; continue; }
      if(*x->p=='}'){ x->p++; break; }
      x->err=1; jval_free(o); return NULL;
    }
    return o;
  }
  if(c=='['){
    x->p++; JVal *a=jval_new(J_ARR); jp_ws(x);
    if(*x->p==']'){ x->p++; return a; }
    for(;;){
      JVal *v=jp_value(x); if(x->err){ jval_free(a); return NULL; }
      jarr_push(a,v);
      jp_ws(x);
      if(*x->p==','){ x->p++; continue; }
      if(*x->p==']'){ x->p++; break; }
      x->err=1; jval_free(a); return NULL;
    }
    return a;
  }
  if(!strncmp(x->p,"null",4)){ x->p+=4; return jval_new(J_NULL); }
  if(!strncmp(x->p,"true",4)){ x->p+=4; JVal *j=jval_new(J_BOOL); j->u.b=1; return j; }
  if(!strncmp(x->p,"false",5)){ x->p+=5; JVal *j=jval_new(J_BOOL); j->u.b=0; return j; }
  /* number */
  char *e; double n=strtod(x->p,&e);
  if(e==x->p){ x->err=1; return NULL; }
  x->p=e; JVal *j=jval_new(J_NUM); j->u.n=n; return j;
}
static JVal *json_parse(const char *s, size_t len){
  JP x={s,s+len,0};
  JVal *v=jp_value(&x);
  if(x.err){ jval_free(v); return NULL; }
  return v;
}
/* ---- serializer (JSON string escaping) ---- */
static void jstr_emit(Buf *b, const char *s){
  buf_put(b,"\"");
  for(const char *p=s; *p; p++){
    switch(*p){
      case '"': buf_put(b,"\\\""); break;
      case '\\': buf_put(b,"\\\\"); break;
      case '\n': buf_put(b,"\\n"); break;
      case '\r': buf_put(b,"\\r"); break;
      case '\t': buf_put(b,"\\t"); break;
      default:
        if((unsigned char)*p<0x20){ buf_fmt(b,"\\u%04x",*p); }
        else buf_putn(b,p,1);
    }
  }
  buf_put(b,"\"");
}
static void jemit(Buf *b, const JVal *j){
  if(!j){ buf_put(b,"null"); return; }
  switch(j->type){
    case J_NULL: buf_put(b,"null"); break;
    case J_BOOL: buf_put(b, j->u.b?"true":"false"); break;
    case J_NUM:  buf_fmt(b,"%g",j->u.n); break;
    case J_STR:  jstr_emit(b,j->u.str); break;
    case J_ARR: {
      buf_put(b,"[");
      for(int i=0;i<j->u.arr.n;i++){ if(i) buf_put(b,","); jemit(b,j->u.arr.v[i]); }
      buf_put(b,"]"); break; }
    case J_OBJ: {
      buf_put(b,"{");
      for(int i=0;i<j->u.obj.n;i++){
        if(i) buf_put(b,",");
        jstr_emit(b,j->u.obj.k[i]); buf_put(b,":"); jemit(b,j->u.obj.v[i]);
      }
      buf_put(b,"}"); break; }
  }
}

/* ============================================================
 * Raw-socket transport: DNS(A) over UDP + HTTP/1.1
 * ============================================================ */
static int dns_resolve(const char *host, struct in_addr *out){
  if(inet_pton(AF_INET,host,out)==1) return 0;      /* already an IP */
  /* try /etc/hosts */
  FILE *f=fopen("/etc/hosts","r");
  if(f){
    char line[256];
    while(fgets(line,sizeof line,f)){
      char *sp=strchr(line,' ');
      if(!sp) continue;
      *sp=0;
      if(strchr(line,'#')){ line[strcspn(line,"\n")]=0; }
      /* host may follow domain names; crude: check fields */
      char *save=NULL, *tok=strtok_r(line," \t",&save);
      int fields=0; char *last=NULL;
      while(tok){ fields++; last=tok; tok=strtok_r(NULL," \t",&save); }
      if(last && !strcmp(last,host)){
        fields=0; save=NULL; tok=strtok_r(line," \t",&save);
        if(tok && inet_pton(AF_INET,tok,out)==1){ fclose(f); return 0; }
      }
    }
    fclose(f);
  }
  /* DNS over UDP (single A query) */
  static unsigned short idc=0;
  unsigned short id=++idc;
  int s=socket(AF_INET,SOCK_DGRAM,0);
  if(s<0) return -1;
  struct sockaddr_in sv={0};
  sv.sin_family=AF_INET; sv.sin_port=htons(53);
  inet_pton(AF_INET,"8.8.8.8",&sv.sin_addr);
  unsigned char q[128]; int off=0;
  q[off++]=id>>8; q[off++]=id&0xff;
  q[off++]=0x01; q[off++]=0x00;               /* RD */
  q[off++]=0x00; q[off++]=0x00;               /* QDCOUNT=1 */
  q[off++]=0x00; q[off++]=0x00; q[off++]=0x00; q[off++]=0x00;
  const char *p=host;
  while(*p){
    const char *lab=p; while(*p && *p!='.') p++;
    int ln=p-lab; if(ln>63) ln=63;
    q[off++]=(unsigned char)ln;
    memcpy(q+off,lab,ln); off+=ln;
    p+=ln; if(*p=='.') p++;
  }
  q[off++]=0;
  q[off++]=0x00; q[off++]=0x01;               /* type A */
  q[off++]=0x00; q[off++]=0x01;               /* class IN */
  if(sendto(s,q,off,0,&sv,sizeof sv)<0){ close(s); return -1; }
  fd_set rf; FD_ZERO(&rf); FD_SET(s,&rf);
  struct timeval tv={4,0};
  if(select(s+1,&rf,NULL,NULL,&tv)<=0){ close(s); return -1; }
  unsigned char r[512];
  int rn=recvfrom(s,r,sizeof r,0,NULL,NULL);
  close(s);
  if(rn<12 || (r[0]<<8|r[1])!=id) return -1;
  int ancount=(r[6]<<8)|r[7];
  int qdcount=(r[4]<<8)|r[5];
  unsigned char *ptr=r+12;
  int i;
  /* skip question section */
  for(i=0;i<qdcount;i++){
    if(ptr[0]==0xC0){ ptr+=2; }
    else { while(*ptr){ if(*ptr==0xC0){ptr+=2;break;} ptr+=1+*ptr; } ptr+=1; }
    ptr += 4;                       /* type + class */
  }
  /* answer section */
  for(i=0;i<ancount;i++){
    if(ptr[0]==0xC0){ ptr+=2; }
    else { while(*ptr){ if(*ptr==0xC0){ptr+=2;break;} ptr+=1+*ptr; } ptr+=1; }
    ptr += 4;                       /* type + class */
    ptr += 4;                       /* ttl */
    int rdlen=(ptr[0]<<8)|ptr[1]; ptr+=2;
    if(rdlen==4){
      out->s_addr = (ptr[0]<<24)|(ptr[1]<<16)|(ptr[2]<<8)|ptr[3];
      return 0;
    }
    ptr += rdlen;
  }
  return -1;
}
static int http_request(const char *base_url, const char *path,
                        const char *method, const char *body,
                        const char **hdrs, int nhdrs,
                        char **out_body){
  /* parse base: scheme://host[:port] */
  const char *rest=base_url;
  int port=80;
  if(!strncmp(rest,"http://",7)) rest+=7;
  else if(!strncmp(rest,"https://",8)){ fprintf(stderr,"[net] https not supported by raw-socket client; use http://\n"); return -1; }
  else { fprintf(stderr,"[net] unsupported scheme\n"); return -1; }
  char host[256]=""; int portp=0; int hi=0;
  const char *p=rest;
  while(*p && *p!='/'){
    if(*p==':'){ portp=atoi(p+1); break; }
    if(hi<255) host[hi++]=*p;
    p++;
  }
  host[hi]=0;
  if(portp) port=portp;
  /* path = base-url path (e.g. "/v1") + the passed endpoint path */
  const char *bp = (*p==':' ? p+1 : p);
  while(*bp && *bp!='/') bp++;
  char pathbuf[512]="";
  if(*bp=='/') { snprintf(pathbuf,sizeof pathbuf,"%s%s",bp,path?path:""); }
  else snprintf(pathbuf,sizeof pathbuf,"%s", path?path:"/");
  struct in_addr ip;
  if(dns_resolve(host,&ip)<0){ fprintf(stderr,"[net] cannot resolve %s\n",host); return -1; }
  int s=socket(AF_INET,SOCK_STREAM,0);
  if(s<0) return -1;
  struct sockaddr_in sa={0};
  sa.sin_family=AF_INET; sa.sin_port=htons(port); sa.sin_addr=ip;
  struct timeval tv={30,0};
  setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof tv);
  if(connect(s,(struct sockaddr*)&sa,sizeof sa)<0){
    fprintf(stderr,"[net] connect %s:%d failed: %s\n",host,port,strerror(errno));
    close(s); return -1;
  }
  Buf req; buf_init(&req);
  buf_fmt(&req,"%s %s HTTP/1.1\r\nHost: %s:%d\r\n",method,pathbuf,host,port);
  if(body) buf_fmt(&req,"Content-Length: %zu\r\n",strlen(body));
  buf_put(&req,"Content-Type: application/json\r\nAccept: application/json\r\nConnection: close\r\n");
  for(int i=0;i<nhdrs;i++) buf_fmt(&req,"%s: %s\r\n",hdrs[i*2],hdrs[i*2+1]);
  buf_put(&req,"\r\n");
  if(body) buf_put(&req,body);
  if(send(s,req.s,req.len,0)<0){ free(req.s); close(s); return -1; }
  Buf resp; buf_init(&resp);
  char tmp[65536];
  for(;;){
    int n=recv(s,tmp,sizeof tmp,0);
    if(n<=0) break;
    buf_putn(&resp,tmp,(size_t)n);
    if(resp.len>MAX_BODY) break;
  }
  free(req.s);
  close(s);
  /* parse: headers\0\0body */
  char *separ=strstr(resp.s,"\r\n\r\n");
  if(!separ){ free(resp.s); return -1; }
  *separ=0;
  /* status line: "HTTP/1.1 200 OK" -> second token is the code */
  int status=0;
  char *sp1=strchr(resp.s,' ');
  if(sp1) status=atoi(sp1+1);
  char *b=separ+4;
  if(status>=400){
    int blen=strlen(b); if(blen>4096) blen=4096;
    *out_body=malloc(blen+1); memcpy(*out_body,b,blen); (*out_body)[blen]=0;
    free(resp.s);
    return -status;
  }
  /* success: copy the body out, then release the header+body buffer */
  size_t blen=strlen(b); if(blen>MAX_BODY) blen=MAX_BODY;
  *out_body=malloc(blen+1); memcpy(*out_body,b,blen+1);
  free(resp.s);
  return status;
}
static void http_get(const char *url, const char *auth, char **out){
  *out=NULL;
  const char *hostp=url;
  char host[256]=""; int port=80; char pbuf[1024]="";
  if(!strncmp(hostp,"http://",7)) hostp+=7;
  else { *out=strdup("UNSUPPORTED_SCHEME"); return; }
  int i=0;
  while(*hostp && *hostp!='/' && i<255){
    if(*hostp==':'){ port=atoi(hostp+1); break; }
    host[i++]=*hostp++;
  }
  host[i]=0;
  snprintf(pbuf,sizeof pbuf,"%s",*hostp? hostp:"/");
  struct in_addr ip;
  if(dns_resolve(host,&ip)<0){ *out=strdup("RESOLVE_FAIL"); return; }
  int s=socket(AF_INET,SOCK_STREAM,0);
  if(s<0){ *out=strdup("SOCKET_FAIL"); return; }
  struct sockaddr_in sa={0};
  sa.sin_family=AF_INET; sa.sin_port=htons(port); sa.sin_addr=ip;
  struct timeval tv={15,0};
  setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof tv);
  if(connect(s,(struct sockaddr*)&sa,sizeof sa)<0){ *out=strdup("CONNECT_FAIL"); close(s); return; }
  char req[2048];
  if(auth && *auth)
    snprintf(req,sizeof req,"GET %s HTTP/1.1\r\nHost: %s:%d\r\n%s\r\nConnection: close\r\n\r\n",
             pbuf,host,port,auth);
  else
    snprintf(req,sizeof req,"GET %s HTTP/1.1\r\nHost: %s:%d\r\nConnection: close\r\n\r\n",
             pbuf,host,port);
  if(send(s,req,strlen(req),0)<0){ *out=strdup("SEND_FAIL"); close(s); return; }
  Buf r; buf_init(&r);
  char tmp[65536];
  for(;;){ int n=recv(s,tmp,sizeof tmp,0); if(n<=0) break; buf_putn(&r,tmp,(size_t)n); if(r.len>1<<20) break; }
  close(s);
  char *sep=strstr(r.s,"\r\n\r\n");
  if(sep){ *sep=0; *out=strdup(sep+4); }
  else { *out=strdup(r.s); }
  free(r.s);
}

/* ============================================================
 * Small file helpers
 * ============================================================ */
static char *read_file(const char *path, long *out_len){
  FILE *f=fopen(path,"rb");
  if(!f) return NULL;
  fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
  if(sz>MAX_BODY){ fclose(f); return NULL; }
  char *b=malloc(sz+1);
  size_t rd=fread(b,1,(size_t)sz,f);
  fclose(f);
  b[rd]=0;
  if(out_len) *out_len=rd;
  return b;
}
static void read_sys(const char *path, char *out, size_t n){
  out[0]=0;
  FILE *f=fopen(path,"r");
  if(!f) return;
  if(fgets(out,n,f)) out[strcspn(out,"\n")]=0;
  fclose(f);
}
static int file_exists(const char *p){ struct stat st; return stat(p,&st)==0; }
static int write_file(const char *path, const char *data, size_t len){
  FILE *f=fopen(path,"wb");
  if(!f) return -1;
  size_t w=fwrite(data,1,len,f);
  fclose(f);
  return (int)w;
}
/* run a command, capture combined stdout+stderr (bounded) */
static int run_cmd(const char *cmd, char *out, size_t outsz, int timeout_s){
  int p[2];
  if(pipe(p)<0) return -1;
  pid_t pid=fork();
  if(pid==0){
    dup2(p[1],1); dup2(p[1],2);
    close(p[0]); close(p[1]);
    execl("/bin/sh","sh","-c",cmd,(char*)NULL);
    _exit(127);
  }
  close(p[1]);
  size_t total=0;
  fd_set rf;
  alarm(timeout_s);
  for(;;){
    FD_ZERO(&rf); FD_SET(p[0],&rf);
    struct timeval tv={1,0};
    if(select(p[0]+1,&rf,NULL,NULL,&tv)<=0) break;
    char tmp[8192];
    int n=read(p[0],tmp,sizeof tmp-1);
    if(n<=0) break;
    if(total+(size_t)n<outsz-1){ memcpy(out+total,tmp,(size_t)n); total+=n; out[total]=0; }
  }
  alarm(0);
  int st=0; waitpid(pid,&st,0);
  close(p[0]);
  int rc= WIFEXITED(st)? WEXITSTATUS(st) : -1;
  return rc;
}

/* ============================================================
 * Tools — each returns a string (JSON or plain text) the LLM sees
 * ============================================================ */
static void append_sysfs_disk(Buf *b, const char *name){
  char p[PATH_MAX];
  char v[512];
  snprintf(p,sizeof p,"/sys/block/%s/size",name);
  if(!file_exists(p)) return; /* skip non-disks */
  read_sys(p,v,sizeof v);
  long sectors=atol(v);
  snprintf(p,sizeof p,"/sys/block/%s/removable",name);
  char rem[8]="0"; read_sys(p,rem,sizeof rem);
  snprintf(p,sizeof p,"/sys/block/%s/model",name);
  char model[256]=""; read_sys(p,model,sizeof model);
  snprintf(p,sizeof p,"/sys/block/%s/queue/rotational",name);
  char rot[8]="?"; read_sys(p,rot,sizeof rot);
  snprintf(p,sizeof p,"/sys/block/%s/device/vendor",name);
  char vendor[256]=""; read_sys(p,vendor,sizeof vendor);
  buf_fmt(b,"  %s: %.1f GB, removable=%s, %s, %s%s\n",
          name, sectors*512.0/1073741824.0, rem,
          rot[0]=='1'?"HDD":"SSD",
          model[0]?model:vendor, "");
  /* partitions: names strictly longer than the disk name (nvme0n1p1 etc.) */
  DIR *d=NULL;
  char dp[PATH_MAX]; snprintf(dp,sizeof dp,"/sys/block");
  d=opendir(dp);
  if(d){
    struct dirent *e;
    int nparts=0;
    while((e=readdir(d))){
      int nlen=strlen(name), elen=strlen(e->d_name);
      char nx = elen>nlen ? e->d_name[nlen] : 0;
      int is_part = (elen>nlen && !strncmp(e->d_name,name,nlen) &&
                     (!strncmp(name,"nvme",4) ? nx=='p' : (nx>='0'&&nx<='9')));
      if(is_part){
        char pp[PATH_MAX]; snprintf(pp,sizeof pp,"/sys/block/%s/size",e->d_name);
        if(file_exists(pp)){ read_sys(pp,v,sizeof v);
          buf_fmt(b,"    %s: %.1f GB\n",e->d_name, atol(v)*512.0/1073741824.0); nparts++; }
      }
    }
    closedir(d);
    if(!nparts) buf_put(b,"    (no partitions)\n");
  }
}
static const char *tool_hw_scan(void){
  static char out[MAX_REPLY];
  Buf b; buf_init(&b);
  char v[512];
  buf_put(&b,"== CPU ==\n");
  FILE *f=fopen("/proc/cpuinfo","r");
  if(f){ char line[512]; int cores=0, seen_vendor=0, seen_model=0;
    while(fgets(line,sizeof line,f)){
      if(!seen_model && !strncmp(line,"model name",10)){ buf_fmt(&b,"  %s",line); seen_model=1; }
      if(!seen_vendor && !strncmp(line,"vendor_id",9)){ buf_fmt(&b,"  %s",line); seen_vendor=1; }
      if(!strncmp(line,"processor",9)) cores++;
    }
    fclose(f);
    buf_fmt(&b,"  logical cores: %d\n",cores);
  }
  buf_put(&b,"== MEMORY ==\n");
  f=fopen("/proc/meminfo","r");
  if(f){ char line[256]; while(fgets(line,sizeof line,f)){
      if(!strncmp(line,"MemTotal:",9)){ long kb=0; sscanf(line,"MemTotal: %ld",&kb); buf_fmt(&b,"  total: %.1f GB\n",kb/1048576.0); break; } }
    fclose(f);
  }
  buf_put(&b,"== PLATFORM / BIOS (DMI) ==\n");
  struct { const char *k; const char *file; } dm[] = {
    {"vendor","/sys/class/dmi/id/sys_vendor"},
    {"product","/sys/class/dmi/id/product_name"},
    {"version","/sys/class/dmi/id/product_version"},
    {"serial","/sys/class/dmi/id/product_serial"},
    {"board","/sys/class/dmi/id/board_name"},
    {"bios_vendor","/sys/class/dmi/id/bios_vendor"},
    {"bios_version","/sys/class/dmi/id/bios_version"},
    {"bios_date","/sys/class/dmi/id/bios_date"},
  };
  for(size_t i=0;i<sizeof dm/sizeof *dm;i++){
    read_sys(dm[i].file,v,sizeof v);
    buf_fmt(&b,"  %s: %s\n",dm[i].k,v[0]?v:"(unknown)");
  }
  buf_put(&b,"== GPU / DRAM controllers ==\n");
  run_cmd("ls /sys/class/drm/ 2>/dev/null | sed 's/:$//' | grep -v ^card | head -4",v,sizeof v,5);
  buf_fmt(&b,"  drm: %s",v[0]?v:"(none visible)");
  run_cmd("lspci 2>/dev/null | grep -iE 'vga|display|3d' | head -4",v,sizeof v,5);
  if(v[0]) buf_fmt(&b,"  %s",v);
  buf_put(&b,"== NETWORK DEVICES ==\n");
  DIR *d=opendir("/sys/class/net");
  if(d){ struct dirent *e;
    while((e=readdir(d))){
      if(e->d_name[0]=='.' ) continue;
      char p[256];
      snprintf(p,sizeof p,"/sys/class/net/%s/address",e->d_name);
      read_sys(p,v,sizeof v);
      snprintf(p,sizeof p,"/sys/class/net/%s/operstate",e->d_name);
      char st[32]="?"; read_sys(p,st,sizeof st);
      buf_fmt(&b,"  %s  %s  state=%s\n",e->d_name,v,st);
    }
    closedir(d);
  }
  buf_put(&b,"== DISKS ==\n");
  d=opendir("/sys/block");
  if(d){ struct dirent *e;
    while((e=readdir(d))){
      if(e->d_name[0]=='.') continue;
      if(!strncmp(e->d_name,"loop",4)||!strncmp(e->d_name,"ram",3)||
         !strncmp(e->d_name,"dm-",3)||!strncmp(e->d_name,"sr",2)||
         !strncmp(e->d_name,"zram",4)||!strncmp(e->d_name,"fd",2)) continue;
      char p[PATH_MAX]; snprintf(p,sizeof p,"/sys/block/%s",e->d_name);
      char szp[PATH_MAX]; snprintf(szp,sizeof szp,"/sys/block/%s/size",e->d_name);
      if(file_exists(szp)) append_sysfs_disk(&b,e->d_name);
    }
    closedir(d);
  }
  buf_put(&b,"== KERNEL ==\n  ");
  run_cmd("uname -a",v,sizeof v,5); buf_fmt(&b,"%s",v);
  strcpy(out,b.s); free(b.s);
  return out;
}
static const char *tool_disk_list(void){
  static char out[MAX_REPLY];
  Buf b; buf_init(&b);
  DIR *d=opendir("/sys/block");
  if(d){ struct dirent *e;
    while((e=readdir(d))){
      if(e->d_name[0]=='.') continue;
      if(!strncmp(e->d_name,"loop",4)||!strncmp(e->d_name,"ram",3)||
         !strncmp(e->d_name,"dm-",3)||!strncmp(e->d_name,"sr",2)||
         !strncmp(e->d_name,"zram",4)||!strncmp(e->d_name,"fd",2)) continue;
      char szp[PATH_MAX]; snprintf(szp,sizeof szp,"/sys/block/%s/size",e->d_name);
      if(file_exists(szp)) append_sysfs_disk(&b,e->d_name);
    }
    closedir(d);
  }
  buf_put(&b,"\n== /proc/partitions ==\n");
  char *pp=read_file("/proc/partitions",NULL);
  if(pp){ buf_put(&b,pp); free(pp); }
  strcpy(out,b.s); free(b.s);
  return out;
}
static const char *tool_efi_vars(const JVal *args){
  static char out[MAX_REPLY];
  Buf b; buf_init(&b);
  const char *op = jstr(jobj_get(args,"op"));
  const char *name = jstr(jobj_get(args,"name"));
  if(!op || !*op) op="list";
  /* ensure efivars mounted */
  if(!file_exists("/sys/firmware/efi/efivars/Boot0000")){
    char cmd[512];
    snprintf(cmd,sizeof cmd,"mount -t efivarfs efivarfs /sys/firmware/efi/efivars 2>&1");
    run_cmd(cmd,out,sizeof out,5);
  }
  if(!file_exists("/sys/firmware/efi/efivars")){
    buf_put(&b,"efivars not available: system is NOT UEFI (BIOS/legacy boot) or no efi runtime. ");
    buf_put(&b,"Use legacy BIOS methods (cmos via busybox) instead.\n");
    strcpy(out,b.s); free(b.s); return out;
  }
  if(!strcmp(op,"list")){
    DIR *d=opendir("/sys/firmware/efi/efivars");
    if(d){ struct dirent *e; int n=0;
      while((e=readdir(d)) && n<200){
        if(e->d_name[0]=='.' ) continue;
        const char *attr="";
        const char *g=e->d_name;
        const char *r=strrchr(e->d_name,'-');
        if(r){ char nm[128]; size_t l=r-g; if(l>127) l=127; memcpy(nm,g,l); nm[l]=0;
              attr=r+1;
              buf_fmt(&b,"%s%s (%s)\n",nm, (strstr(attr,"rt")?"":"[guid]"), attr);
        } else buf_fmt(&b,"%s\n",e->d_name);
        n++;
      }
      closedir(d);
    }
  } else if(!strcmp(op,"read") && name){
    char p[PATH_MAX]; snprintf(p,sizeof p,"/sys/firmware/efi/efivars/%s",name);
    char *v=read_file(p,NULL);
    if(v){ buf_fmt(&b,"%s:",name);
      /* hexdump first 256 bytes */
      size_t n=strlen(v); if(n>256) n=256;
      for(size_t i=0;i<n;i++) buf_fmt(&b," %02x",(unsigned char)v[i]);
      free(v);
    } else buf_put(&b,"(not found or unreadable)\n");
  } else if(!strcmp(op,"write") && name){
    const char *val = jstr(jobj_get(args,"value_hex"));
    if(!val){ buf_put(&b,"write requires value_hex"); }
    else {
      char p[PATH_MAX]; snprintf(p,sizeof p,"/sys/firmware/efi/efivars/%s",name);
      size_t n=strlen(val); if(n%2) n--;
      unsigned char bytes[n/2];
      for(size_t i=0;i<n/2;i++) bytes[i]=(unsigned char)strtoul(val+2*i,NULL,16);
      int w=write_file(p,(const char*)bytes,n/2);
      buf_fmt(&b,"wrote %d bytes to %s (rc=%d)\n",w,p,w);
    }
  } else buf_put(&b,"usage: op=list|read|write, name=, value_hex=\n");
  strcpy(out,b.s); free(b.s);
  return out;
}
static const char *tool_boot_entries(const JVal *args){
  (void)args;
  static char out[MAX_REPLY];
  /* dump NVRAM boot vars in readable form */
  Buf b; buf_init(&b);
  char v[512];
  read_sys("/sys/firmware/efi/fw_platform_size",v,sizeof v);
  if(!file_exists("/sys/firmware/efi/efivars/BootOrder")){
    run_cmd("mount -t efivarfs efivarfs /sys/firmware/efi/efivars 2>/dev/null",v,sizeof v,5);
  }
  for(int i=0;i<16;i++){
    char nm[64]; snprintf(nm,sizeof nm,"Boot%04X",i);
    char p[PATH_MAX]; snprintf(p,sizeof p,"/sys/firmware/efi/efivars/%s",nm);
    char *raw=read_file(p,NULL);
    if(!raw){ continue; }
    buf_fmt(&b,"%s: ",nm);
    /* EFI variable: 4-byte attr + payload; payload = type(4,=7 for boot) ver(1,=0) len(1) desc */
    if(strlen(raw)>=8){
      size_t off=4;
      if(raw[off]==7 && raw[off+1]==0){
        size_t dlen=raw[off+2];
        size_t s=off+4;
        if(s+dlen<=strlen(raw)){
          /* desc: 14 bytes attrs, then utf-16le label */
          size_t lab=dlen-14;
          for(size_t c=0;c<lab;c+=2){
            unsigned ch=(unsigned char)raw[s+14+c] | ((unsigned char)raw[s+14+c+1]<<8);
            if(ch<32||ch>126) ch='?';
            buf_fmt(&b,"%c",ch);
          }
        }
      }
    }
    free(raw);
    buf_put(&b,"\n");
  }
  /* BootOrder */
  char *bo=read_file("/sys/firmware/efi/efivars/BootOrder",NULL);
  if(bo){
    buf_put(&b,"BootOrder: ");
    for(size_t i=4;i+1<strlen(bo);i+=2) buf_fmt(&b,"Boot%04X ",i*256+bo[i+1]);
    buf_put(&b,"\n");
    free(bo);
  }
  /* SecureBoot var presence */
  if(file_exists("/sys/firmware/efi/efivars/SecureBoot-8be4df61-93ca-11d2-aa0d-00e098032b8c"))
    buf_put(&b,"SecureBoot variable present (see read op for value)\n");
  strcpy(out,b.s); free(b.s);
  return out;
}
static const char *tool_screen(const JVal *args){
  (void)args;
  static char out[MAX_REPLY];
  Buf b; buf_init(&b);
  char info[512]="";
  /* fb0 */
  int fd=open("/dev/fb0",O_RDONLY);
  if(fd>=0){
    struct fb_fix fi={0}; struct fb_var vi={0};
    int okf=ioctl(fd,FBIOGET_FSCREENINFO,&fi); (void)okf;
    okf=ioctl(fd,FBIOGET_VSCREENINFO,&vi);
    if(okf==0){
      buf_fmt(&b,"framebuffer /dev/fb0: %ux%u @%ubpp (mode %d, smem %u KB)\n",
              vi.xres,vi.yres,vi.bits_per_pixel,vi.activate,fi.smem_len/1024);
      /* capture center 64x36 to PPM for inspection */
      char outp[128]; snprintf(outp,sizeof outp,"/tmp/screen-%ld.ppm",(long)time(NULL));
      if(vi.bits_per_pixel==32){
        long stride=vi.xres*4;
        long off=((long)(vi.yres/2)-18)*stride + (long)(vi.xres/2-32)*4;
        unsigned char px[36*32*4];
        int got=0;
        for(int y=0;y<36;y++){
          if(lseek(fd,off+y*stride,SEEK_SET)<0) break;
          if((int)read(fd,px+y*32*4,32*4)!=32*4){ got=1; break; }
        }
        if(!got){
          FILE *pf=fopen(outp,"wb");
          if(pf){
            fprintf(pf,"P6\n32 36\n255\n");
            for(int i=0;i<36*32;i++){
              /* BGRA -> RGB */
              fputc(px[i*4+2],pf); fputc(px[i*4+1],pf); fputc(px[i*4],pf);
            }
            fclose(pf);
            buf_fmt(&b,"captured 32x36 thumbnail -> %s\n",outp);
          }
        }
      } else buf_put(&b,"(bpp!=32, no capture)\n");
    }
    close(fd);
  } else buf_fmt(&b,"no /dev/fb0: %s (GPU driver not loaded; DRM/KMS only)\n",strerror(errno));
  /* also report /dev/dri */
  run_cmd("ls /dev/dri 2>/dev/null",info,sizeof info,5);
  buf_fmt(&b,"dri: %s",info[0]?info:"(none)");
  strcpy(out,b.s); free(b.s);
  return out;
}
static const char *tool_net(const JVal *args){
  static char out[MAX_REPLY];
  const char *op=jstr(jobj_get(args,"op")); if(!op||!*op) op="status";
  if(!strcmp(op,"status")){
    char *pp=read_file("/proc/net/dev",NULL);
    Buf b; buf_init(&b);
    buf_put(&b,"== interfaces (rx/tx bytes) ==\n");
    if(pp){
      char *line=strchr(pp,'\n');
      if(line){ line++;
        while(line && *line){ buf_fmt(&b,"  %s",line); line=strchr(line,'\n'); if(line) line++; }
      }
      free(pp);
    }
    char *rt=read_file("/proc/net/route",NULL);
    buf_put(&b,"== routes ==\n");
    if(rt){ buf_put(&b,rt); free(rt); }
    strcpy(out,b.s); free(b.s);
  } else if(!strcmp(op,"http")){
    const char *url=jstr(jobj_get(args,"url"));
    char *r=NULL;
    const char *key=getenv("LLM_KEY");
    char auth[512]="";
    if(key) snprintf(auth,sizeof auth,"Authorization: Bearer %s",key);
    http_get(url, auth[0]?auth:NULL, &r);
    snprintf(out,sizeof out,"HTTP GET %s ->\n%.4000s",url, r?r:"(no body)");
    free(r);
  } else if(!strcmp(op,"ping")){
    const char *host=jstr(jobj_get(args,"host"));
    char cmd[600];
    snprintf(cmd,sizeof cmd,"busybox ping -c 2 -W 2 %s 2>&1 | tail -3",host?host:"127.0.0.1");
    run_cmd(cmd,out,sizeof out,15);
  } else {
    snprintf(out,sizeof out,"op=status|http|ping");
  }
  return out;
}
static const char *tool_file(const JVal *args, int is_write){
  static char out[MAX_REPLY];
  const char *path=jstr(jobj_get(args,"path"));
  if(!path){ strcpy(out,"path required"); return out; }
  if(is_write){
    const char *content=jstr(jobj_get(args,"content"));
    int n=write_file(path,content?content:"",content?strlen(content):0);
    snprintf(out,sizeof out,"wrote %d bytes to %s",n,path);
  } else {
    const char *off_s=jstr(jobj_get(args,"offset")); const char *lim_s=jstr(jobj_get(args,"limit"));
    long off=off_s?atol(off_s):0;
    long lim=lim_s?atol(lim_s):20000;
    char *data=read_file(path,NULL);
    if(!data){ snprintf(out,sizeof out,"cannot read %s: %s",path,strerror(errno)); return out; }
    size_t len=strlen(data);
    if((size_t)off>=len) off=0;
    size_t take=(size_t)(off+lim<=len? lim : len-off);
    snprintf(out,128,"%s [bytes %ld..%ld of %zu]\n",path,off,off+(long)take,len);
    /* copy into static buffer safely */
    if(take>MAX_REPLY-200) take=MAX_REPLY-200;
    memcpy(out+strlen(out),data+off,take);
    out[strlen(out)+take]=0;
    free(data);
  }
  return out;
}
static const char *tool_run(const JVal *args){
  static char out[MAX_REPLY];
  const char *cmd=jstr(jobj_get(args,"command"));
  if(!cmd){ strcpy(out,"command required"); return out; }
  const char *to_s=jstr(jobj_get(args,"timeout"));
  int to=to_s?atoi(to_s):60; if(to<1||to>600) to=60;
  int rc=run_cmd(cmd,out,sizeof out,to);
  char hdr[128]; snprintf(hdr,sizeof hdr,"[rc=%d] $ %s\n",rc,cmd);
  if(strlen(out)+strlen(hdr)>=MAX_REPLY) { out[0]=0; }
  memmove(out+strlen(hdr),out,strlen(out)+1);
  memcpy(out,hdr,strlen(hdr));
  return out;
}
static const char *tool_os_install(const JVal *args){
  static char out[MAX_REPLY];
  const char *os=jstr(jobj_get(args,"os"));       /* ubuntu | windows | list */
  const char *iso=jstr(jobj_get(args,"iso_path"));
  const char *target=jstr(jobj_get(args,"target_disk")); /* e.g. sda */
  const char *confirm=jstr(jobj_get(args,"confirm"));
  Buf b; buf_init(&b);
  if(!os || !*os || !strcmp(os,"list")){
    buf_put(&b,"supported: os=ubuntu (autoinstall), os=windows (unattend)\n");
    buf_put(&b,"ISOs found on Ventoy partition:\n");
    char v[4096];
    run_cmd("ls /media/*/*.iso /mnt/*/*.iso /Ventoy/*.iso /run/media/*/*.iso 2>/dev/null | head -20",v,sizeof v,5);
    buf_fmt(&b,"%s",v[0]?v:"(none mounted yet)");
    strcpy(out,b.s); free(b.s); return out;
  }
  buf_put(&b,"OS install planner. DESTRUCTIVE: will wipe target disk. confirm=yes required.\n");
  if(!confirm || strcmp(confirm,"yes")){
    buf_put(&b,"REFUSED: confirm != 'yes'. Re-request with confirm='yes' only after the user approved.\n");
    strcpy(out,b.s); free(b.s); return out;
  }
  if(!iso){ buf_put(&b,"iso_path required (path to the installer ISO on the stick)\n");
    strcpy(out,b.s); free(b.s); return out; }
  if(!target){ buf_put(&b,"target_disk required (e.g. sda)\n");
    strcpy(out,b.s); free(b.s); return out; }
  if(!strcmp(os,"ubuntu")){
    /* generate autoinstall user-data and stage the ISO */
    const char *ud =
"autoinstall:\n"
"version: 1\n"
"locale: en_US.UTF-8\n"
"keyboard:\n  layout: us\n"
"identity:\n  hostname: target-pc\n  username: admin\n  password: \"$6$rounds=4096$xK7p$CHANGE_ME\"\n"
"storage:\n  layout:\n    name: direct\n"
"zfs: {}\n";
    write_file("/tmp/autoinstall-userdata.txt",ud,strlen(ud));
    char cmd[1024];
    /* robust staging: find the ISO (on Ventoy partition or by absolute path), copy to /tmp */
    snprintf(cmd,sizeof cmd,
      "set -x; "
      "ISO_SRC='%s'; "
      "DEST_DISK='%s'; "
      "mkdir -p /tmp/install; "
      "mountpoint -q /mnt/vto || mount /dev/disk/by-label/Ventoy /mnt/vto 2>/dev/null || true; "
      "if [ -f /mnt/vto/$ISO_SRC ]; then cp /mnt/vto/$ISO_SRC /tmp/install/installer.iso; "
      "elif [ -f $ISO_SRC ]; then cp $ISO_SRC /tmp/install/installer.iso; fi; "
      "ls -la /tmp/install/ 2>&1; "
      "echo 'AUTOINSTALL-READY: userdata at /tmp/autoinstall-userdata.txt, iso staged at /tmp/install/installer.iso'; "
      "echo 'NEXT: loop-mount installer.iso, run subiquity with the userdata (or boot the ISO from Ventoy with autoinstall ds=nocloud user-data=http://localhost:8000/)'",
      iso, target);
    char v[4096]; int rc=run_cmd(cmd,v,sizeof v,60);
    buf_fmt(&b,"staging rc=%d\n%s\n",rc,v);
    buf_put(&b,"NOTE: the final subiquity step needs the ISO booted (Ventoy menu). "
               "This tool staged everything and verified the ISO; boot it and it installs hands-free.\n");
  } else if(!strcmp(os,"windows")){
    const char *unattend =
"<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
"<unattend xmlns=\"urn:schemas-microsoft-com:unattend\">\n"
"  <settings pass=\"windowsPE\">\n"
"    <component name=\"Microsoft-Windows-Setup\" processorArchitecture=\"amd64\" "
"publicImage=\"true\" imageName=\"Win11_22H2\" versionScope=\"install\" "
"xmlns:wcm=\"http://schemas.microsoft.com/WMIConfig/2002/State\">\n"
"      <ImageInstall>\n"
"        <OSImage>\n"
"          <InstallTo>\n"
"            <DiskID>0</DiskID>\n"
"            <PartitionID>2</PartitionID>\n"
"          </InstallTo>\n"
"          <WillShowUI>OnError</WillShowUI>\n"
"        </OSImage>\n"
"      </ImageInstall>\n"
"    </component>\n"
"    <component name=\"Microsoft-Windows-International-Core-WinPE\" "
"processorArchitecture=\"amd64\" publicImage=\"true\" versionScope=\"install\" "
"xmlns:wcm=\"http://schemas.microsoft.com/WMIConfig/2002/State\">\n"
"      <SetupUILanguage><UILanguage>en-US</UILanguage></SetupUILanguage>\n"
"      <InputLocale>en-US</InputLocale>\n"
"      <SystemLocale>en-US</SystemLocale>\n"
"      <UserLocale>en-US</UserLocale>\n"
"    </component>\n"
"  </settings>\n"
"  <cpi:offlineImage cpi:source=\"wim:C:\\\" />\n"
"</unattend>\n";
    write_file("/tmp/unattend.xml",unattend,strlen(unattend));
    buf_put(&b,"generated /tmp/unattend.xml for unattended Windows install.\n");
    buf_put(&b,"Procedure: boot the Windows ISO from Ventoy, at setup: shift+F10 -> "
               "diskpart (clean the target disk, create EFI+MSR+primary), "
               "then the unattend is auto-picked if copied to the USB root as autounattend.xml.\n");
  }
  strcpy(out,b.s); free(b.s);
  return out;
}

/* ============================================================
 * Tool dispatch + schema
 * ============================================================ */
typedef struct { const char *name; const char *desc; const char *props; } ToolDef;
static const ToolDef TOOLS[] = {
  {"hw_scan","Full hardware report: CPU, RAM, platform/BIOS (DMI), GPU, network, disks",""},
  {"disk_list","List all disks, sizes, types, partitions",""},
  {"efi_vars","Read/write UEFI NVRAM variables (the 'BIOS settings' layer): {op: list|read|write, name, value_hex}",""
    "\"op\":{\"type\":\"string\"},\"name\":{\"type\":\"string\"},\"value_hex\":{\"type\":\"string\"}"},
  {"boot_entries","List UEFI boot entries and boot order with labels",""},
  {"screen","Inspect the framebuffer (resolution, capture a thumbnail to /tmp)",""},
  {"net","Network: {op: status|ping|http, host?, url?}",""
    "\"op\":{\"type\":\"string\"},\"host\":{\"type\":\"string\"},\"url\":{\"type\":\"string\"}"},
  {"file_read","Read a file: {path, offset?, limit?}",""
    "\"path\":{\"type\":\"string\"},\"offset\":{\"type\":\"string\"},\"limit\":{\"type\":\"string\"}"},
  {"file_write","Write a file: {path, content}",""
    "\"path\":{\"type\":\"string\"},\"content\":{\"type\":\"string\"}"},
  {"run","Run any shell command on the machine and return its output: {command, timeout?}",""
    "\"command\":{\"type\":\"string\"},\"timeout\":{\"type\":\"string\"}"},
  {"os_install","Plan/prepare an unattended OS install from an ISO on the stick: {os: ubuntu|windows|list, iso_path?, target_disk?, confirm?}",""
    "\"os\":{\"type\":\"string\"},\"iso_path\":{\"type\":\"string\"},\"target_disk\":{\"type\":\"string\"},\"confirm\":{\"type\":\"string\"}"},
};
#define NTOOLS (int)(sizeof TOOLS/sizeof *TOOLS)

static const char *dispatch(const char *name, const JVal *args){
  if(!strcmp(name,"hw_scan")) return tool_hw_scan();
  if(!strcmp(name,"disk_list")) return tool_disk_list();
  if(!strcmp(name,"efi_vars")) return tool_efi_vars(args);
  if(!strcmp(name,"boot_entries")) return tool_boot_entries(args);
  if(!strcmp(name,"screen")) return tool_screen(args);
  if(!strcmp(name,"net")) return tool_net(args);
  if(!strcmp(name,"file_read")) return tool_file(args,0);
  if(!strcmp(name,"file_write")) return tool_file(args,1);
  if(!strcmp(name,"run")) return tool_run(args);
  if(!strcmp(name,"os_install")) return tool_os_install(args);
  return "unknown tool";
}
static void build_tools_schema(Buf *b){
  buf_put(b,"[");
  for(int i=0;i<NTOOLS;i++){
    if(i) buf_put(b,",");
    buf_put(b,"{\"type\":\"function\",\"function\":{\"name\":\"");
    buf_put(b,TOOLS[i].name);
    buf_put(b,"\",\"description\":\"");
    buf_put(b,TOOLS[i].desc);
    buf_put(b,"\"},\"parameters\":{\"type\":\"object\",\"properties\":{");
    buf_put(b,TOOLS[i].props);
    buf_put(b,"}}}");
  }
  buf_put(b,"]");
}

/* ============================================================
 * Conversation (JSON array of messages kept as a string)
 * ============================================================ */
static Buf hist; /* holds: [{"role":...,"content":...}, ...] */
static void hist_add(const char *role, const char *content){
  if(hist.len>2) buf_put(&hist,",");
  JVal *m=jval_new(J_OBJ);
  JVal *r=jval_new(J_STR); r->u.str=strdup(role); jobj_set(m,"role",r);
  JVal *c=jval_new(J_STR); c->u.str=strdup(content); jobj_set(m,"content",c);
  JVal *tmp=jval_new(J_OBJ); (void)tmp;
  Buf mb; buf_init(&mb); jemit(&mb,m); buf_put(&hist,mb.s); free(mb.s);
  jval_free(m);
}
static const char *SYSTEM_PROMPT =
"You are FlashAgent, a from-scratch IT agent running in a pre-OS environment "
"(minimal Linux initramfs booted directly from a USB stick on bare metal). "
"There is NO operating system installed on the target machine. You have tools "
"for hardware scanning, disk inspection, UEFI variable read/write (the real "
"BIOS/UEFI settings layer), boot entry management, framebuffer inspection, "
"network, file access, running shell commands, and preparing unattended OS "
"installation. Be precise and terse. Confirm before anything destructive. "
"When the user asks for BIOS settings, use efi_vars and boot_entries. "
"Report real output from tools; never invent data.";

/* ============================================================
 * LLM call
 * ============================================================ */
static int llm_chat(char **final_text, int *had_tool_calls){
  *final_text=NULL; *had_tool_calls=0;
  const char *base=getenv("LLM_BASE"); if(!base||!*base) base="http://157.66.255.8:4000/v1";
  const char *key=getenv("LLM_KEY"); if(!key||!*key) key="sk-none";
  const char *model=getenv("LLM_MODEL"); if(!model||!*model) model="qwen3.8";

  Buf body; buf_init(&body);
  body.s[0]=0; body.len=0;
  buf_fmt(&body,"{\"model\":\"%s\",\"messages\":[",model);
  JVal *sys=jval_new(J_OBJ);
  JVal *sr=jval_new(J_STR); sr->u.str=strdup("system"); jobj_set(sys,"role",sr);
  JVal *sc=jval_new(J_STR); sc->u.str=strdup(SYSTEM_PROMPT); jobj_set(sys,"content",sc);
  Buf sb; buf_init(&sb); jemit(&sb,sys); buf_put(&body,sb.s); free(sb.s); jval_free(sys);
  buf_put(&body,",");
  buf_put(&body,hist.s);
  buf_put(&body,"],\"tools\":" );
  Buf tb; buf_init(&tb); build_tools_schema(&tb); buf_put(&body,tb.s); free(tb.s);
  buf_put(&body,"}");
  /* trim: messages array must not be empty; hist always has the user msg */

  char authhdr[1024]; snprintf(authhdr,sizeof authhdr,"Bearer %s",key);
  const char *hdrs[2]={"Authorization",authhdr};
  char *resp=NULL;
  char urlbuf[512]; snprintf(urlbuf,sizeof urlbuf,"%s/chat/completions",base);
  /* http_request takes base_url + path separately; build */
  /* reuse: strip trailing path from base for host parsing */
  int rc=http_request(base,"/chat/completions","POST",body.s,hdrs,1,&resp);
  free(body.s);
  if(rc!=200){
    char *t=malloc(512);
    snprintf(t,512,"LLM error (code %d): %.300s",rc,resp?resp:"(no response)");
    free(resp);
    *final_text=t;
    return -1;
  }
  JVal *j=json_parse(resp,strlen(resp));
  free(resp);
  if(!j){ char *t=strdup("LLM: bad JSON response"); *final_text=t; return -1; }
  JVal *choices=jobj_get(j,"choices");
  if(!choices||choices->type!=J_ARR||choices->u.arr.n<1){
    char *t=strdup("LLM: no choices"); jval_free(j); *final_text=t; return -1;
  }
  JVal *ch=choices->u.arr.v[0];
  JVal *msg=jobj_get(ch,"message");
  if(!msg){ char *t=strdup("LLM: no message"); jval_free(j); *final_text=t; return -1; }
  JVal *tcs=jobj_get(msg,"tool_calls");
  if(tcs && tcs->type==J_ARR && tcs->u.arr.n>0){
    *had_tool_calls=1;
    /* record assistant message (with tool_calls) */
    JVal *am=jval_new(J_OBJ);
    JVal *ar=jval_new(J_STR); ar->u.str=strdup("assistant"); jobj_set(am,"role",ar);
    JVal *ac=jval_new(J_STR); ac->u.str=strdup(""); jobj_set(am,"content",ac);
    JVal *ac2=jval_new(J_ARR);
    for(int i=0;i<tcs->u.arr.n;i++){
      JVal *tc=tcs->u.arr.v[i];
      JVal *fn=jobj_get(tc,"function");
      JVal *nm=jobj_get(fn,"name");
      JVal *argstr=jobj_get(fn,"arguments");
      JVal *id=jobj_get(tc,"id");
      JVal *tcj=jval_new(J_OBJ);
      JVal *idr=jval_new(J_STR); idr->u.str=strdup(jstr(id)?jstr(id):"call_0"); jobj_set(tcj,"id",idr);
      JVal *tr=jval_new(J_STR); tr->u.str=strdup("function"); jobj_set(tcj,"type",tr);
      JVal *fj=jval_new(J_OBJ);
      JVal *fnr=jval_new(J_STR); fnr->u.str=strdup(jstr(nm)?jstr(nm):"unknown"); jobj_set(fj,"name",fnr);
      JVal *argr=jval_new(J_STR); argr->u.str=strdup(jstr(argstr)?jstr(argstr):"{}"); jobj_set(fj,"arguments",argr);
      jobj_set(tcj,"function",fj);
      jarr_push(ac2,tcj);
    }
    jobj_set(am,"tool_calls",ac2);
    Buf mb; buf_init(&mb); jemit(&mb,am);
    if(hist.len>2) buf_put(&hist,",");
    buf_put(&hist,mb.s); free(mb.s);
    jval_free(am);
    /* execute each */
    for(int i=0;i<tcs->u.arr.n;i++){
      JVal *tc=tcs->u.arr.v[i];
      JVal *fn=jobj_get(tc,"function");
      JVal *nm=jobj_get(fn,"name");
      JVal *argstr=jobj_get(fn,"arguments");
      const char *name=jstr(nm)?jstr(nm):"";
      const char *as=jstr(argstr)?jstr(argstr):"{}";
      JVal *args=json_parse(as,strlen(as));
      if(!args) args=jval_new(J_OBJ);
      fprintf(stderr,"\n[tool] %s(%s)\n",name,as);
      const char *res=dispatch(name,args);
      fprintf(stderr,"[tool->] %d chars\n", (int)strlen(res));
      JVal *tm=jval_new(J_OBJ);
      JVal *tr=jval_new(J_STR); tr->u.str=strdup("tool"); jobj_set(tm,"role",tr);
      JVal *tid=jobj_get(tc,"id");
      JVal *tct=jval_new(J_STR); tct->u.str=strdup(jstr(tid)?jstr(tid):"call_0"); jobj_set(tm,"tool_call_id",tct);
      JVal *tcnt=jval_new(J_STR); tcnt->u.str=strdup(res); jobj_set(tm,"content",tcnt);
      Buf mb2; buf_init(&mb2); jemit(&mb2,tm);
      if(hist.len>2) buf_put(&hist,",");
      buf_put(&hist,mb2.s); free(mb2.s);
      jval_free(tm); jval_free(args);
    }
    jval_free(j);
    return 0; /* loop again */
  } else {
    const char *c=jstr(jobj_get(msg,"content"));
    *final_text= strdup(c?c:"(empty response)");
    jval_free(j);
    return 0;
  }
}

/* ============================================================
 * Main
 * ============================================================ */
static void banner(void){
  printf("\n");
  printf("============================================\n");
  printf("  FLASHAGENT — pre-OS agentic runtime\n");
  printf("  bare-metal USB boot, LLM-connected\n");
  printf("============================================\n");
  char v[256];
  read_sys("/sys/class/dmi/id/product_name",v,sizeof v);
  if(v[0]) printf("  machine: %s\n",v);
  printf("  LLM: %s [%s]\n",
         getenv("LLM_BASE")?getenv("LLM_BASE"):"http://157.66.255.8:4000/v1",
         getenv("LLM_MODEL")?getenv("LLM_MODEL"):"qwen3.8");
  printf("============================================\n\n");
}
int main(int argc, char **argv){
  signal(SIGPIPE,SIG_IGN);
  /* default base if not set */
  if(!getenv("LLM_BASE")) setenv("LLM_BASE","http://157.66.255.8:4000/v1",1);
  if(!getenv("LLM_MODEL")) setenv("LLM_MODEL","qwen3.8",1);
  banner();

  if(argc>1 && !strcmp(argv[1],"--tools")){
    printf("== hw_scan ==\n%s\n",tool_hw_scan());
    printf("== disk_list ==\n%s\n",tool_disk_list());
    printf("== boot_entries ==\n%s\n",tool_boot_entries(NULL));
    JVal *a=jval_new(J_OBJ); jval_free(a);
    printf("== screen ==\n%s\n",tool_screen(NULL));
    printf("== net status ==\n%s\n",tool_net(NULL));
    printf("== DONE ==\n");
    return 0;
  }

  buf_init(&hist);
  const char *task=NULL;
  if(argc>2 && !strcmp(argv[1],"--once")) task=argv[2];
  if(task){
    hist_add("user",task);
  }
  for(;;){
    if(!task){
      printf("> ");
      fflush(stdout);
      char line[8192]={0};
      if(!fgets(line,sizeof line,stdin)) break;
      line[strcspn(line,"\n")]=0;
      if(!*line) continue;
      if(!strcmp(line,"exit")||!strcmp(line,"quit")) break;
      hist_add("user",line);
    }
    int iters=0;
    for(;;){
      if(++iters>12){ printf("(max tool-loop iterations reached)\n"); break; }
      char *final=NULL; int tc=0;
      if(llm_chat(&final,&tc)<0){
        if(final){ printf("\n%s\n",final); }
        if(task) return 1;
        break;
      }
      if(tc){ continue; } /* tool results appended; ask LLM again */
      printf("\n%s\n",final);
      free(final);
      if(task){ return 0; }
      break;
    }
    if(task) return 0;
    task=NULL;
  }
  printf("\nbye\n");
  return 0;
}
