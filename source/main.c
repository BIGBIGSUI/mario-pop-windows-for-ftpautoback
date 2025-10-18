// 采用 libtesla 的绘制逻辑：VI 层、帧缓冲、RGBA4444、块线性 swizzle 与像素混合
// 标准头文件
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "util/log.h"

// libnx 头文件
#include <switch.h>
#include <switch/display/framebuffer.h>
#include <switch/display/native_window.h>
#include <switch/services/sm.h>
#include <switch/runtime/devices/fs_dev.h>
// Applet type query for NV auto-selection
#include <switch/services/applet.h>
// 字体与系统语言服务
// 已移除 pl.h 依赖
#include <switch/services/set.h>
#include <switch/runtime/hosversion.h>
// NV 与 NVMAP/FENCE 以便显式初始化与日志
#include <switch/services/nv.h>
#include <switch/nvidia/map.h>
#include <switch/nvidia/fence.h>

/* 字体功能已移除 */

// 覆盖 libnx 的弱符号以强制 NV 服务类型和 tmem 大小（避免卡住）
NvServiceType __attribute__((weak)) __nx_nv_service_type = NvServiceType_Application; // 默认 Auto 在 sysmodule 会选 System；强制走 nvdrv(u)
u32 __attribute__((weak)) __nx_nv_transfermem_size = 0x200000; // 将 tmem 从 8MB 降到 2MB，规避大内存问题

// 内部堆大小（按需调整）
#define INNER_HEAP_SIZE 0x400000

// 屏幕分辨率（与 tesla.hpp 对齐）
#define SCREEN_WIDTH 1920
#define SCREEN_HEIGHT 1080

// 配置项（与 tesla cfg 对齐）
static u16 CFG_FramebufferWidth = 448;
static u16 CFG_FramebufferHeight = 720;
static u16 CFG_LayerWidth = 0;
static u16 CFG_LayerHeight = 0;
static u16 CFG_LayerPosX = 0;
static u16 CFG_LayerPosY = 0;

// Renderer 等价的状态
static ViDisplay g_display;
static ViLayer g_layer;
static Event g_vsyncEvent;
static NWindow g_window;
static Framebuffer g_framebuffer;
static void *g_currentFramebuffer = NULL;
static bool g_gfxInitialized = false;

// 字体状态（与 tesla.hpp 的 Renderer::initFonts 等价的 C 实现）
/* 字体状态已移除 */

// VI 层栈添加（tesla.hpp 使用的辅助函数）
static Result viAddToLayerStack(ViLayer *layer, ViLayerStack stack) {
    const struct {
        u32 stack;
        u64 layerId;
    } in = { stack, layer->layer_id };
    return serviceDispatchIn(viGetSession_IManagerDisplayService(), 6000, in);
}

// libnx 在 vi.c 中提供的弱符号：用于让 viCreateLayer 关联到已创建的 Managed Layer
extern u64 __nx_vi_layer_id;

// 颜色结构（4bit RGBA）
typedef struct { u8 r, g, b, a; } Color;

static inline u16 color_to_u16(Color c) {
    return (u16)((c.r & 0xF) | ((c.g & 0xF) << 4) | ((c.b & 0xF) << 8) | ((c.a & 0xF) << 12));
}

static inline Color color_from_u16(u16 raw) {
    Color c;
    c.r = (raw >> 0) & 0xF;
    c.g = (raw >> 4) & 0xF;
    c.b = (raw >> 8) & 0xF;
    c.a = (raw >> 12) & 0xF;
    return c;
}

// 像素混合（与 tesla.hpp 的 blendColor 一致）
static inline u8 blendColor(u8 src, u8 dst, u8 alpha) {
    u8 oneMinusAlpha = 0x0F - alpha;
    // 使用浮点以匹配 tesla.hpp 行为
    return (u8)((dst * alpha + src * oneMinusAlpha) / (float)0xF);
}

// 将 x,y 映射为块线性帧缓冲中的偏移（与 tesla.hpp getPixelOffset 一致）
static inline u32 getPixelOffset(s32 x, s32 y) {
    // 边界由调用者保证，这里直接映射
    u32 tmpPos = ((y & 127) / 16) + (x / 32 * 8) + ((y / 16 / 8) * (((CFG_FramebufferWidth / 2) / 16 * 8)));
    tmpPos *= 16 * 16 * 4;
    tmpPos += ((y % 16) / 8) * 512 + ((x % 32) / 16) * 256 + ((y % 8) / 2) * 64 + ((x % 16) / 8) * 32 + (y % 2) * 16 + (x % 8) * 2;
    return tmpPos / 2;
}

// 绘制基本原语
static inline void setPixel(s32 x, s32 y, Color color) {
    if (x < 0 || y < 0 || x >= (s32)CFG_FramebufferWidth || y >= (s32)CFG_FramebufferHeight || g_currentFramebuffer == NULL) return;
    u32 offset = getPixelOffset(x, y);
    ((u16*)g_currentFramebuffer)[offset] = color_to_u16(color);
}

static inline void setPixelBlendDst(s32 x, s32 y, Color color) {
    if (x < 0 || y < 0 || x >= (s32)CFG_FramebufferWidth || y >= (s32)CFG_FramebufferHeight || g_currentFramebuffer == NULL) return;
    u32 offset = getPixelOffset(x, y);
    Color src = color_from_u16(((u16*)g_currentFramebuffer)[offset]);
    Color dst = color;
    Color out = {0,0,0,0};
    out.r = blendColor(src.r, dst.r, dst.a);
    out.g = blendColor(src.g, dst.g, dst.a);
    out.b = blendColor(src.b, dst.b, dst.a);
    // alpha 叠加并限制到 0xF
    u16 sumA = (u16)dst.a + (u16)src.a;
    out.a = (sumA > 0xF) ? 0xF : (u8)sumA;
    setPixel(x, y, out);
}

