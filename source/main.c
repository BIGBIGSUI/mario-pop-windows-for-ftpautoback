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

// 实心矩形（不混合，直接覆盖）
static inline void drawRectSolid(s32 x, s32 y, s32 w, s32 h, Color color) {
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
            setPixel(xi, yi, color);
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

// 图形初始化与释放（仿照 pop-windows-main 的防御式策略）
static Result gfx_init(void) {
    // 计算 Layer 尺寸，继续缩小高度以形成更小的"弹窗"效果并水平居中
    CFG_LayerHeight = (u16)(SCREEN_HEIGHT * 0.35f);
    CFG_LayerWidth  = (u16)(SCREEN_HEIGHT * ((float)CFG_FramebufferWidth / (float)CFG_FramebufferHeight));
    CFG_LayerPosX = (u16)((SCREEN_WIDTH - CFG_LayerWidth) / 2);
    CFG_LayerPosY = (u16)((SCREEN_HEIGHT - CFG_LayerHeight) / 2); // 等于 0

    log_info("viInitialize(ViServiceType_Manager)");
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

    s32 layerZ = 250;
        log_info("viSetLayerZ(%d)...", layerZ);
        rc = viSetLayerZ(&g_layer, layerZ);
        if (R_FAILED(rc)) return rc;

    // 添加到图层栈（保守策略：仅 Default 和 Screenshot，避免冲突）
    log_info("viAddToLayerStack(Default and Screenshot)...");
    rc = viAddToLayerStack(&g_layer, ViLayerStack_Default);
    if (R_FAILED(rc)) return rc;
    rc = viAddToLayerStack(&g_layer, ViLayerStack_Screenshot);
    if (R_FAILED(rc)) return rc;

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

static void gfx_exit(void) {
    if (!g_gfxInitialized) return;
    
    log_info("开始清理图形资源...");
    
    // 清理图形相关资源
    framebufferClose(&g_framebuffer);
    nwindowClose(&g_window);
    
    // 安全清理VI资源，避免与其他 overlay 退出冲突（仿照 pop-windows-main）
    log_info("安全清理VI资源...");
    
    Result rc = 0;
    
    // 尝试销毁Managed Layer（容错处理）
    rc = viDestroyManagedLayer(&g_layer);
    if (R_FAILED(rc)) {
        log_info("viDestroyManagedLayer失败 (可能已被其他程序清理): 0x%x", rc);
    }
    
    // 尝试关闭Display（容错处理）
    rc = viCloseDisplay(&g_display);
    if (R_FAILED(rc)) {
        log_info("viCloseDisplay失败 (可能已被其他程序清理): 0x%x", rc);
    }
    
    eventClose(&g_vsyncEvent);
    
    // 最后尝试退出VI服务
    // 如果其他程序（如其他 overlay）已经调用了viExit()，
    // 这里的调用可能会失败，但不会导致程序崩溃
    viExit();
    
    g_gfxInitialized = false;
    
    log_info("图形资源清理完成");
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

// 必要服务初始化（仿照 pop-windows-main 的严格错误处理）
void __appInit(void)
{
    log_info("应用程序初始化开始...");
    
    Result rc = 0;
    
    // 基础服务初始化（严格错误检查）
    rc = smInitialize();
    if (R_FAILED(rc)) {
        log_error("smInitialize失败: 0x%x", rc);
        fatalThrow(rc);
    }
    
    rc = fsInitialize();
    if (R_FAILED(rc)) {
        log_error("fsInitialize失败: 0x%x", rc);
        fatalThrow(rc);
    }
    
    fsdevMountSdmc();
    
    // 其他服务初始化
    rc = hidInitialize();
    if (R_FAILED(rc)) {
        log_error("hidInitialize失败: 0x%x", rc);
        fatalThrow(rc);
    }
    
    rc = setInitialize();
    if (R_FAILED(rc)) {
        log_error("setInitialize失败: 0x%x", rc);
        fatalThrow(rc);
    }
    
    // NV 初始化（不使用 fatalThrow，因为某些环境可能不需要）
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
    
    log_info("应用程序初始化完成");
}

// 服务释放（仿照 pop-windows-main 的清理顺序）
void __appExit(void)
{
    log_info("应用程序退出开始...");
    
    // 优先清理图形资源，避免与其他叠加层冲突
    gfx_exit();
    
    // 清理其他服务
    setExit();
    hidExit();
    
    // NV 相关清理
    log_info("nvFenceExit...");
    nvFenceExit();
    log_info("nvMapExit...");
    nvMapExit();
    log_info("nvExit...");
    nvExit();
    
    // 最后清理基础服务
    fsdevUnmountAll();
    fsExit();
    smExit();
    
    log_info("应用程序退出完成");
}

#ifdef __cplusplus
}
#endif

// 主入口：初始化绘制并执行一次演示帧，然后进入后台循环
// 位图字形（16x15）绘制支持：正、在、备、份、上
#define GLYPH_W 16
#define GLYPH_H 15

// 字形位图绘制（支持独立横纵缩放）
static void draw_glyph_bitmap_scaled(s32 left, s32 top, s32 scale_x, s32 scale_y, const unsigned char *bits, int width, int height, Color color) {
    if (!g_currentFramebuffer) return;
    for (int row = 0; row < height; ++row) {
        unsigned char b0 = bits[row*2 + 0];
        unsigned char b1 = bits[row*2 + 1];
        unsigned short rowBits = ((unsigned short)b0 << 8) | (unsigned short)b1; // MSB -> 左侧
        for (int col = 0; col < width; ++col) {
            unsigned short mask = (unsigned short)1 << (15 - col);
            if (rowBits & mask) {
                drawRect(left + col*scale_x, top + row*scale_y, scale_x, scale_y, color);
            }
        }
    }
}

// 等比例缩放（兼容旧调用）
static void draw_glyph_bitmap(s32 left, s32 top, s32 scale, const unsigned char *bits, int width, int height, Color color) {
    draw_glyph_bitmap_scaled(left, top, scale, scale, bits, width, height, color);
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

// 将指定 codepoint 映射到已知字形并绘制（支持独立横纵缩放）
static bool draw_known_glyph_scaled(u32 cp, s32 left, s32 top, s32 scale_x, s32 scale_y, Color color) {
    switch (cp) {
        case 0x6B63: // 正
            draw_glyph_bitmap_scaled(left, top, scale_x, scale_y, glyph_zheng_bits, GLYPH_W, GLYPH_H, color); return true;
        case 0x5728: // 在
            draw_glyph_bitmap_scaled(left, top, scale_x, scale_y, glyph_zai_bits, GLYPH_W, GLYPH_H, color); return true;
        case 0x5907: // 备
            draw_glyph_bitmap_scaled(left, top, scale_x, scale_y, glyph_bei_bits, GLYPH_W, GLYPH_H, color); return true;
        case 0x4EFD: // 份
            draw_glyph_bitmap_scaled(left, top, scale_x, scale_y, glyph_fen_bits, GLYPH_W, GLYPH_H, color); return true;
        case 0x4E0A: // 上
            draw_glyph_bitmap_scaled(left, top, scale_x, scale_y, glyph_shang_bits, GLYPH_W, GLYPH_H, color); return true;
        case 0x4F20: // 传
            draw_glyph_bitmap_scaled(left, top, scale_x, scale_y, glyph_chuan_bits, GLYPH_W, GLYPH_H, color); return true;
        case 0x6210: // 成
            draw_glyph_bitmap_scaled(left, top, scale_x, scale_y, glyph_cheng_bits, GLYPH_W, GLYPH_H, color); return true;
        case 0x529F: // 功
            draw_glyph_bitmap_scaled(left, top, scale_x, scale_y, glyph_gong_bits, GLYPH_W, GLYPH_H, color); return true;
        case 0x5931: // 失
            draw_glyph_bitmap_scaled(left, top, scale_x, scale_y, glyph_shi_bits, GLYPH_W, GLYPH_H, color); return true;
        case 0x8D25: // 败
            draw_glyph_bitmap_scaled(left, top, scale_x, scale_y, glyph_bai_bits, GLYPH_W, GLYPH_H, color); return true;
        default:
            return false;
    }
}

// 等比例缩放（兼容旧调用）
static bool draw_known_glyph(u32 cp, s32 left, s32 top, s32 scale, Color color) {
    return draw_known_glyph_scaled(cp, left, top, scale, scale, color);
}

// 使用位图字形绘制 UTF-8 字符串（支持独立横纵缩放）
static void draw_text_bitmap_scaled(const char *text, s32 left, s32 top, s32 scale_x, s32 scale_y, Color color, s32 letter_spacing) {
    if (!text || !g_currentFramebuffer) return;
    s32 x = left;
    const char *p = text;
    while (*p) {
        u32 cp = 0; p = utf8_next_simple(p, &cp);
        if (cp == 0) break;
        if (draw_known_glyph_scaled(cp, x, top, scale_x, scale_y, color)) {
            x += (GLYPH_W + letter_spacing) * scale_x;
        } else {
            // 未知字符：空格宽度处理
            x += (GLYPH_W + letter_spacing) * scale_x;
        }
    }
}

// 等比例缩放（兼容旧调用）
static void draw_text_bitmap(const char *text, s32 left, s32 top, s32 scale, Color color, s32 letter_spacing) {
    draw_text_bitmap_scaled(text, left, top, scale, scale, color, letter_spacing);
}

// 计算位图字符串的像素宽度（按照每字 GLYPH_W 与字间距）
static __attribute__((unused)) s32 text_bitmap_width(const char *text, s32 scale, s32 letter_spacing) {
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

// 粗体文字渲染（使用砖块颜色，带白色描边，支持独立横纵缩放）
static void draw_text_bold_outline_scaled(const char *text, s32 left, s32 top, s32 scale_x, s32 scale_y, s32 letter_spacing) {
    if (!text || !g_currentFramebuffer) return;
    Color outline = (Color){15, 15, 15, 15}; // 白色描边
    // 使用砖块颜色（从 SCN_GROUND 的棕色系 0xE2C2，手动转换为 RGBA4444）
    // RGB565: 0xE2C2 = R:28(11100), G:17(010001), B:2(00010)
    // 转 RGBA4444: R≈14, G≈8, B≈1
    Color fill = {14, 8, 1, 15};

    // 白色描边：在周围1像素位置绘制（帧缓冲像素单位）
    static const s32 off[8][2] = {
        {-1, 0}, {1, 0}, {0, -1}, {0, 1},
        {-1, -1}, {-1, 1}, {1, -1}, {1, 1}
    };
    for (int i = 0; i < 8; ++i) {
        draw_text_bitmap_scaled(text, left + off[i][0], top + off[i][1], scale_x, scale_y, outline, letter_spacing);
    }

    // 砖块色填充：多次偏移模拟加粗（4倍加粗效果）
    for (s32 dy = 0; dy < 4; dy++) {
        for (s32 dx = 0; dx < 4; dx++) {
            draw_text_bitmap_scaled(text, left + dx, top + dy, scale_x, scale_y, fill, letter_spacing);
        }
    }
}

// 等比例缩放（兼容旧调用）
static __attribute__((unused)) void draw_text_bold_outline(const char *text, s32 left, s32 top, s32 scale, s32 letter_spacing) {
    draw_text_bold_outline_scaled(text, left, top, scale, scale, letter_spacing);
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
#define M_SHIRT  0xFFFF  // 衬衫（白色）
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

// 绘制 RGB565 点阵图（支持透明色，支持独立横纵缩放）
static void draw_rgb565_bitmap_scaled(s32 x, s32 y, const u16 *bitmap, s32 width, s32 height, s32 scale_x, s32 scale_y) {
    if (!g_currentFramebuffer || !bitmap) return;
    
    for (s32 row = 0; row < height; row++) {
        for (s32 col = 0; col < width; col++) {
            u16 pixel = bitmap[row * width + col];
            // 跳过透明色（与 mariobros-clock-main 的 _MASK 一致）
            if (pixel == M_MASK) continue;
            
            Color color = rgb565_to_color(pixel);
            // 使用实心绘制，避免半透明混合导致的失真/边缘发灰
            drawRectSolid(x + col * scale_x, y + row * scale_y, scale_x, scale_y, color);
        }
    }
}

// 绘制 RGB565 点阵图（等比例缩放，兼容旧调用）
static void draw_rgb565_bitmap(s32 x, s32 y, const u16 *bitmap, s32 width, s32 height, s32 scale) {
    draw_rgb565_bitmap_scaled(x, y, bitmap, width, height, scale, scale);
}

// 复用 mariobros-clock-main 的场景素材（重命名为 SCN_* 以避免命名冲突）
// CLOUD1 从 mariobros-clock-main 精确复制（13列×12行=156，行主序）
static const u16 SCN_CLOUD1[156] = {
    0x000E, 0x0000, 0x0000, 0x0000, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E,
    0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0x0000, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E,
    0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0x000E, 0x0000, 0x000E, 0x000E, 0x000E, 0x000E,
    0xFFFF, 0x3DFF, 0xFFFF, 0xFFFF, 0x3DFF, 0xFFFF, 0xFFFF, 0x0000, 0xFFFF, 0x0000, 0x000E, 0x000E, 0x000E,
    0x3DFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0x000E, 0x000E,
    0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0x000E,
    0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0x000E,
    0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0x000E, 0x000E,
    0xFFFF, 0xFFFF, 0xFFFF, 0x3DFF, 0x3DFF, 0xFFFF, 0x3DFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0x000E,
    0x3DFF, 0x3DFF, 0x3DFF, 0xFFFF, 0xFFFF, 0x3DFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0x000E, 0x000E,
    0xFFFF, 0xFFFF, 0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x000E, 0x000E, 0x000E, 0x000E,
    0x0000, 0x0000, 0x000E, 0x0000, 0x0000, 0x0000, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E
};

// CLOUD2 从 mariobros-clock-main 精确复制（13列×12行=156，行主序）
static const u16 SCN_CLOUD2[156] = {
    0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x0000, 0x0000, 0x0000, 0x000E, 0x000E, 0x000E,
    0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0x0000, 0x000E,
    0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x0000,
    0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x0000, 0xFFFF, 0x3DFF, 0xFFFF, 0xFFFF, 0x3DFF, 0xFFFF, 0xFFFF,
    0x000E, 0x000E, 0x000E, 0x0000, 0x0000, 0xFFFF, 0x3DFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
    0x000E, 0x000E, 0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
    0x000E, 0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
    0x000E, 0xFFFF, 0xFFFF, 0xFFFF, 0x3DFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
    0x000E, 0x000E, 0x0000, 0xFFFF, 0xFFFF, 0x3DFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x3DFF, 0x3DFF, 0xFFFF, 0x3DFF,
    0x000E, 0x000E, 0x000E, 0x0000, 0xFFFF, 0xFFFF, 0x3DFF, 0x3DFF, 0x3DFF, 0xFFFF, 0xFFFF, 0x3DFF, 0xFFFF,
    0x000E, 0x000E, 0x000E, 0x000E, 0x0000, 0x0000, 0xFFFF, 0xFFFF, 0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0x0000,
    0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x0000, 0x0000, 0x000E, 0x0000, 0x0000, 0x0000, 0x000E
};

static const u16 SCN_BUSH[189] = {
    0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x0000,0x0000,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,
    0x000E,0x0000,0x0000,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x0000,0xBFE3,0xBFE3,0x0000,
    0x000E,0x0000,0x000E,0x000E,0x000E,0x0000,0xBFE3,0xBFE3,0x0000,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,
    0x0000,0xBFE3,0xBFE3,0xBFE3,0xBFE3,0x0000,0xBFE3,0x0000,0x000E,0x0000,0xBFE3,0xBFE3,0xBFE3,0xBFE3,0x0000,0x000E,
    0x000E,0x000E,0x000E,0x000E,0x000E,0x0000,0xBFE3,0xBFE3,0xBFE3,0x0560,0xBFE3,0xBFE3,0x0000,0x000E,0x0000,0xBFE3,
    0xBFE3,0xBFE3,0x0560,0xBFE3,0x000E,0x000E,0x000E,0x000E,0x000E,0x0000,0xBFE3,0x0560,0x0560,0xBFE3,0xBFE3,0x0560,
    0xBFE3,0xBFE3,0x0000,0xBFE3,0x0560,0x0560,0xBFE3,0xBFE3,0x0560,0x000E,0x000E,0x000E,0x0000,0x0000,0xBFE3,0x0560,
    0xBFE3,0xBFE3,0xBFE3,0xBFE3,0xBFE3,0xBFE3,0xBFE3,0xBFE3,0x0560,0xBFE3,0xBFE3,0xBFE3,0xBFE3,0xBFE3,0x000E,0x000E,
    0x0000,0xBFE3,0xBFE3,0xBFE3,0xBFE3,0xBFE3,0xBFE3,0xBFE3,0xBFE3,0xBFE3,0xBFE3,0xBFE3,0xBFE3,0xBFE3,0xBFE3,0xBFE3,
    0xBFE3,0xBFE3,0xBFE3,0x000E,0x000E,0x0000,0xBFE3,0xBFE3,0xBFE3,0xBFE3,0xBFE3,0xBFE3,0xBFE3,0xBFE3,0xBFE3,0xBFE3,
    0xBFE3,0xBFE3,0xBFE3,0xBFE3,0xBFE3,0xBFE3,0xBFE3,0xBFE3,0x000E,0x0000,0xBFE3,0xBFE3,0xBFE3,0xBFE3,0xBFE3,0xBFE3,
    0xBFE3,0xBFE3,0xBFE3,0xBFE3,0xBFE3,0xBFE3,0xBFE3
};

static const u16 SCN_GROUND[64] = {
    0xE2C2,0xF6B6,0xF6B6,0xF6B6,0x0000,0xE2C2,0xF6B6,0xE2C2,0xF6B6,0xE2C2,0xE2C2,0xE2C2,0x0000,0xF6B6,0xE2C2,0x0000,
    0xF6B6,0xE2C2,0xE2C2,0xE2C2,0x0000,0xE2C2,0x0000,0xE2C2,0x0000,0xE2C2,0xE2C2,0xE2C2,0x0000,0xF6B6,0xF6B6,0x0000,
    0xF6B6,0x0000,0x0000,0xE2C2,0x0000,0xF6B6,0xE2C2,0x0000,0xF6B6,0xF6B6,0xF6B6,0x0000,0xF6B6,0xE2C2,0xE2C2,0x0000,
    0xF6B6,0xE2C2,0xE2C2,0xF6B6,0xE2C2,0xE2C2,0xE2C2,0x0000,0xE2C2,0x0000,0x0000,0xF6B6,0x0000,0x0000,0x0000,0xE2C2
};

// HILL 从 mariobros-clock-main 精确复制（原始格式：440个元素，按注释每16个分行）
static const u16 SCN_HILL[440] = {
0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E,
0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E,
0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E,
0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E,
0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E,
0x0000, 0x0000, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E,
0x000E, 0x000E, 0x000E, 0x000E, 0x0560, 0x0560, 0x0000, 0x0000, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E,
0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x0560, 0x0560, 0x0560, 0x0560, 0x0000, 0x000E, 0x000E, 0x000E,
0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x0560, 0x0560, 0x0000, 0x0560,
0x0560, 0x0000, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E,
0x0560, 0x0560, 0x0000, 0x0560, 0x0560, 0x0560, 0x0000, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E,
0x000E, 0x000E, 0x000E, 0x000E, 0x0000, 0x0560, 0x0000, 0x0560, 0x0560, 0x0560, 0x0560, 0x0000, 0x000E, 0x000E, 0x000E, 0x000E,
0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x0000, 0x0560, 0x0560, 0x0560, 0x0560, 0x0560, 0x0560, 0x0560,
0x0000, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x0560, 0x0560, 0x0560, 0x0560,
0x0560, 0x0560, 0x0560, 0x0560, 0x0560, 0x0000, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E,
0x0560, 0x0560, 0x0560, 0x0560, 0x0560, 0x0560, 0x0560, 0x0560, 0x0560, 0x0560, 0x0000, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E,
0x000E, 0x000E, 0x000E, 0x000E, 0x0560, 0x0560, 0x0560, 0x0560, 0x0560, 0x0560, 0x0560, 0x0560, 0x0560, 0x0560, 0x0560, 0x0000,
0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x0560, 0x0560, 0x0560, 0x0560, 0x0560, 0x0560, 0x0560, 0x0560,
0x0560, 0x0560, 0x0560, 0x0560, 0x0000, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x0560, 0x0560, 0x0560, 0x0560,
0x0560, 0x0560, 0x0000, 0x0560, 0x0560, 0x0560, 0x0560, 0x0560, 0x0560, 0x0000, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E, 0x000E,
0x0560, 0x0560, 0x0560, 0x0560, 0x0560, 0x0560, 0x0000, 0x0560, 0x0560, 0x0560, 0x0560, 0x0560, 0x0560, 0x0560, 0x0000, 0x000E,
0x000E, 0x000E, 0x000E, 0x000E, 0x0560, 0x0560, 0x0560, 0x0560, 0x0000, 0x0560, 0x0000, 0x0560, 0x0560, 0x0560, 0x0560, 0x0560,
0x0560, 0x0560, 0x0560, 0x0000, 0x000E, 0x000E, 0x000E, 0x000E, 0x0560, 0x0560, 0x0560, 0x0560, 0x0000, 0x0560, 0x0560, 0x0560,
0x0560, 0x0560, 0x0560, 0x0560, 0x0560, 0x0560, 0x0560, 0x0560, 0x0000, 0x000E, 0x000E, 0x000E, 0x0560, 0x0560, 0x0560, 0x0560,
0x0560, 0x0560, 0x0560, 0x0560, 0x0560, 0x0560, 0x0560, 0x0560, 0x0560, 0x0560, 0x0560, 0x0560, 0x0560, 0x0000, 0x000E, 0x000E,
0x0560, 0x0560, 0x0560, 0x0560, 0x0560, 0x0560, 0x0560, 0x0560, 0x0560, 0x0560, 0x0560, 0x0560, 0x0560, 0x0560, 0x0560, 0x0560,
0x0560, 0x0560, 0x0000, 0x000E, 0x0560, 0x0560, 0x0560, 0x0560, 0x0560, 0x0560, 0x0560, 0x0560, 0x0560, 0x0560, 0x0560, 0x0560,
0x0560, 0x0560, 0x0560, 0x0560, 0x0560, 0x0560, 0x0560
};

#define SCN_CLOUD_W 13
#define SCN_CLOUD_H 12
#define SCN_BUSH_W 21
#define SCN_BUSH_H 9
#define SCN_GROUND_W 8
#define SCN_GROUND_H 8
// 修正小山尺寸以匹配 mariobros-clock-main 实际数据（行优先存储）
#define SCN_HILL_W 20
#define SCN_HILL_H 22

static void draw_scene_mariobros(void) {
    // 天空底色改为半透明蓝色
    Color semi_blue = {3, 6, 12, 8}; // 半透明蓝色（alpha=8，约50%透明度）
    fillScreenSolid(semi_blue);

    // 地面平铺（稍微下移）
    s32 tile_scale = 6; // 地面砖块扩大2倍（原3→6）
    s32 ground_offset = 60; // 砖块上移量减少（从80→60，相当于下移20）
    s32 ground_y = (s32)CFG_FramebufferHeight - (SCN_GROUND_H * tile_scale) - ground_offset;
    for (s32 x = 0; x < (s32)CFG_FramebufferWidth; x += SCN_GROUND_W * tile_scale) {
        draw_rgb565_bitmap(x, ground_y, SCN_GROUND, SCN_GROUND_W, SCN_GROUND_H, tile_scale);
    }

    // 小山与灌木（放大并纵向拉伸）
    s32 hill_scale_x = 6; // 小山横向6倍
    s32 hill_scale_y = 8; // 小山纵向8倍（拉伸）
    s32 hill_left = -10; // 小山左移
    s32 hill_top = ground_y - (SCN_HILL_H * hill_scale_y);
    if (hill_top < 0) hill_top = 0; // 防止越界到可视区域之外
    draw_rgb565_bitmap_scaled(hill_left, hill_top, SCN_HILL, SCN_HILL_W, SCN_HILL_H, hill_scale_x, hill_scale_y);

    s32 bush_scale_x = 6; // 灌木横向6倍
    s32 bush_scale_y = 8; // 灌木纵向8倍（拉伸）
    s32 bush_left = (s32)CFG_FramebufferWidth - (SCN_BUSH_W * bush_scale_x) + 10; // 灌木右移
    s32 bush_top = ground_y - (SCN_BUSH_H * bush_scale_y) + 2;
    draw_rgb565_bitmap_scaled(bush_left, bush_top, SCN_BUSH, SCN_BUSH_W, SCN_BUSH_H, bush_scale_x, bush_scale_y);

    // 云朵（放大并下移）
    s32 cloud_scale = 6; // 云朵扩大到6倍（从5→6）
    s32 cloud_down = 70; // 云朵整体下移更多（从60→70）
    s32 cloud_h = SCN_CLOUD_H * cloud_scale;
    s32 cloud_max_top = ground_y - cloud_h - 10; if (cloud_max_top < 0) cloud_max_top = 0;
    s32 c1_top = 30 + cloud_down; if (c1_top > cloud_max_top) c1_top = cloud_max_top;
    s32 c2_top = 50 + cloud_down; if (c2_top > cloud_max_top) c2_top = cloud_max_top;
    s32 c3_top = 40 + cloud_down; if (c3_top > cloud_max_top) c3_top = cloud_max_top;
    draw_rgb565_bitmap(30, c1_top, SCN_CLOUD1, SCN_CLOUD_W, SCN_CLOUD_H, cloud_scale);
    draw_rgb565_bitmap(180, c2_top, SCN_CLOUD2, SCN_CLOUD_W, SCN_CLOUD_H, cloud_scale);
    draw_rgb565_bitmap((s32)CFG_FramebufferWidth - 30 - SCN_CLOUD_W*cloud_scale, c3_top, SCN_CLOUD1, SCN_CLOUD_W, SCN_CLOUD_H, cloud_scale);
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

// 移除未使用的声明以消除编译警告
// static void draw_cloud(s32 left, s32 top, s32 scale);
// static void draw_background_box(s32 left, s32 top, s32 right, s32 bottom);
int main(int argc, char *argv[])
{
    log_info("后台程序启动（移植 tesla 绘制逻辑）");

    Result rc = gfx_init();
    if (R_SUCCEEDED(rc)) {
        // 字体初始化已移除，不再加载共享字体或绘制文本
        log_info("开始首帧绘制：framebufferBegin...");
        // 示例：绘制一次半透明面板与边框
        startFrame();
        // 绘制 mariobros 风格场景
        draw_scene_mariobros();
        // 初始马里奥位置与比例
        s32 mario_scale = 5;
        s32 tile_scale = 3;
        s32 ground_y = (s32)CFG_FramebufferHeight - (SCN_GROUND_H * tile_scale);
        // 从左侧入场
        s32 mario_x0 = 30;
        s32 mario_bottom0 = ground_y;
        s32 mario_top0 = mario_bottom0 - (MARIO_IDLE_H * mario_scale);
        draw_mario_bitmap(mario_x0, mario_top0 + (MARIO_IDLE_H * mario_scale)/2, mario_scale, false);
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
        // 完整场景：mariobros 风格 + 马里奥动作（行走+周期跳跃）
        draw_scene_mariobros();
        static s32 mario_scale = 5;
        static s32 tile_scale = 3;
        static s32 ground_y = 0;
        if (ground_y == 0) ground_y = (s32)CFG_FramebufferHeight - (SCN_GROUND_H * tile_scale) - 60; // 同步砖块上移
        static s32 mario_x = 30;
        static s32 mario_bottom = 0;
        static s32 vy = 0;
        static bool jumping = false;
        if (mario_bottom == 0) mario_bottom = ground_y;
        // 增加弹跳频率：每30帧起跳一次
        if (!jumping && (frame_index % 30 == 0)) {
            jumping = true;
            vy = -20; // 初速度再增
        }
        // 恢复水平平移
        mario_x += 4; // 再快一些的行走速度
        if (mario_x > (s32)CFG_FramebufferWidth + 40) mario_x = -40;
        // 垂直物理（重力）
        if (jumping) {
            mario_bottom += vy;
            vy += 2; // 重力
            if (mario_bottom >= ground_y) {
                mario_bottom = ground_y;
                vy = 0;
                jumping = false;
            }
        }
        // 计算绘制用中心Y（把 bottom 转为中心），整体上移 50 像素
        s32 sprite_h = jumping ? MARIO_JUMP_H : MARIO_IDLE_H;
        s32 cy2 = mario_bottom - (sprite_h * mario_scale)/2 - 50;
        draw_mario_bitmap(mario_x, cy2 + (sprite_h * mario_scale)/2, mario_scale, jumping);
        
        // 在窗口上半部分显示"正在备份"（居中，纵向拉伸，上移）
        {
            const char *text = "正在备份";
            s32 text_scale_x = 5; // 横向5倍
            s32 text_scale_y = 7; // 纵向7倍（拉伸）
            s32 letter_spacing = 1;
            s32 text_height = GLYPH_H * text_scale_y;
            // 上移：减少基准高度，下移1.5倍文字高度
            s32 text_top = (s32)(CFG_FramebufferHeight * 0.15f) + text_height + text_height/2; // 下移1.5倍
            s32 text_width = text_bitmap_width(text, text_scale_x, letter_spacing);
            s32 text_left = ((s32)CFG_FramebufferWidth - text_width) / 2;
            draw_text_bold_outline_scaled(text, text_left, text_top, text_scale_x, text_scale_y, letter_spacing);
        }
        
        endFrame();
        frame_index++;
        svcSleepThread(60000000ULL); // 60ms per frame ≈ 16.7 fps（更快）
    }

    gfx_exit();
    return 0;
}

// 砖块绘制（16x16像素，带边框和纹理）
// 静态工具未被使用：保留实现但加 unused 标注，避免警告
static __attribute__((unused)) void draw_cloud(s32 left, s32 top, s32 scale) {
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

static __attribute__((unused)) void draw_background_box(s32 left, s32 top, s32 right, s32 bottom) {
    if (!g_currentFramebuffer) return;
    if (left > right) { s32 t = left; left = right; right = t; }
    if (top > bottom) { s32 t = top; top = bottom; bottom = t; }
    // 限制到帧缓冲
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > (s32)CFG_FramebufferWidth) right = CFG_FramebufferWidth;
    if (bottom > (s32)CFG_FramebufferHeight) bottom = CFG_FramebufferHeight;

    // 草地配色（包裹内容，无天空）
    Color grass_dark  = (Color){0, 6, 0, 15};   // 深绿草地阴影
    Color grass_base  = (Color){1, 8, 1, 15};   // 基础草地绿
    Color grass_light = (Color){2, 10, 2, 15};  // 亮绿草地高光
    Color dirt        = (Color){5, 3, 1, 15};   // 泥土色
    
    s32 width = right - left;
    s32 height = bottom - top;
    // 草地高度改为盒子高度的 1/3，仅在底部绘制草地条带
    s32 grass_h_total = height / 3;
    if (grass_h_total < 24) grass_h_total = 24; // 保证最小高度，避免细节被裁剪
    s32 grass_top = bottom - grass_h_total;

    // 草地主体（使用实心绘制，避免历史残影）
    drawRectSolid(left, grass_top, width, grass_h_total, grass_base);
    // 草地顶部深色边界线
    drawRectSolid(left, grass_top, width, 2, grass_dark);
    // 侧边微暗（轻微包边，避免突兀）
    drawRectSolid(left, grass_top, 1, grass_h_total, grass_dark);
    drawRectSolid(right - 1, grass_top, 1, grass_h_total, grass_dark);

    // 底部泥土层（草地下方露出泥土，包裹字体）
    s32 dirt_height = grass_h_total / 3;
    if (dirt_height < 8) dirt_height = 8;
    drawRectSolid(left, bottom - dirt_height, width, dirt_height, dirt);
    // 泥土与草地交界处的深色线
    drawRectSolid(left, bottom - dirt_height, width, 1, grass_dark);

    // 添加草丛细节（垂直小条纹模拟草叶）
    for (s32 i = 4; i < width - 4; i += 6) {
        s32 x = left + i;
        s32 base_y = grass_top + 4;
        s32 blade_h = 3 + (i % 3);
        drawRect(x, base_y, 1, blade_h, grass_light);
        drawRect(x + 2, base_y + 1, 1, blade_h - 1, grass_light);
    }

    // 泥土细节：小石子和纹理
    for (s32 i = 5; i < width - 5; i += 12) {
        s32 x = left + i;
        s32 y = bottom - dirt_height + 3;
        drawRect(x, y, 2, 2, grass_dark);
        drawRect(x + 6, y + 4, 1, 1, grass_dark);
    }

    // 草地带中的随机深色斑点（增加真实感）
    for (s32 i = 8; i < width - 8; i += 20) {
        s32 x = left + i;
        s32 y = grass_top + (grass_h_total / 2) + ((i / 4) % 5) - 2;
        drawRect(x, y, 2, 1, grass_dark);
    }
}
