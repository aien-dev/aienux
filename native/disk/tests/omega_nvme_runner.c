/* SPDX-License-Identifier: Apache-2.0
 * Verification-only byte transport and existing C model/oracle, not a driver.
 * Never accepts a block device. QEMU runs stopped with a disposable regular image.
 */
#define _POSIX_C_SOURCE 200809L
#define main original_reference_tests_main
#include "nvme_test.c"
#undef main
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#include <poll.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <time.h>

static int qfd = -1, outfd, infd, errors, published, reported, retained;
static pid_t qp = -1, child = -1;
static char tempdir[128], image[180], socketpath[180], qlog[180];
static uint8_t expected[8192];
static size_t wanted;
static const char *scenario;
static int result_code = -1;
static void die(const char *s) { perror(s); exit(2); }
static void check(int b, const char *s) { if (!b) { fprintf(stderr,"FAIL %s: %s\n",scenario,s); errors++; } }
static int exact(int fd, void *p, size_t n, int writing)
{
    size_t done = 0;
    while (done < n) {
        struct pollfd f = {fd, writing ? POLLOUT : POLLIN, 0};
        if (poll(&f,1,3000) != 1) return -1;
        ssize_t r = writing ? write(fd,(char *)p+done,n-done) : read(fd,(char *)p+done,n-done);
        if (r <= 0) return -1;
        done += (size_t)r;
    }
    return 0;
}
static void cleanup(void)
{
    if (child > 0) { kill(child,SIGKILL); waitpid(child,NULL,0); }
    if (qp > 0) { kill(qp,SIGTERM); waitpid(qp,NULL,0); }
    if (qfd >= 0) close(qfd);
    if (*image) unlink(image);
    if (*socketpath) unlink(socketpath);
    /* Keep QEMU stderr as an artifact when requested by the caller. */
    if (*qlog) unlink(qlog);
    if (*tempdir) rmdir(tempdir);
}
static uint64_t qt(const char *fmt,...)
{
    char line[512], response[256];
    va_list a; va_start(a,fmt); vsnprintf(line,sizeof line,fmt,a); va_end(a);
    size_t n = strlen(line); line[n++]='\n';
    if (exact(qfd,line,n,1)) die("qtest send");
    n=0;
    do { if (n+1 >= sizeof response || exact(qfd,response+n,1,0)) die("qtest response"); } while (response[n++]!='\n');
    response[n]=0;
    if (strncmp(response,"OK",2)) { fprintf(stderr,"qtest: %s",response); exit(2); }
    return response[2]==' ' ? strtoull(response+3,NULL,0) : 0;
}
static void start_qemu(void)
{
    strcpy(tempdir,"/tmp/omega-nvme-XXXXXX");
    if (!mkdtemp(tempdir)) die("mkdtemp");
    snprintf(image,sizeof image,"%s/namespace.raw",tempdir);
    snprintf(socketpath,sizeof socketpath,"%s/qtest",tempdir);
    snprintf(qlog,sizeof qlog,"%s/qemu.log",tempdir);
    int fd=open(image,O_CREAT|O_EXCL|O_RDWR,0600); if (fd<0) die("image");
    struct stat st; if (fstat(fd,&st) || !S_ISREG(st.st_mode)) die("regular image");
    uint8_t block[4096];
    for (size_t pos=0;pos<4096u*512u;pos+=sizeof block) {
        for (size_t j=0;j<sizeof block;j++) block[j]=(uint8_t)(7u*37u+(pos+j)*13u+((pos+j)>>8));
        if (write(fd,block,sizeof block)!=(ssize_t)sizeof block) die("image fixture");
    }
    close(fd);
    char drive[256], qtest[256];
    snprintf(drive,sizeof drive,"if=none,id=ns,file=%s,format=raw",image);
    snprintf(qtest,sizeof qtest,"unix:%s,server=on,wait=off",socketpath);
    qp=fork(); if (qp<0) die("fork qemu");
    if (!qp) {
        int log=open(qlog,O_CREAT|O_WRONLY,0600); dup2(log,2); close(log);
        execlp("qemu-system-aarch64","qemu-system-aarch64","-machine","virt","-accel","tcg","-S","-m","64M","-nodefaults","-display","none","-monitor","none","-serial","none","-qtest",qtest,"-drive",drive,"-device","nvme,drive=ns,serial=OMEGA-FIXTURE-000001,addr=1.0",NULL);
        _exit(127);
    }
    struct sockaddr_un sa={.sun_family=AF_UNIX}; strcpy(sa.sun_path,socketpath);
    struct timespec nap={0,10000000};
    for (int i=0;i<300;i++) {
        qfd=socket(AF_UNIX,SOCK_STREAM,0);
        if (connect(qfd,(struct sockaddr *)&sa,sizeof sa)==0) break;
        close(qfd); qfd=-1; nanosleep(&nap,NULL);
    }
    if (qfd<0) die("QEMU qtest socket");
    /* virt ECAM, bus0/dev1/function0. PCI binding is verifier setup, not .om NVMe logic. */
    uint64_t ecam=0x4010008000ull;
    check((qt("readl 0x%llx",(unsigned long long)ecam)&0xffff)==0x1b36,"QEMU NVMe vendor");
    qt("writel 0x%llx 0x10000004",(unsigned long long)(ecam+0x10));
    qt("writel 0x%llx 0",(unsigned long long)(ecam+0x14));
    qt("writew 0x%llx 6",(unsigned long long)(ecam+4));
}
static int iscase(const char *s) { return !strcmp(scenario,s); }
static void inject_io(void)
{
    if (iscase("cid")) M.bad_cid=1;
    if (iscase("sqid")) M.bad_sqid=1;
    if (iscase("timeout") || iscase("cfs")) M.stuck=1;
    if (iscase("cfs")) M.cfs_on_stuck=1;
    if (iscase("status")) M.inject_sc=0x80;
}
static uint64_t access_rpc(uint64_t op,uint64_t addr,uint64_t val)
{
    if (qfd<0 && M.nlog>0 && (iscase("timeout") || iscase("cfs") || iscase("cid") || iscase("sqid") || iscase("phase") || iscase("sqhead"))) {
        check(!(op==5 || (op==3 && addr==0x10000014)), "uncertain DMA is not zeroed/reset/reopened");
    }
    if (qfd>=0) {
        if (op==1) return qt("readl 0x%llx",(unsigned long long)addr);
        if (op==2) return qt("readq 0x%llx",(unsigned long long)addr);
        if (op==3) return qt("writel 0x%llx 0x%llx",(unsigned long long)addr,(unsigned long long)val);
        if (op==4) return qt("writeq 0x%llx 0x%llx",(unsigned long long)addr,(unsigned long long)val);
        if (op==5) return qt("memset 0x%llx %llu 0",(unsigned long long)addr,(unsigned long long)val);
        return 0;
    }
    if (addr>=0x10000000 && addr<0x10010000) {
        uint32_t off=(uint32_t)(addr-0x10000000);
        if (op==1) return m_read32(NULL,off);
        if (op==2) return m_read32(NULL,off)|((uint64_t)m_read32(NULL,off+4)<<32);
        if (op==3 || op==4) {
            if (off==0x1008) inject_io();
            m_write32(NULL,off,(uint32_t)val);
            if (off==0x1008 && (iscase("phase") || iscase("sqhead"))) {
                uint64_t slot=(M.iocq_tail+M.iocq_size-1)%M.iocq_size;
                uint8_t *c=ptr(M.iocq+slot*16,16);
                if (iscase("phase")) p32(c+12,g32(c+12)^(1u<<16));
                else p32(c+8,(g32(c+8)&0xffff0000u)|15);
            }
            if (op==4) m_write32(NULL,off+4,(uint32_t)(val>>32));
            return 0;
        }
    } else if (addr>=PHYS_BASE && addr-PHYS_BASE<DMA_BYTES) {
        uint8_t *p=ptr(addr,op==5?val:op==2||op==4?8:4);
        if (!p) die("model DMA bounds");
        if (op==1) return g32(p);
        if (op==2) return g64(p);
        if (op==3) { p32(p,(uint32_t)val); return 0; }
        if (op==4) { p64(p,val); return 0; }
        if (op==5) { memset(p,0,(size_t)val); return 0; }
    }
    fprintf(stderr,"unsupported RPC %llu %llx\n",(unsigned long long)op,(unsigned long long)addr); exit(2);
}
static void payload(uint64_t addr,uint64_t n)
{
    if (n>8192) die("payload bound");
    uint8_t b[8192];
    if (qfd>=0) {
        for (uint64_t i=0;i<n;i+=4) {
            uint32_t v=(uint32_t)qt("readl 0x%llx",(unsigned long long)(addr+i));
            for (uint64_t j=0;j<4 && i+j<n;j++) b[i+j]=(uint8_t)(v>>(j*8));
        }
    } else {
        uint8_t *p=ptr(addr,n); if (!p) die("payload map"); memcpy(b,p,(size_t)n);
        if (M.nadmin==1) {
            memcpy(b+4,"OMEGA-FIXTURE-000001 ",20);
            memcpy(b+64,"8.2.2   ",8);
        }
    }
    if (exact(infd,b,(size_t)n,1)) die("payload send");
}
int main(int argc,char **argv)
{
    if (argc!=3 && argc!=4) { fprintf(stderr,"usage: runner binary case [qemu]\n"); return 2; }
    scenario=argv[2]; atexit(cleanup); signal(SIGPIPE,SIG_IGN);
    if (posix_memalign((void **)&dma_mem,4096,DMA_BYTES)) die("oracle memory");
    model_reset();
    if (iscase("lba4k")) M.lbads=12;
    model_alloc_media(); fill(M.media,(size_t)M.nsze*bsz(),7);
    check(init_default()==NVME_OK,"C oracle initialization");
    check(nvme_read(&C,8,iscase("lba4k")?2:16,expected)==NVME_OK,"C oracle read");
    model_reset();
    if (iscase("lba4k")) M.lbads=12;
    model_alloc_media(); fill(M.media,(size_t)M.nsze*bsz(),7);
    uint8_t manifest[80]={0};
    memcpy(manifest,"OMEGA-FIXTURE-000001 ",20); memcpy(manifest+20,"8.2.2   ",8);
    p32(manifest+28,1); p64(manifest+32,8); p64(manifest+40,64);
    p64(manifest+48,8); p32(manifest+56,16); p32(manifest+60,40); p32(manifest+64,1);
    p64(manifest+72,argc==4?0x40000000:PHYS_BASE);
    int expected_code=0, expected_copies=40, expected_retained=0;
    wanted=8192;
    if (iscase("lba4k")) p32(manifest+56,2);
    if (iscase("singlepage")) { p32(manifest+56,8); wanted=4096; }
    if (iscase("serial")) { manifest[0]^=1; expected_code=21; }
    if (iscase("firmware")) { manifest[20]^=1; expected_code=22; }
    if (iscase("approval")) { p32(manifest+64,0); expected_code=20; }
    if (iscase("nsid")) { p32(manifest+28,2); expected_code=argc==4?25:24; }
    if (iscase("extent")) { p64(manifest+40,4096); expected_code=27; }
    if (iscase("range")) { p64(manifest+48,72); expected_code=31; }
    if (iscase("before")) { p64(manifest+48,7); expected_code=31; }
    if (iscase("prp")) { p32(manifest+56,17); expected_code=30; }
    if (iscase("zero")) { p32(manifest+56,0); expected_code=30; }
    if (iscase("metadata")) { M.ms=8; expected_code=26; }
    if (iscase("format")) { M.lbads=10; expected_code=26; }
    if (iscase("nsze")) { M.nsze=0; expected_code=25; }
    if (iscase("sqes")) { M.sqes=0x55; expected_code=23; }
    if (iscase("cqes")) { M.cqes=0x33; expected_code=23; }
    if (iscase("cap")) { M.cap&=~(1ull<<37); expected_code=28; }
    if (iscase("dstrd")) M.cap|=1ull<<32;
    if (iscase("stride")) { M.cap|=13ull<<32; expected_code=28; }
    if (iscase("readytimeout")) { M.never_ready=1; expected_code=12; expected_retained=1; }
    if (iscase("readycfs")) { M.cfs_on_enable=1; expected_code=11; expected_retained=1; }
    if (iscase("cid")) { expected_code=14; expected_retained=1; }
    if (iscase("sqid")) { expected_code=15; expected_retained=1; }
    if (iscase("timeout")) { expected_code=12; expected_retained=1; }
    if (iscase("cfs")) { expected_code=11; expected_retained=1; }
    if (iscase("phase")) { expected_code=12; expected_retained=1; }
    if (iscase("sqhead")) { expected_code=16; expected_retained=1; }
    if (iscase("status")) expected_code=17;
    if (!strncmp(scenario,"opcode",6)) {
        expected_code=13;
        p32(manifest+60,1);
        p32(manifest+68,(uint32_t)strtoul(scenario+6,NULL,0));
    }
    if (iscase("adminformat")) {
        expected_code=13; p32(manifest+60,0); p32(manifest+68,0x80);
    }
    if (expected_code) expected_copies=0;
    if (argc==4) {
        start_qemu();
        /* QEMU 8.2.2 reports its model firmware as 8.2.2 (space padded). */
    }
    int input[2], output[2]; if (pipe(input)||pipe(output)) die("pipe");
    child=fork(); if (child<0) die("fork candidate");
    if (!child) {
        dup2(input[0],0); dup2(output[1],1);
        close(input[0]); close(input[1]); close(output[0]); close(output[1]);
        execl(argv[1],argv[1],NULL); _exit(127);
    }
    close(input[0]); close(output[1]); infd=input[1]; outfd=output[0];
    if (exact(infd,manifest,sizeof manifest,1)) die("manifest");
    uint8_t request[24], response[8]; unsigned requests=0;
    while (exact(outfd,request,sizeof request,0)==0) {
        if (++requests>50000) die("RPC budget");
        uint64_t op=g64(request),addr=g64(request+8),val=g64(request+16),r=0;
        if (op>=1 && op<=5) r=access_rpc(op,addr,val);
        else if (op!=6 && op!=7 && op!=8) die("RPC opcode");
        p64(response,r); if (exact(infd,response,sizeof response,1)) die("RPC response");
        if (op==6) payload(addr,val);
        if (op==7) {
            uint8_t data[8192]; check(val==wanted,"copy-out length");
            if (val>sizeof data || exact(outfd,data,(size_t)val,0)) die("copy-out");
            check(!memcmp(data,expected,(size_t)val),"copy-out equals existing C oracle"); published++;
        }
        if (op==8) { result_code=(int)val; retained=(int)addr; reported++; }
    }
    close(infd); close(outfd);
    int status=0; waitpid(child,&status,0); child=-1;
    check(WIFEXITED(status) && WEXITSTATUS(status)==expected_code,"candidate exit code");
    check(reported==1 && result_code==expected_code,"reported result");
    check(retained==expected_retained,"timeout/correlation DMA retention");
    check(published==expected_copies,"copy-out only after success");
    if (qfd<0) {
        check(M.protocol_errors==0,"existing C model protocol checks");
        for (int i=0;i<M.nlog && i<MAXLOG;i++) check(M.log[i].opc==2,"NO WRITE OPCODE REACHABLE");
        if (expected_code && expected_code<30 && expected_code!=14 && expected_code!=15 && expected_code!=16 && expected_code!=12 && expected_code!=11 && expected_code!=17 && expected_code!=13)
            check(M.nlog==0,"identity refusal before IO");
        if (!expected_code) { check(M.nlog==40,"IO queue wraps twice"); check(M.last_cq_db[1]==8,"CQ head wraps"); check(M.iocq_phase==1,"CQ phase wraps twice"); }
        if (iscase("adminwrap")) { check(M.nadmin==44,"admin commands wrap twice"); check(M.acq_phase==1 && M.last_cq_db[0]==12,"admin CQ phase/head wrap"); }
    }
    printf("%s case=%s transport=%s result=%d copies=%d retained=%d RPC=%u physical=NOT_RUN\n",errors?"FAIL":"PASS",scenario,qfd<0?"C-model":"QEMU",result_code,published,retained,requests);
    free(dma_mem); free(M.media); free(M.cache); free(M.dirty);
    return errors?1:0;
}