static inline void drawRect(s32 x, s32 y, s32 w, s32 h, Color color) {
    s32 x2 = x + w;
    s32 y2 = y + h;
    if (x2 < 0 || y2 < 0) return;
    if (x >= (s32)CFG_FramebufferWidth || y >= (s32)CFG_FramebufferHeight) return;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x2 > (s32)CFG_FramebufferWidth) x2 = CFG_FramebufferWidth;
    if (y2 > (s32)CFG_FramebufferHeight) y2 = CFG_FramebufferHeight;
    for (s32 xi = x; xi < x2; ++xi) {
        for (s32 yi = y; yi < y2; ++yi) {
            setPixelBlendDst(xi, yi, color);
        }
    }
}

static inline void fillScreenSolid(Color color) {
    if (!g_currentFramebuffer) return;
    for (s32 xi = 0; xi < (s32)CFG_FramebufferWidth; ++xi) {
        for (s32 yi = 0; yi < (s32)CFG_FramebufferHeight; ++yi) {
            setPixel(xi, yi, color);
        }
    }
}

static inline void fillScreen(Color color) {
    drawRect(0, 0, CFG_FramebufferWidth, CFG_FramebufferHeight, color);
}

// 帧控制
static inline void startFrame(void) {
    g_currentFramebuffer = framebufferBegin(&g_framebuffer, NULL);
}

static inline void endFrame(void) {
    eventWait(&g_vsyncEvent, UINT64_MAX);
    framebufferEnd(&g_framebuffer);
    g_currentFramebuffer = NULL;
}

// 图形初始化与释放（移植 tesla Renderer::init/exit 的核心）
static Result gfx_init(void) {
    // 计算 Layer 尺寸，保持纵向填满屏幕并水平居中
    CFG_LayerHeight = SCREEN_HEIGHT;
    CFG_LayerWidth  = (u16)(SCREEN_HEIGHT * ((float)CFG_FramebufferWidth / (float)CFG_FramebufferHeight));
    CFG_LayerPosX = (u16)((SCREEN_WIDTH - CFG_LayerWidth) / 2);
    CFG_LayerPosY = (u16)((SCREEN_HEIGHT - CFG_LayerHeight) / 2); // 等于 0

    log_info("viInitialize(ViServiceType_Manager)...");
    Result rc = viInitialize(ViServiceType_Manager);
    if (R_FAILED(rc)) return rc;

    log_info("viOpenDefaultDisplay...");
    rc = viOpenDefaultDisplay(&g_display);
    if (R_FAILED(rc)) return rc;

    log_info("viGetDisplayVsyncEvent...");
    rc = viGetDisplayVsyncEvent(&g_display, &g_vsyncEvent);
    if (R_FAILED(rc)) return rc;

    // 确保显示全局 Alpha 为不透明
    log_info("viSetDisplayAlpha(1.0f)...");
    viSetDisplayAlpha(&g_display, 1.0f);

    log_info("viCreateManagedLayer...");
    rc = viCreateManagedLayer(&g_display, (ViLayerFlags)0, 0, &__nx_vi_layer_id);
    if (R_FAILED(rc)) return rc;

    log_info("viCreateLayer...");
    rc = viCreateLayer(&g_display, &g_layer);
    if (R_FAILED(rc)) return rc;

    log_info("viSetLayerScalingMode(FitToLayer)...");
    rc = viSetLayerScalingMode(&g_layer, ViScalingMode_FitToLayer);
    if (R_FAILED(rc)) return rc;

    s32 targetZ = 250;
    log_info("viSetLayerZ(%d)...", targetZ);
    rc = viSetLayerZ(&g_layer, targetZ);
        if (R_FAILED(rc)) return rc;

    // 按 tesla.hpp 将 Layer 加入多个层栈（至少 Default）
    log_info("viAddToLayerStack(Default/Lcd/others)...");
    rc = viAddToLayerStack(&g_layer, ViLayerStack_Default);
    if (R_FAILED(rc)) return rc;
    // 可选：截图/录制/任意/最后一帧/空/LCD/调试
    viAddToLayerStack(&g_layer, ViLayerStack_Screenshot);
    viAddToLayerStack(&g_layer, ViLayerStack_Recording);
    viAddToLayerStack(&g_layer, ViLayerStack_Arbitrary);
    viAddToLayerStack(&g_layer, ViLayerStack_LastFrame);
    viAddToLayerStack(&g_layer, ViLayerStack_Null);
    viAddToLayerStack(&g_layer, ViLayerStack_ApplicationForDebug);
    viAddToLayerStack(&g_layer, ViLayerStack_Lcd);

    log_info("viSetLayerSize(%u,%u)...", CFG_LayerWidth, CFG_LayerHeight);
    rc = viSetLayerSize(&g_layer, CFG_LayerWidth, CFG_LayerHeight);
    if (R_FAILED(rc)) return rc;
    log_info("viSetLayerPosition(%u,%u) 屏幕居中", CFG_LayerPosX, CFG_LayerPosY);
    rc = viSetLayerPosition(&g_layer, CFG_LayerPosX, CFG_LayerPosY);
    if (R_FAILED(rc)) return rc;

    log_info("nwindowCreateFromLayer...");
    rc = nwindowCreateFromLayer(&g_window, &g_layer);
    if (R_FAILED(rc)) return rc;

    log_info("framebufferCreate(%u,%u,RGBA_4444,2)...", CFG_FramebufferWidth, CFG_FramebufferHeight);
    rc = framebufferCreate(&g_framebuffer, &g_window, CFG_FramebufferWidth, CFG_FramebufferHeight, PIXEL_FORMAT_RGBA_4444, 2);
    if (R_FAILED(rc)) return rc;

    g_gfxInitialized = true;
    log_info("gfx_init 完成");
    return 0;
}

