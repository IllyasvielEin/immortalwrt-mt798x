/* Actual vendor firmware parsing/start/readiness helpers with host OS edges. */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
typedef uint8_t u8;typedef uint32_t u32;typedef uint64_t resource_size_t;
#define CONFIG_MTK_AP_BRIDGE 1
#define MAX_REGION_SIZE 3
#define FW_V3_COMMON_TAILER_SIZE 36
#define FW_V3_REGION_TAILER_SIZE 40
#define FW_DL_TIMEOUT 5
#define WO_FW_EMBEDDED 0
#define WO_FW_BIN 1
#define WARP_CHIP_SET "mt7986"
#define WARP_WED1 0
#define WOX_MCU_CFG_LS_WM_BOOT_ADDR_ADDR 0x60
#define WOX_MCU_CFG_LS_WA_BOOT_ADDR_ADDR 0x64
#define WOX_MCU_CFG_LS_WF_MCCR_CLR_ADDR 0x1c
#define WED_DUMMY_CR_FWDL 0
#define time_before(a,b) ((a)<(b))
#define warp_dbg(...) ((void)0)
struct firmware{const u8*data;size_t size;};struct device{};struct platform_device{struct device dev;};
struct mutex{bool locked;};static struct mutex fwdl_lock;
static void mutex_lock(struct mutex*m){assert(!m->locked);m->locked=true;}
static void mutex_unlock(struct mutex*m){assert(m->locked);m->locked=false;}
#include "actual-fwdl-state.inc"
struct warp_entry;struct wed_entry{struct warp_entry*warp;struct platform_device*pdev;int ver,sub_ver;};
struct warp_entry{u8 idx;struct wed_entry wed;struct{struct warp_fwdl_ctrl fwdl_ctrl;}woif;};
static struct platform_device pdev;static struct firmware fw;static u8 image[256];
static u32 boot_regs[64];static unsigned long jiffies;static int request_error,release_count,start_count,start_error,unmaps,polls;
static bool boot_corrupt,ready_timeout;
static int request_firmware_direct(const struct firmware**out,const char*name,struct device*d){assert(!strcmp(name,"7986_WOCPU0_RAM_CODE_release.bin"));if(request_error)return request_error;*out=&fw;return 0;}
static void release_firmware(const struct firmware*f){assert(f==&fw);release_count++;}
static void iounmap(void*p){unmaps++;}
static void memcpy_toio(void*d,const void*s,size_t n){memcpy(d,s,n);}
static void warp_fwdl_write_start_address(struct warp_fwdl_ctrl*c,u32 a,u8 idx){boot_regs[(idx?0x64:0x60)/4]=a>>16;}
#define fwdl_io_read32(c,r,p) (*(p)=boot_regs[(r)/4]^(boot_corrupt?1:0))
#define fwdl_io_write32(c,r,v) (boot_regs[(r)/4]=(v))
static int woif_boot_release(struct warp_entry*w){start_count++;assert(boot_regs[0x60/4]==0x10);assert(boot_regs[0x1c/4]==0xc00);return start_error;}
static void usleep_range(int a,int b){jiffies++;}
static int warp_dummy_cr_get(struct wed_entry*w,u8 idx,u32*out){polls++;*out=ready_timeout?1:0;return 0;}
#include "actual-fwdl.inc"
static u8 memory[3][32];
static void put32(u8*p,u32 v){for(int i=0;i<4;i++)p[i]=v>>(i*8);}
static struct warp_entry setup(void){struct warp_entry w={0};w.wed.warp=NULL;w.wed.pdev=&pdev;w.wed.ver=2;w.wed.sub_ver=1;memset(image,0,sizeof(image));fw=(struct firmware){image,204};image[204-34]=3;for(int i=0;i<3;i++){memset(image+i*16,i+1,16);put32(image+48+i*40+16,0x100000*(i+1));put32(image+48+i*40+20,16);w.woif.fwdl_ctrl.region[i]=(struct fwdl_region){.base_addr=memory[i],.base_addr_pa=0x100000*(i+1),.size=32};}w.woif.fwdl_ctrl.boot_setting.base_addr=boot_regs;release_count=start_count=request_error=start_error=unmaps=polls=0;boot_corrupt=ready_timeout=false;jiffies=0;return w;}
static int run(struct warp_entry*w){w->wed.warp=w;mutex_lock(&fwdl_lock);int ret=warp_fwdl_mcu_mode(&w->woif.fwdl_ctrl,&w->wed,0);mutex_unlock(&fwdl_lock);return ret;}
int main(void){struct warp_entry w=setup();request_error=-ENOENT;assert(run(&w)==-ENOENT&&!start_count&&!release_count);
 w=setup();fw.size=35;assert(run(&w)==-EINVAL&&!start_count&&release_count==1);
 w=setup();image[170]=4;assert(run(&w)==-EINVAL&&!start_count&&release_count==1);
 w=setup();put32(image+48+16,0xf0000999);assert(run(&w)==-EINVAL&&!start_count);
 w=setup();put32(image+48+20,33);assert(run(&w)==-EINVAL&&!start_count);
 w=setup();boot_corrupt=true;assert(run(&w)==-EIO&&!start_count);
 w=setup();start_error=-EIO;assert(run(&w)==-EIO&&start_count==1&&release_count==1);
 w=setup();assert(!run(&w)&&start_count==1&&release_count==1);assert(!w.woif.fwdl_ctrl.bin_ptr&&!w.woif.fwdl_ctrl.fw_entry);for(int i=0;i<3;i++)for(int j=0;j<16;j++)assert(memory[i][j]==i+1);
 ready_timeout=true;assert(warp_fwdl_ready_check_mcu_mode(&w.woif.fwdl_ctrl,&w.wed)==-ETIMEDOUT);assert(unmaps==3&&w.woif.fwdl_ctrl.boot_setting.base_addr==boot_regs);
 w=setup();assert(!warp_fwdl_ready_check_mcu_mode(&w.woif.fwdl_ctrl,&w.wed)&&unmaps==3);
 puts("actual FWDL: missing firmware/short image/invalid region/start/readback/ready-timeout fail without false success; valid image copied before checked boot passed");return 0;}