static __attribute__((unused)) void gfx_exit(void) {
    if (!g_gfxInitialized) return;
    framebufferClose(&g_framebuffer);
    nwindowClose(&g_window);
    // 彻底销毁 Managed Layer，避免残留占用
    log_info("viDestroyManagedLayer...");
    viDestroyManagedLayer(&g_layer);
    viCloseDisplay(&g_display);
    eventClose(&g_vsyncEvent);
    viExit();
    g_gfxInitialized = false;
}



#ifdef __cplusplus
extern "C" {
#endif

// 后台程序：不使用 Applet 环境
u32 __nx_applet_type = AppletType_None;
u32 __nx_fs_num_sessions = 1;

// 配置 newlib 堆（使 malloc/free 可用）
void __libnx_initheap(void)
{
    static u8 inner_heap[INNER_HEAP_SIZE];
    extern void *fake_heap_start;
    extern void *fake_heap_end;
    fake_heap_start = inner_heap;
    fake_heap_end = inner_heap + sizeof(inner_heap);
}

// 必要服务初始化（最小化）
void __appInit(void)
{
    // 参考 libnx 默认初始化，确保时间与 SD 卡挂载可用
    Result rc = smInitialize();
    if (R_FAILED(rc)) return;

    // AppletType_None 时 appletInitialize 返回 0，作为类型选择器
    appletInitialize();
    timeInitialize();
    fsInitialize();
    fsdevMountSdmc();
    // HID 在 None 类型下不是必须，按需初始化
    hidInitialize();

    // 初始化设置服务（用于读取系统语言）
    Result src = setInitialize();
    if (R_FAILED(src)) {
        log_error("setInitialize 失败: 0x%x", src);
    }

    // 字体服务初始化已移除（不再使用 plInitialize）

    // NV 初始化提前到 appinit，避免在 gfx_init 阶段卡住，并输出详细日志
    AppletType appType = appletGetAppletType();
    log_info("appletGetAppletType=%d", (int)appType);
    log_info("nvInitialize... (force type=%d, tmem=0x%x)", __nx_nv_service_type, __nx_nv_transfermem_size);
    Result rc2 = nvInitialize();
    if (R_FAILED(rc2)) {
        log_error("nvInitialize 失败: 0x%x", rc2);
    } else {
        log_info("nvInitialize 成功");
    }
    log_info("nvMapInit...");
    rc2 = nvMapInit();
    if (R_FAILED(rc2)) {
        log_error("nvMapInit 失败: 0x%x", rc2);
    } else {
        log_info("nvMapInit 成功");
    }
    log_info("nvFenceInit...");
    rc2 = nvFenceInit();
    if (R_FAILED(rc2)) {
        log_error("nvFenceInit 失败: 0x%x", rc2);
    } else {
        log_info("nvFenceInit 成功");
    }
}

// 服务释放
void __appExit(void)
{
    hidExit();
    fsExit();
    // 设置服务退出
    setExit();
    // 字体服务退出已移除
    // NV 相关清理
    log_info("nvFenceExit...");
    nvFenceExit();
    log_info("nvMapExit...");
    nvMapExit();
    log_info("nvExit...");
    nvExit();
    smExit();
}

#ifdef __cplusplus
}
#endif

// 主入口：初始化绘制并执行一次演示帧，然后进入后台循环
// 位图字形（16x15）绘制支持：正、在、备、份、上
#define GLYPH_W 16
#define GLYPH_H 15

static void draw_glyph_bitmap(s32 left, s32 top, s32 scale, const unsigned char *bits, int width, int height, Color color) {
    if (!g_currentFramebuffer) return;
    for (int row = 0; row < height; ++row) {
        unsigned char b0 = bits[row*2 + 0];
        unsigned char b1 = bits[row*2 + 1];
        unsigned short rowBits = ((unsigned short)b0 << 8) | (unsigned short)b1; // MSB -> 左侧
        for (int col = 0; col < width; ++col) {
            unsigned short mask = (unsigned short)1 << (15 - col);
            if (rowBits & mask) {
                drawRect(left + col*scale, top + row*scale, scale, scale, color);
            }
        }
    }
}

// UTF-8 简易解码（仅支持到 U+10FFFF）
static const char* utf8_next_simple(const char *s, u32 *out_cp) {
    if (!s || !*s) {
        if (out_cp) { *out_cp = 0; }
        return s;
    }
    unsigned char c = (unsigned char)s[0];
    if (c < 0x80) {
        if (out_cp) { *out_cp = c; }
        return s + 1;
    }
    if ((c >> 5) == 0x6) { // 110xxxxx
        unsigned char c1 = (unsigned char)s[1];
        if ((c1 & 0xC0) != 0x80) {
            if (out_cp) { *out_cp = 0; }
            return s + 1;
        }
        u32 cp = ((u32)(c & 0x1F) << 6) | (u32)(c1 & 0x3F);
        if (out_cp) { *out_cp = cp; }
        return s + 2;
    }
    if ((c >> 4) == 0xE) { // 1110xxxx
        unsigned char c1 = (unsigned char)s[1];
        unsigned char c2 = (unsigned char)s[2];
        if (((c1 & 0xC0) != 0x80) || ((c2 & 0xC0) != 0x80)) {
            if (out_cp) { *out_cp = 0; }
            return s + 1;
        }
        u32 cp = ((u32)(c & 0x0F) << 12) | ((u32)(c1 & 0x3F) << 6) | (u32)(c2 & 0x3F);
        if (out_cp) { *out_cp = cp; }
        return s + 3;
    }
    if ((c >> 3) == 0x1E) { // 11110xxx
        unsigned char c1 = (unsigned char)s[1];
        unsigned char c2 = (unsigned char)s[2];
        unsigned char c3 = (unsigned char)s[3];
        if (((c1 & 0xC0) != 0x80) || ((c2 & 0xC0) != 0x80) || ((c3 & 0xC0) != 0x80)) {
            if (out_cp) { *out_cp = 0; }
            return s + 1;
        }
        u32 cp = ((u32)(c & 0x07) << 18) | ((u32)(c1 & 0x3F) << 12) | ((u32)(c2 & 0x3F) << 6) | (u32)(c3 & 0x3F);
        if (out_cp) { *out_cp = cp; }
        return s + 4;
    }
    if (out_cp) { *out_cp = 0; }
    return s + 1;
}

// 字形位图数据：正
static const unsigned char glyph_zheng_bits[] = {
    0x7F, 0xF8,
    0x01, 0x00,
    0x01, 0x00,
    0x01, 0x00,
    0x01, 0x00,
    0x11, 0x10,
    0x11, 0xF8,
    0x11, 0x00,
    0x11, 0x00,
    0x11, 0x00,
    0x11, 0x00,
    0x11, 0x00,
    0x11, 0x04,
    0xFF, 0xFE,
    0x00, 0x00,
};

// 字形位图数据：在
static const unsigned char glyph_zai_bits[] = {
    0x02, 0x00,
    0x02, 0x04,
    0xFF, 0xFE,
    0x04, 0x00,
    0x04, 0x10,
    0x08, 0x10,
    0x08, 0x14,
    0x13, 0xF8,
    0x30, 0x10,
    0x50, 0x10,
    0x90, 0x10,
    0x10, 0x10,
    0x10, 0x44,
    0x17, 0xFE,
    0x10, 0x00,
};

// 字形位图数据：备
static const unsigned char glyph_bei_bits[] = {
    0x07, 0xF0,
    0x08, 0x20,
    0x14, 0x40,
    0x23, 0x80,
    0x02, 0x80,
    0x0C, 0x60,
    0x30, 0x3E,
    0xDF, 0xF4,
    0x11, 0x10,
    0x11, 0x10,
    0x1F, 0xF0,
    0x11, 0x10,
    0x11, 0x10,
    0x1F, 0xF0,
    0x10, 0x10,
};

// 字形位图数据：份
static const unsigned char glyph_fen_bits[] = {
    0x09, 0x20,
    0x09, 0x20,
    0x11, 0x10,
    0x12, 0x10,
    0x32, 0x0E,
    0x54, 0x04,
    0x9B, 0xF0,
    0x11, 0x10,
    0x11, 0x10,
    0x11, 0x10,
    0x11, 0x10,
    0x12, 0x10,
    0x12, 0x10,
    0x14, 0xA0,
    0x10, 0x20,
};

// 字形位图数据：上
static const unsigned char glyph_shang_bits[] = {
    0x01, 0x00,
    0x01, 0x00,
    0x01, 0x00,
    0x01, 0x10,
    0x01, 0xFC,
    0x01, 0x00,
    0x01, 0x00,
    0x01, 0x00,
    0x01, 0x00,
    0x01, 0x00,
    0x01, 0x00,
    0x01, 0x00,
    0x01, 0x04,
    0xFF, 0xFE,
    0x00, 0x00,
};

// 字形位图数据：传
static const unsigned char glyph_chuan_bits[] = {
    0x08, 0x40,
    0x08, 0x48,
    0x17, 0xFC,
    0x10, 0x40,
    0x30, 0x44,
    0x5F, 0xFE,
    0x90, 0x80,
    0x11, 0x00,
    0x13, 0xFC,
    0x10, 0x08,
    0x11, 0x10,
    0x10, 0xA0,
    0x10, 0x40,
    0x10, 0x60,
    0x10, 0x20,
};

// 字形位图数据：成
static const unsigned char glyph_cheng_bits[] = {
    0x00, 0xA0,
    0x00, 0x90,
    0x3F, 0xFC,
    0x20, 0x80,
    0x20, 0x80,
    0x20, 0x84,
    0x3E, 0x44,
    0x22, 0x48,
    0x22, 0x48,
    0x22, 0x30,
    0x2A, 0x20,
    0x24, 0x62,
    0x40, 0x92,
    0x81, 0x0A,
    0x00, 0x0E,
};

// 字形位图数据：功
static const unsigned char glyph_gong_bits[] = {
    0x00, 0x80,
    0x08, 0x80,
    0xFC, 0x80,
    0x10, 0x84,
    0x17, 0xFE,
    0x10, 0x84,
    0x10, 0x84,
    0x10, 0x84,
    0x10, 0x84,
    0x1D, 0x04,
    0xF1, 0x04,
    0x41, 0x04,
    0x02, 0x44,
    0x04, 0x28,
    0x08, 0x10,
};

// 字形位图数据：失
static const unsigned char glyph_shi_bits[] = {
    0x11, 0x00,
    0x11, 0x00,
    0x11, 0x10,
    0x1F, 0xF8,
    0x21, 0x00,
    0x41, 0x00,
    0x01, 0x04,
    0xFF, 0xFE,
    0x01, 0x00,
    0x02, 0x80,
    0x02, 0x80,
    0x04, 0x40,
    0x08, 0x30,
    0x10, 0x0E,
    0x60, 0x04,
};

// 字形位图数据：败
static const unsigned char glyph_bai_bits[] = {
    0x7E, 0x40,
    0x44, 0x44,
    0x54, 0x7E,
    0x54, 0x88,
    0x55, 0x08,
    0x54, 0x48,
    0x54, 0x48,
    0x54, 0x48,
    0x54, 0x50,
    0x54, 0x50,
    0x10, 0x20,
    0x28, 0x50,
    0x24, 0x8E,
    0x45, 0x04,
    0x82, 0x00,
};

// 将指定 codepoint 映射到已知字形并绘制；返回是否成功
static bool draw_known_glyph(u32 cp, s32 left, s32 top, s32 scale, Color color) {
    switch (cp) {
        case 0x6B63: // 正
            draw_glyph_bitmap(left, top, scale, glyph_zheng_bits, GLYPH_W, GLYPH_H, color); return true;
        case 0x5728: // 在
            draw_glyph_bitmap(left, top, scale, glyph_zai_bits, GLYPH_W, GLYPH_H, color); return true;
        case 0x5907: // 备
            draw_glyph_bitmap(left, top, scale, glyph_bei_bits, GLYPH_W, GLYPH_H, color); return true;
        case 0x4EFD: // 份
            draw_glyph_bitmap(left, top, scale, glyph_fen_bits, GLYPH_W, GLYPH_H, color); return true;
        case 0x4E0A: // 上
            draw_glyph_bitmap(left, top, scale, glyph_shang_bits, GLYPH_W, GLYPH_H, color); return true;
        case 0x4F20: // 传
            draw_glyph_bitmap(left, top, scale, glyph_chuan_bits, GLYPH_W, GLYPH_H, color); return true;
        case 0x6210: // 成
            draw_glyph_bitmap(left, top, scale, glyph_cheng_bits, GLYPH_W, GLYPH_H, color); return true;
        case 0x529F: // 功
            draw_glyph_bitmap(left, top, scale, glyph_gong_bits, GLYPH_W, GLYPH_H, color); return true;
        case 0x5931: // 失
            draw_glyph_bitmap(left, top, scale, glyph_shi_bits, GLYPH_W, GLYPH_H, color); return true;
        case 0x8D25: // 败
            draw_glyph_bitmap(left, top, scale, glyph_bai_bits, GLYPH_W, GLYPH_H, color); return true;
        default:
            return false;
    }
}

// 使用位图字形绘制 UTF-8 字符串（未知字符将跳过）
static void draw_text_bitmap(const char *text, s32 left, s32 top, s32 scale, Color color, s32 letter_spacing) {
    if (!text || !g_currentFramebuffer) return;
    s32 x = left;
    const char *p = text;
    while (*p) {
        u32 cp = 0; p = utf8_next_simple(p, &cp);
        if (cp == 0) break;
        if (draw_known_glyph(cp, x, top, scale, color)) {
            x += (GLYPH_W + letter_spacing) * scale;
        } else {
            // 未知字符：空格宽度处理
            x += (GLYPH_W + letter_spacing) * scale;
        }
    }
}

// 计算位图字符串的像素宽度（按照每字 GLYPH_W 与字间距）
static s32 text_bitmap_width(const char *text, s32 scale, s32 letter_spacing) {
    if (!text) return 0;
    s32 count = 0;
    const char *p = text;
    while (*p) {
        u32 cp = 0;
        const char *np = utf8_next_simple(p, &cp);
        if (np == p) break;
        if (cp == 0) break;
        count++;
        p = np;
    }
    if (count <= 0) return 0;
    return count * GLYPH_W * scale + (count - 1) * letter_spacing * scale;
}

// 粗体黑字 + 白色描边渲染
static void draw_text_bold_outline(const char *text, s32 left, s32 top, s32 scale, s32 letter_spacing) {
    if (!text || !g_currentFramebuffer) return;
    Color outline = (Color){15, 15, 15, 15}; // 白色描边
    Color fill    = (Color){0, 0, 0, 15};    // 黑色填充

    // 白色描边：在周围1像素位置绘制（帧缓冲像素单位）
    static const s32 off[8][2] = {
        {-1, 0}, {1, 0}, {0, -1}, {0, 1},
        {-1, -1}, {-1, 1}, {1, -1}, {1, 1}
    };
    for (int i = 0; i < 8; ++i) {
        draw_text_bitmap(text, left + off[i][0], top + off[i][1], scale, outline, letter_spacing);
    }

    // 黑色填充：两次轻微偏移模拟加粗
    draw_text_bitmap(text, left, top, scale, fill, letter_spacing);
    draw_text_bitmap(text, left + 1, top, scale, fill, letter_spacing);
}

// 马里奥点阵图（基于 mariobros-clock-main 的专业实现）
// RGB565 颜色定义（转换为 RGBA4444）
#define RGB565_TO_R4(c) ((((c) >> 11) & 0x1F) >> 1)
#define RGB565_TO_G4(c) ((((c) >> 5) & 0x3F) >> 2)
#define RGB565_TO_B4(c) (((c) & 0x1F) >> 1)

// 马里奥颜色常量（RGB565格式）
#define M_RED    0xF801  // 红色（帽子、衣服）
#define M_SKIN   0xFD28  // 肤色
#define M_SHOES  0xC300  // 鞋子（深红/棕色）
#define M_SHIRT  0x7BCF  // 衬衫（蓝色）
#define M_HAIR   0x0000  // 头发（黑色）
#define M_MASK   0x000E  // 透明色（天空色，不绘制）

// RGB565 转 Color 结构
static inline Color rgb565_to_color(u16 rgb565) {
    Color c;
    c.r = RGB565_TO_R4(rgb565);
    c.g = RGB565_TO_G4(rgb565);
    c.b = RGB565_TO_B4(rgb565);
    c.a = 15;
    return c;
}

// 马里奥静止状态点阵图（13x16像素）
static const u16 MARIO_IDLE[] = {
    M_MASK, M_MASK, M_MASK, M_RED, M_RED, M_RED, M_RED, M_RED, M_RED, M_MASK, M_MASK, M_MASK, M_MASK, M_MASK, M_MASK, M_RED, 
    M_RED, M_RED, M_RED, M_RED, M_RED, M_RED, M_RED, M_RED, M_RED, M_MASK, M_MASK, M_MASK, M_HAIR, M_HAIR, M_HAIR, M_SKIN, 
    M_SKIN, M_HAIR, M_SKIN, M_SKIN, M_MASK, M_MASK, M_MASK, M_MASK, M_HAIR, M_SKIN, M_HAIR, M_SKIN, M_SKIN, M_SKIN, M_HAIR, M_SKIN, 
    M_SKIN, M_SKIN, M_SKIN, M_MASK, M_MASK, M_HAIR, M_SKIN, M_HAIR, M_HAIR, M_SKIN, M_SKIN, M_SKIN, M_HAIR, M_SKIN, M_SKIN, M_SKIN, 
    M_SKIN, M_MASK, M_HAIR, M_HAIR, M_SKIN, M_SKIN, M_SKIN, M_SKIN, M_HAIR, M_HAIR, M_HAIR, M_HAIR, M_HAIR, M_MASK, M_MASK, M_MASK, 
    M_MASK, M_SKIN, M_SKIN, M_SKIN, M_SKIN, M_SKIN, M_SKIN, M_SKIN, M_SKIN, M_MASK, M_MASK, M_MASK, M_MASK, M_SHIRT, M_SHIRT, M_RED, 
    M_SHIRT, M_SHIRT, M_SHIRT, M_SHIRT, M_MASK, M_MASK, M_MASK, M_MASK, M_MASK, M_SHIRT, M_SHIRT, M_SHIRT, M_RED, M_SHIRT, M_SHIRT, M_RED, 
    M_SHIRT, M_SHIRT, M_SHIRT, M_SHIRT, M_MASK, M_SHIRT, M_SHIRT, M_SHIRT, M_SHIRT, M_RED, M_RED, M_RED, M_RED, M_SHIRT, M_SHIRT, M_SHIRT, 
    M_SHIRT, M_SHIRT, M_SKIN, M_SKIN, M_SHIRT, M_RED, M_SKIN, M_RED, M_RED, M_SKIN, M_RED, M_SHIRT, M_SKIN, M_SKIN, M_SKIN, M_SKIN, 
    M_SKIN, M_SKIN, M_RED, M_RED, M_RED, M_RED, M_RED, M_RED, M_SKIN, M_SKIN, M_SKIN, M_SKIN, M_SKIN, M_SKIN, M_RED, M_RED, 
    M_RED, M_RED, M_RED, M_RED, M_RED, M_RED, M_SKIN, M_SKIN, M_SKIN, M_MASK, M_MASK, M_RED, M_RED, M_RED, M_RED, M_MASK, 
    M_RED, M_RED, M_RED, M_RED, M_MASK, M_MASK, M_MASK, M_SHOES, M_SHOES, M_SHOES, M_SHOES, M_MASK, M_MASK, M_MASK, M_SHOES, M_SHOES, 
    M_SHOES, M_SHOES, M_MASK, M_SHOES, M_SHOES, M_SHOES, M_SHOES, M_SHOES, M_MASK, M_MASK, M_MASK, M_SHOES, M_SHOES, M_SHOES, M_SHOES, M_SHOES
};

#define MARIO_IDLE_W 13
#define MARIO_IDLE_H 16

// 马里奥跳跃状态点阵图（17x16像素）
static const u16 MARIO_JUMP[] = {
    M_MASK, M_MASK, M_MASK, M_MASK, M_MASK, M_MASK, M_MASK, M_MASK, M_MASK, M_MASK, M_MASK, M_MASK, M_MASK, M_SKIN, M_SKIN, M_SKIN, 
    M_SKIN, M_MASK, M_MASK, M_MASK, M_MASK, M_MASK, M_MASK, M_RED, M_RED, M_RED, M_RED, M_RED, M_RED, M_MASK, M_SKIN, M_SKIN, 
    M_SKIN, M_SKIN, M_MASK, M_MASK, M_MASK, M_MASK, M_MASK, M_RED, M_RED, M_RED, M_RED, M_RED, M_RED, M_RED, M_RED, M_RED, 
    M_SKIN, M_SKIN, M_SKIN, M_MASK, M_MASK, M_MASK, M_MASK, M_MASK, M_HAIR, M_HAIR, M_HAIR, M_SKIN, M_SKIN, M_HAIR, M_SKIN, M_SKIN, 
    M_SHIRT, M_SHIRT, M_SHIRT, M_SHIRT, M_MASK, M_MASK, M_MASK, M_MASK, M_HAIR, M_SKIN, M_HAIR, M_SKIN, M_SKIN, M_SKIN, M_HAIR, M_SKIN, 
    M_SKIN, M_SHIRT, M_SHIRT, M_SHIRT, M_SHIRT, M_MASK, M_MASK, M_MASK, M_MASK, M_HAIR, M_SKIN, M_HAIR, M_HAIR, M_SKIN, M_SKIN, M_SKIN, 
    M_HAIR, M_SKIN, M_SKIN, M_SKIN, M_SHIRT, M_SHIRT, M_MASK, M_MASK, M_MASK, M_MASK, M_HAIR, M_HAIR, M_SKIN, M_SKIN, M_SKIN, M_SKIN, 
    M_HAIR, M_HAIR, M_HAIR, M_HAIR, M_SHIRT, M_SHIRT, M_MASK, M_MASK, M_MASK, M_MASK, M_MASK, M_MASK, M_MASK, M_SKIN, M_SKIN, M_SKIN, 
    M_SKIN, M_SKIN, M_SKIN, M_SKIN, M_SHIRT, M_SHIRT, M_MASK, M_MASK, M_MASK, M_MASK, M_SHIRT, M_SHIRT, M_SHIRT, M_SHIRT, M_SHIRT, M_RED, 
    M_SHIRT, M_SHIRT, M_SHIRT, M_RED, M_SHIRT, M_SHIRT, M_MASK, M_MASK, M_MASK, M_MASK, M_SHIRT, M_SHIRT, M_SHIRT, M_SHIRT, M_SHIRT, M_SHIRT, 
    M_SHIRT, M_RED, M_SHIRT, M_SHIRT, M_SHIRT, M_RED, M_RED, M_MASK, M_SHOES, M_SHOES, M_SKIN, M_SKIN, M_SHIRT, M_SHIRT, M_SHIRT, M_SHIRT, 
    M_SHIRT, M_SHIRT, M_RED, M_RED, M_RED, M_RED, M_RED, M_RED, M_MASK, M_SHOES, M_SHOES, M_SKIN, M_SKIN, M_SKIN, M_SKIN, M_RED, 
    M_RED, M_SHIRT, M_RED, M_RED, M_SKIN, M_RED, M_RED, M_SKIN, M_RED, M_SHOES, M_SHOES, M_SHOES, M_MASK, M_SKIN, M_SKIN, M_SHOES, 
    M_RED, M_RED, M_RED, M_RED, M_RED, M_RED, M_RED, M_RED, M_RED, M_RED, M_SHOES, M_SHOES, M_SHOES, M_MASK, M_MASK, M_SHOES, 
    M_SHOES, M_SHOES, M_RED, M_RED, M_RED, M_RED, M_RED, M_RED, M_RED, M_RED, M_RED, M_SHOES, M_SHOES, M_SHOES, M_MASK, M_SHOES, 
    M_SHOES, M_SHOES, M_RED, M_RED, M_RED, M_RED, M_RED, M_RED, M_RED, M_RED, M_MASK, M_MASK, M_MASK, M_MASK, M_MASK, M_MASK, 
    M_SHOES, M_SHOES, M_MASK, M_RED, M_RED, M_RED, M_RED, M_RED, M_MASK, M_MASK, M_MASK, M_MASK, M_MASK, M_MASK, M_MASK, M_MASK
};

#define MARIO_JUMP_W 17
#define MARIO_JUMP_H 16

// 绘制 RGB565 点阵图（支持透明色）
static void draw_rgb565_bitmap(s32 x, s32 y, const u16 *bitmap, s32 width, s32 height, s32 scale) {
    if (!g_currentFramebuffer || !bitmap) return;
    
    for (s32 row = 0; row < height; row++) {
        for (s32 col = 0; col < width; col++) {
            u16 pixel = bitmap[row * width + col];
            // 跳过透明色
            if (pixel == M_MASK) continue;
            
            Color color = rgb565_to_color(pixel);
            // 绘制放大的像素块
            drawRect(x + col * scale, y + row * scale, scale, scale, color);
        }
    }
}

// 绘制马里奥（居中，可选状态）
static void draw_mario_bitmap(s32 cx, s32 cy, s32 scale, bool jumping) {
    if (jumping) {
        s32 left = cx - (MARIO_JUMP_W * scale) / 2;
        s32 top = cy - (MARIO_JUMP_H * scale) / 2;
        draw_rgb565_bitmap(left, top, MARIO_JUMP, MARIO_JUMP_W, MARIO_JUMP_H, scale);
    } else {
        s32 left = cx - (MARIO_IDLE_W * scale) / 2;
        s32 top = cy - (MARIO_IDLE_H * scale) / 2;
        draw_rgb565_bitmap(left, top, MARIO_IDLE, MARIO_IDLE_W, MARIO_IDLE_H, scale);
    }
}

static void draw_cloud(s32 left, s32 top, s32 scale);
static void draw_background_box(s32 left, s32 top, s32 right, s32 bottom);
int main(int argc, char *argv[])
{
    log_info("后台程序启动（移植 tesla 绘制逻辑）");

    Result rc = gfx_init();
    if (R_SUCCEEDED(rc)) {
        // 字体初始化已移除，不再加载共享字体或绘制文本
        log_info("开始首帧绘制：framebufferBegin...");
        // 示例：绘制一次半透明面板与边框
        startFrame();
		// 背景缩小：仅覆盖主体区域（马里奥+文字+云朵）
		{
			const s32 mario_scale = 5;
        s32 cx = (s32)(CFG_FramebufferWidth * 0.5f);
        s32 cy = (s32)(CFG_FramebufferHeight * 0.5f);
			s32 mario_top = cy - (MARIO_IDLE_H*mario_scale)/2;
			s32 mario_bottom = cy + (MARIO_IDLE_H*mario_scale)/2;
			s32 text_top = mario_bottom + 12;
			s32 box_top = mario_top - 80;       // 再增加留白
			s32 box_bottom = text_top + 90;     // 再增加留白
			s32 box_left = cx - 260;
			s32 box_right = cx + 260;
			draw_background_box(box_left, box_top, box_right, box_bottom);
		}
		// 使用优化后的马里奥点阵图（静止状态），放大到 scale=5
		s32 cx = (s32)(CFG_FramebufferWidth * 0.5f);
		s32 cy = (s32)(CFG_FramebufferHeight * 0.5f);
		const s32 mario_scale = 5;
		const s32 brick_scale = 5;
		const s32 jump_delta = 30; // 跳跃高度（像素）
		draw_mario_bitmap(cx, cy, mario_scale, false);
		// 砖块：与马里奥跳跃顶点的头顶相平（砖块底边 = 头顶）
		{
			s32 left_x = cx - (16*brick_scale)/2;
			s32 mario_top_apex = (cy - jump_delta) - (MARIO_JUMP_H * mario_scale) / 2; // 跳跃顶点时头顶y
			s32 brick_top = mario_top_apex - 16*brick_scale; // 让砖块底边与头顶对齐
			draw_cloud(left_x, brick_top, brick_scale);
		}
        // 在马里奥下面显示首帧短语
        {
            const char *texts[] = {"正在备份", "正在上传", "备份成功", "备份失败", "上传成功", "上传失败"};
            size_t idx = 0;
            const char *t = texts[idx];
            s32 text_scale = 4;
            s32 letter_spacing = 1;
			s32 mario_bottom = cy + (MARIO_IDLE_H*5)/2;
            s32 text_top = mario_bottom + 12;
            s32 text_width = text_bitmap_width(t, text_scale, letter_spacing);
            s32 text_left = cx - text_width/2;
			draw_text_bold_outline(t, text_left, text_top, text_scale, letter_spacing);
        }
        log_info("提交首帧：framebufferEnd...");
        endFrame();

        // 保持图层与帧缓冲不释放，持续显示马里奥图案
    } else {
        log_error("图形初始化失败: 0x%x", rc);
    }

	// 动画循环：两帧（静止/跳跃顶点）
    u32 frame_index = 0;
    while (true) {
        startFrame();
		// 背景缩小：仅覆盖主体区域
		{
			const s32 mario_scale = 5;
        s32 cx = (s32)(CFG_FramebufferWidth * 0.5f);
        s32 baseCy = (s32)(CFG_FramebufferHeight * 0.5f);
			s32 mario_top = (baseCy) - (MARIO_IDLE_H*mario_scale)/2 - 30; // 取跳跃顶点稍高
			s32 mario_bottom = (baseCy) + (MARIO_IDLE_H*mario_scale)/2;
			s32 text_top = mario_bottom + 12;
			s32 box_top = mario_top - 80;
			s32 box_bottom = text_top + 90;
			s32 box_left = cx - 260;
			s32 box_right = cx + 260;
			draw_background_box(box_left, box_top, box_right, box_bottom);
		}
		s32 cx = (s32)(CFG_FramebufferWidth * 0.5f);
		s32 baseCy = (s32)(CFG_FramebufferHeight * 0.5f);
		const s32 mario_scale = 5;
		const s32 brick_scale = 5;
		const s32 jump_delta = 30;
		
		bool is_jumping = (frame_index & 1) != 0; // 偶数静止，奇数跳跃顶点
		s32 cy2 = is_jumping ? (baseCy - jump_delta) : baseCy;
		draw_mario_bitmap(cx, cy2, mario_scale, is_jumping);
		
		// 砖块固定位置：与跳跃顶点头顶相平
		{
			s32 left_x = cx - (16*brick_scale)/2;
			s32 mario_top_apex = (baseCy - jump_delta) - (MARIO_JUMP_H * mario_scale) / 2;
			s32 brick_top = mario_top_apex - 16*brick_scale;
			draw_cloud(left_x, brick_top, brick_scale);
		}
        
        // 循环显示文字提示
        {
            const char *texts[] = {"正在备份", "正在上传", "备份成功", "备份失败", "上传成功", "上传失败"};
            const size_t texts_count = sizeof(texts)/sizeof(texts[0]);
            size_t idx = (frame_index / 16) % texts_count; // 每16帧切换一次文字
            const char *t = texts[idx];
            s32 text_scale = 4;
            s32 letter_spacing = 1;
			s32 mario_bottom = baseCy + (MARIO_IDLE_H*5)/2;
            s32 text_top = mario_bottom + 12;
            s32 text_width = text_bitmap_width(t, text_scale, letter_spacing);
            s32 text_left = cx - text_width/2;
			draw_text_bold_outline(t, text_left, text_top, text_scale, letter_spacing);
        }
        endFrame();
        frame_index++;
		svcSleepThread(150000000ULL); // 150ms per frame ≈ 6.7fps（稍慢）
    }

    gfx_exit();
    return 0;
}

// 砖块绘制（16x16像素，带边框和纹理）
static void draw_cloud(s32 left, s32 top, s32 scale) {
    if (!g_currentFramebuffer) return;
    // 16x12 简易云朵
    Color white = (Color){15,15,15,15};
    Color light = (Color){14,14,14,15};
    // 基本轮廓（几段椭圆拼接）
    drawRect(left + 2*scale,  top + 5*scale, 12*scale, 4*scale, white);
    drawRect(left + 4*scale,  top + 3*scale, 8*scale, 6*scale, white);
    drawRect(left + 6*scale,  top + 1*scale, 6*scale, 8*scale, white);
    drawRect(left + 0*scale,  top + 7*scale, 16*scale, 3*scale, white);
    // 底部高光过渡
    drawRect(left + 2*scale,  top + 8*scale, 12*scale, 1*scale, light);
}

static void draw_background_box(s32 left, s32 top, s32 right, s32 bottom) {
    if (!g_currentFramebuffer) return;
    if (left > right) { s32 t = left; left = right; right = t; }
    if (top > bottom) { s32 t = top; top = bottom; bottom = t; }
    // 限制到帧缓冲
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > (s32)CFG_FramebufferWidth) right = CFG_FramebufferWidth;
    if (bottom > (s32)CFG_FramebufferHeight) bottom = CFG_FramebufferHeight;

    Color sky  = (Color){2, 6, 15, 15};   // 天空蓝
    Color land = (Color){1, 8, 1, 15};    // 草地绿
    // 天空区域
    drawRect(left, top, right - left, bottom - top - 20, sky);
    // 细小地面条（背景盒子内部底部20像素）
    drawRect(left, bottom - 20, right - left, 20, land);
}