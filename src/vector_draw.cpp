#include "vector_draw.h"
#include <esp_heap_caps.h>

// --- 配置 ---
#define MAX_SHAPES 128      // 最大对象数
#define MAX_RAW_LINES 4096  // 最大底层线段数

static uint8_t drawStepSize = 16; // 默认步长

// --- DMA Mode Globals ---
static DrawMode currentDrawMode = DRAW_MODE_CPU;
// DMA Buffer: Stores packed X (high 16) and Y (low 16)
// Double buffering for DMA to avoid tearing
static uint32_t* dmaBuffers[2] = {NULL, NULL};
static uint32_t dmaBufferCounts[2] = {0, 0};
static size_t dmaBufferCapacity = 0;
static volatile uint8_t activeDmaIdx = 0; // ISR reads from this
static uint8_t backDmaIdx = 1;            // updateFrame writes to this
static volatile uint32_t dmaReadIndex = 0;

// --- 数据结构 ---
enum ShapeType {
    SHAPE_NONE = 0,
    SHAPE_LINE,
    SHAPE_RECT,
    SHAPE_STRING,
    SHAPE_CIRCLE
};

struct Shape {
    ShapeType type;
    int16_t x, y;
    int16_t param1, param2; // Line: x1, y1. Rect: w, h. Circle: r, unused.
    // String specific extensions
    uint16_t scale_x, scale_y;
    uint16_t spacing;
    const char* text;       // For SHAPE_STRING
    int32_t scroll;         // For scrolling text
};

// --- 全局变换 ---
static uint16_t global_scale_x_pct = 100;
static uint16_t global_scale_y_pct = 100;
static int16_t global_offset_x = 0;
static int16_t global_offset_y = 0;

// --- 终端状态 ---
static int32_t term_cursor_y = 4096;
static int32_t term_line_height = 400;
static int32_t term_spacing = 100;
static uint16_t term_scale_pct = 10;

// --- 字符数据 ---
typedef struct { float x0,y0,x1,y1; } Line_t;

struct CharPattern {
    const Line_t* lines;
    uint8_t count;
    bool isLarge;
};

static CharPattern _getPattern(char c);
static void _getCharBounds(const CharPattern& cp, float& min_x, float& max_x);

// 为了简洁，包含从 main.c 复制的一组紧凑模式 (A..Z)
// 在真正的库中，我们会更紧凑地存储这些模式或生成它们。
static const Line_t pattern_A[] = { {2, 1, 4 ,7},{4,7,6,1},{3,4,5,4}};
static const Line_t pattern_a[] = { {3, 5, 5, 5},{5, 5, 6 , 4 } ,{ 6 , 4,  6, 1},{ 6, 1, 3 ,1},{3,1,2,2},{2,2,3,3},{3,3,6,3}};
static const Line_t pattern_B[] = { {2,1,2,7},{2,7,4,7},{4,7,5,6},{5,6,5,5},{5,5,4,4},{4,4,2,4},{4,4,5,3},{5,3,5,2},{5,2,4,1},{4,1,2,1}};
static const Line_t pattern_b[] = { {2,7,2,1},{2,1,4,1},{4,1,5,2},{5,2,5,3},{5,3,4,4},{4,4,2,4}};
static const Line_t pattern_C[] = { {6,6,5,7},{5,7,3,7},{3,7,2,6},{2,6,2,2},{2,2,3,1},{3,1,5,1},{5,1,6,2}};
static const Line_t pattern_c[] = { {5,3,4,4},{4,4,3,4},{3,4,2,3},{2,3,2,2},{2,2,3,1},{3,1,4,1},{4,1,5,2}};
static const Line_t pattern_D[] = { {2,1,2,7},{2,7,4,7},{4,7,6,5},{6,5,6,3},{6,3,4,1},{4,1,2,1}};
static const Line_t pattern_d[] = { {5,7,5,1},{5,1,3,1},{3,1,2,2},{2,2,2,3},{2,3,3,4},{3,4,5,4}};
static const Line_t pattern_E[] = { {6,7,2,7},{2,7,2,1},{2,1,6,1},{2,4,5,4}};
static const Line_t pattern_e[] = { {6,1,3,1},{3,1,2,2},{2,2,2,4},{2,4,3,5},{3,5,4,5},{4,5,5,4},{5,4,4,3},{4,3,2,3}};
static const Line_t pattern_F[] = { {2,1,2,7},{2,7,6,7},{2,4,5,4}};
static const Line_t pattern_f[] = { {5,6,4.5,6},{4.5,6,4,5.5},{4,5.5,4,4},{3,4,5,4},{4,4,4,1}};
static const Line_t pattern_G[] = { {6,6,5,7},{5,7,3,7},{3,7,2,6},{2,6,2,2},{2,2,3,1},{3,1,5,1},{5,1,6,2},{6,2,6,4},{6,4,4,4}};
static const Line_t pattern_g[] = { {2,-1,4,-1},{4,-1,5,0},{5,0,5,3},{5,3,4,4},{4,4,3,4},{3,4,2,3},{2,3,2,2},{2,2,3,1},{3,1,4,1},{4,1,5,2}};
static const Line_t pattern_H[] = { {2,1,2,7},{6,1,6,7},{2,4,6,4}};
static const Line_t pattern_h[] = { {3,1,3,7},{3,1,3,3},{3,3,4,4},{4,4,5,4},{5,4,6,3},{6,3,6,1}};
static const Line_t pattern_I[] = { {3,1,5,1},{4,1,4,7},{3,7,5,7}};
static const Line_t pattern_i[] = { {4,6,4,5},{3,4,4,4},{4,4,4,1},{3.5,1,4.5,1}};
static const Line_t pattern_J[] = { {5.5,7,6.5,7},{6,6,6,2},{6,2,5,1},{5,1,4,1}};
static const Line_t pattern_j[] = { {5,7,5,6},{5,3,5,0},{5,0,4,-1}};
static const Line_t pattern_K[] = { {2,7,2,1},{2,4,5,1},{2,4,5,7}};
static const Line_t pattern_k[] = { {3,1,3,6},{3,3,5,4},{3,3,5,1}};
static const Line_t pattern_L[] = { {2,7,2,1},{2,1,6,1}};
static const Line_t pattern_l[] = { {3.5,6.5,4,7},{4,7,4,1},{4,1,4.5,1.5}};
static const Line_t pattern_M[] = { {2,1,2,7},{2,7,4,4},{4,4,6,7},{6,7,6,1}};
static const Line_t pattern_m[] = { {2,1,2,4},{2,4,3,5},{3,5,4,4},{4,4,4,1},{4,1,4,4},{4,4,5,5},{5,5,6,4},{6,4,6,1}};
static const Line_t pattern_N[] = { {2,1,2,7},{2,7,6,1},{6,1,6,7}};
static const Line_t pattern_n[] = { {2,1,2,5},{2,4,3,5},{3,5,4,5},{4,5,5,4},{5,4,5,1}};
static const Line_t pattern_O[] = { {3,1,5,1},{5,1,6,2},{6,2,6,6},{6,6,5,7},{5,7,3,7},{3,7,2,6},{2,6,2,2},{2,2,3,1}};
static const Line_t pattern_o[] = { {3,1,4,1},{4,1,5,2},{5,2,5,4},{5,4,4,5},{4,5,3,5},{3,5,2,4},{2,4,2,2},{2,2,3,1}};
static const Line_t pattern_P[] = { {2,1,2,7},{2,7,5,7},{5,7,6,6},{6,6,6,5},{6,5,5,4},{5,4,2,4}};
static const Line_t pattern_p[] = { {2,-1,2,4},{2,4,4,4},{4,4,5,3},{5,3,4,2},{4,2,2,1}};
static const Line_t pattern_Q[] = { {3,1,5,1},{5,1,6,2},{6,2,6,6},{6,6,5,7},{5,7,3,7},{3,7,2,6},{2,6,2,2},{2,2,3,1},{5,3,7,1}};
static const Line_t pattern_q[] = { {5,-1,5,4},{5,4,3,4},{3,4,2,3},{2,3,2,2},{2,2,3,1},{3,1,5,1}};
static const Line_t pattern_R[] = { {2,1,2,7},{2,7,5,7},{5,7,6,6},{6,6,6,5},{6,5,5,4},{5,4,2,4},{4,4,7,1}};
static const Line_t pattern_r[] = { {2.5,4,3,4},{3,4,3,1},{2.5,1,3.5,1},{3,3,4,4},{4,4,5,4},{5,4,6,3}};
static const Line_t pattern_S[] = { {6,6,5,7},{5,7,3,7},{3,7,2,6},{2,6,2,5},{2,5,3,4},{3,4,5,4},{5,4,6,3},{6,3,6,2},{6,2,5,1},{5,1,3,1},{3,1,2,2}};
static const Line_t pattern_s[] = { {3.75,3.00,3.00,3.75},{3.00,3.75,2.25,3.75},{2.25,3.75,1.50,3.00},{1.50,3.00,2.25,2.25},{2.25,2.25,3.00,2.25},{3.00,2.25,3.75,1.50},{3.75,1.50,3.00,0.75},{3.00,0.75,2.25,0.75},{2.25,0.75,1.50,1.50}};
static const Line_t pattern_T[] = { {2,7,6,7},{4,7,4,1}};
static const Line_t pattern_t[] = { {2,4,4,4},{3,5,3,1.5},{3,1.5,3.5,1},{3.5,1,4,1}};
static const Line_t pattern_U[] = { {2,7,2,2},{2,2,3,1},{3,1,5,1},{5,1,6,2},{6,2,6,7}};
static const Line_t pattern_u[] = { {2,4,2,2},{2,2,3,1},{3,1,4,1},{4,1,5,2},{5,4,5,1}};
static const Line_t pattern_V[] = { {2,7,4,1},{4,1,6,7}};
static const Line_t pattern_v[] = { {2,4,3.5,1},{3.5,1,5,4}};
static const Line_t pattern_W[] = { {2,7,2,1},{2,1,4,4},{4,4,6,1},{6,1,6,7}};
static const Line_t pattern_w[] = { {2,4,2,2},{2,2,3,1},{3,1,4,2},{4,2,4,4},{4,4,4,2},{4,2,5,1},{5,1,6,2},{6,2,6,4}};
static const Line_t pattern_X[] = { {2,7,6,1},{6,7,2,1}};
static const Line_t pattern_x[] = { {2,4,4,1},{4,4,2,1}};
static const Line_t pattern_Y[] = { {2,7,4,4},{6,7,4,4},{4,4,4,1}};
static const Line_t pattern_y[] = { {2,-1,5,4},{3.5,1.5,2,4}};
static const Line_t pattern_Z[] = { {2,7,6,7},{6,7,2,1},{2,1,6,1}};
static const Line_t pattern_z[] = { {2,4,4,4},{4,4,2,1},{2,1,4,1}};



// 数字 0-9
/*
static const Line_t pattern_0[] = { {1200,3295,3000,3295}, {3000,3295,3000,295}, {3000,295,1200,295}, {1200,295,1200,3295} };
static const Line_t pattern_1[] = { {2100,3295,2100,295} };
static const Line_t pattern_2[] = { {1200,3295,3000,3295}, {3000,3295,3000,1795}, {3000,1795,1200,1795}, {1200,1795,1200,295}, {1200,295,3000,295} };
static const Line_t pattern_3[] = { {1200,3295,3000,3295}, {3000,3295,3000,295}, {3000,295,1200,295}, {1200,1795,3000,1795} };
static const Line_t pattern_4[] = { {1200,3295,1200,1795}, {1200,1795,3000,1795}, {3000,3295,3000,295} };
static const Line_t pattern_5[] = { {3000,3295,1200,3295}, {1200,3295,1200,1795}, {1200,1795,3000,1795}, {3000,1795,3000,295}, {3000,295,1200,295} };
static const Line_t pattern_6[] = { {3000,3295,1200,3295}, {1200,3295,1200,295}, {1200,295,3000,295}, {3000,295,3000,1795}, {3000,1795,1200,1795} };
static const Line_t pattern_7[] = { {1200,3295,3000,3295}, {3000,3295,3000,295} };
static const Line_t pattern_8[] = { {1200,3295,3000,3295}, {3000,3295,3000,295}, {3000,295,1200,295}, {1200,295,1200,3295}, {1200,1795,3000,1795} };
static const Line_t pattern_9[] = { {3000,295,3000,3295}, {3000,3295,1200,3295}, {1200,3295,1200,1795}, {1200,1795,3000,1795} };
*/

static const Line_t pattern_0[] = { {3,6.5,3,1.5},{3,1.5,3.5,1},{3.5,1,5.5,1},{5.5,1,6,1.5},{6,1.5,6,6.5},{6,6.5,5.5,7},{5.5,7,3.5,7},{3.5,7,3,6.5} };
static const Line_t pattern_1[] = { {3,6,4,7},{4,7,4,1},{3,1,5,1} };
static const Line_t pattern_2[] = { {2,6,3,7},{3,7,5,7},{5,7,6,6},{6,6,2,1},{2,1,6,1} };
static const Line_t pattern_3[] = { {3,7,5,7},{5,7,6,5.5},{6,5.5,5,4},{5,4,3,4},{5,4,6,2.5},{6,2.5,5,1},{5,1,3,1} };
static const Line_t pattern_4[] = { {3,7,2,3},{2,3,6,3},{6,3,4,3},{4,1,4,7} };
static const Line_t pattern_5[] = { {6,7,3,7},{3,7,3,4.5},{3,4.5,3.5,4},{3.5,4,5.5,4},{5.5,4,6,3.5},{6,3.5,6,1.5},{6,1.5,5.5,1},{5.5,1,3,1} };
static const Line_t pattern_6[] = { {6,6.5,5.5,7},{5.5,7,3.5,7},{3.5,7,3,6.5},{3,6.5,3,1.5},{3,1.5,3.5,1},{3.5,1,5.5,1},{5.5,1,6,1.5},{6,1.5,6,3.5},{6,3.5,5.5,4},{5.5,4,3,4} };
static const Line_t pattern_7[] = { {3,7,6,7},{6,7,5,1} };
static const Line_t pattern_8[] = { {5.5,4,3.5,4},{3.5,4,3,3.5},{3,3.5,3.5,4},{3.5,4,3,4.5},{3,4.5,3,6.5},{3,6.5,3.5,7},{3.5,7,5.5,7},{5.5,7,6,6.5},{6,6.5,6,4.5},{6,4.5,5.5,4},{5.5,4,6,3.5},{6,3.5,6,1.5},{6,1.5,5.5,1},{5.5,1,3.5,1},{3.5,1,3,1.5},{3,1.5,3,3.5},{3,3.5,3.5,4} };
static const Line_t pattern_9[] = { {3.5,7,3,6.5},{3,6.5,3,4.5},{3,4.5,3.5,4},{3.5,4,5.5,4},{5.5,4,6,5.5},{6,1.5,6,5.5},{5.5,4,3.5,4},{3,4.5,3,6.5},{3,6.5,3.5,7},{3.5,7,5.5,7},{5.5,7,6,6.5},{6,6.5,6,4.5},{6,4.5,5.5,4},{5.5,4,3.5,4} };
// 符号
static const Line_t pattern_excl[] = { {2100,3295,2100,1295}, {2100,795,2100,295} }; // !
static const Line_t pattern_apos[] = { {2100,3295,2100,2295} }; // '
static const Line_t pattern_hash[] = { {1600,3295,1600,295}, {2600,3295,2600,295}, {1200,2295,3000,2295}, {1200,1295,3000,1295} }; // #
static const Line_t pattern_pct[] = { {1200,295,3000,3295}, {1400,3095,1600,3095}, {1600,3095,1600,2895}, {1600,2895,1400,2895}, {1400,2895,1400,3095}, {2600,695,2800,695}, {2800,695,2800,495}, {2800,495,2600,495}, {2600,495,2600,695} }; // % (简化圆)
static const Line_t pattern_caret[] = { {1200,1795,2100,3295}, {2100,3295,3000,1795} }; // ^
static const Line_t pattern_ast[] = { {1200,2795,3000,795}, {3000,2795,1200,795}, {2100,3295,2100,295}, {1200,1795,3000,1795} }; // *
static const Line_t pattern_under[] = { {1200,295,3000,295} }; // _
static const Line_t pattern_minus[] = { {1200,1795,3000,1795} }; // -
static const Line_t pattern_plus[] = { {2100,3295,2100,295}, {1200,1795,3000,1795} }; // +
static const Line_t pattern_eq[] = { {1200,2295,3000,2295}, {1200,1295,3000,1295} }; // =
static const Line_t pattern_bslash[] = { {1200,3295,3000,295} }; // \ (反斜杠)
static const Line_t pattern_fslash[] = { {1200,295,3000,3295} }; // /
static const Line_t pattern_lparen[] = { {2600,3295,1600,1795}, {1600,1795,2600,295} }; // (
static const Line_t pattern_rparen[] = { {1600,3295,2600,1795}, {2600,1795,1600,295} }; // )
static const Line_t pattern_lbrack[] = { {2600,3295,1600,3295}, {1600,3295,1600,295}, {1600,295,2600,295} }; // [
static const Line_t pattern_rbrack[] = { {1600,3295,2600,3295}, {2600,3295,2600,295}, {2600,295,1600,295} }; // ]
static const Line_t pattern_lbrace[] = { {2600,3295,2100,3295}, {2100,3295,2100,1795}, {2100,1795,1600,1795}, {2100,1795,2100,295}, {2100,295,2600,295} }; // {
static const Line_t pattern_rbrace[] = { {1600,3295,2100,3295}, {2100,3295,2100,1795}, {2100,1795,2600,1795}, {2100,1795,2100,295}, {2100,295,1600,295} }; // }
static const Line_t pattern_quote[] = { {1600,3295,1600,2295}, {2600,3295,2600,2295} }; // "
static const Line_t pattern_semi[] = { {2100,3295,2100,2795}, {2100,1295,1700,295} }; // ; (点 + 线)
static const Line_t pattern_colon[] = { {2100,3295,2100,2295}, {2100,1295,2100,295} }; // :
static const Line_t pattern_comma[] = { {2100,495,2100,295}, {2100,295,1700,95} }; // ,
static const Line_t pattern_period[] = { {1900,495,2300,495}, {2300,495,2300,295}, {2300,295,1900,295}, {1900,295,1900,495} }; // .
static const Line_t pattern_question[] = { {1200,2595,1200,2995}, {1200,2995,1800,3295}, {1800,3295,2400,3295}, {2400,3295,3000,2995}, {3000,2995,3000,2295}, {3000,2295,2100,1595}, {2100,1595,2100,1095}, {2100,595,2100,295} }; // ?
static const Line_t pattern_at[] = { {2600,1295,2200,1295}, {2200,1295,1800,1695}, {1800,1695,1800,2095}, {1800,2095,2200,2495}, {2200,2495,2600,2095}, {2600,2095,2600,1695}, {2600,1695,3000,1295}, {3000,1295,3000,2895}, {3000,2895,1400,2895}, {1400,2895,1400,895}, {1400,895,3000,895} }; // @
static const Line_t pattern_dollar[] = { {2600,3295,1600,3295}, {1600,3295,1600,2095}, {1600,2095,2600,2095}, {2600,2095,2600,895}, {2600,895,1600,895}, {2100,3695,2100,495} }; // $
static const Line_t pattern_lt[] = { {2600,3295,1200,1795}, {1200,1795,2600,295} }; // <
static const Line_t pattern_gt[] = { {1200,3295,2600,1795}, {2600,1795,1200,295} }; // >
static const Line_t pattern_pipe[] = { {2100,3295,2100,295} }; // |
static const Line_t pattern_tilde[] = { {1200,1295,1600,2295}, {1600,2295,2200,1295}, {2200,1295,2600,2295} }; // ~
// --- 对象池 ---
Shape shapePool[MAX_SHAPES];
uint16_t shapeCount = 0;

// --- 双缓冲底层线段 ---
// rawLines[bufferIndex][lineIndex][0=x0, 1=y0, 2=x1, 3=y1]
uint16_t rawLines[2][MAX_RAW_LINES][4];
volatile uint16_t rawLineCounts[2] = {0, 0};
volatile uint8_t activeBufferIdx = 0; // 当前 ISR 使用的缓冲区
volatile uint8_t backBufferIdx = 1;   // 当前 updateFrame 写入的缓冲区

// --- 运行时状态 ---
volatile uint16_t currentLineIndex = 0;

// 定点数插值变量 (16.16格式)
int32_t curr_x_fp, curr_y_fp; 
int32_t inc_x_fp, inc_y_fp;   
uint16_t steps_remaining;     

// 辅助函数：计算绝对值
inline int16_t i_abs(int16_t v) { return v < 0 ? -v : v; }

// --- API 实现 ---

void DRAW_Clear() {
    shapeCount = 0;
}

void DRAW_SetMode(DrawMode mode) {
    currentDrawMode = mode;
    if (mode == DRAW_MODE_DMA) {
        if (dmaBuffers[0] == NULL) {
            // Allocate 1MB for each buffer (250k points)
            // 250k points * 4 bytes = 1MB.
            size_t size = 1024 * 1024; 
            dmaBuffers[0] = (uint32_t*)heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
            dmaBuffers[1] = (uint32_t*)heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
            
            if (dmaBuffers[0] && dmaBuffers[1]) {
                dmaBufferCapacity = size / 4;
            } else {
                // Allocation failed
                if (dmaBuffers[0]) free(dmaBuffers[0]);
                if (dmaBuffers[1]) free(dmaBuffers[1]);
                dmaBuffers[0] = NULL;
                dmaBuffers[1] = NULL;
                currentDrawMode = DRAW_MODE_CPU;
            }
        }
    }
}

void DRAW_SetStepSize(uint8_t step) {
    if (step == 0) step = 1;
    drawStepSize = step;
}

void DRAW_AddLine(int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    if (shapeCount >= MAX_SHAPES) return;
    shapePool[shapeCount].type = SHAPE_LINE;
    shapePool[shapeCount].x = (int16_t)x0;
    shapePool[shapeCount].y = (int16_t)y0;
    shapePool[shapeCount].param1 = (int16_t)x1;
    shapePool[shapeCount].param2 = (int16_t)y1;
    shapeCount++;
}

void DRAW_AddRect(int32_t x, int32_t y, int32_t w, int32_t h) {
    if (shapeCount >= MAX_SHAPES) return;
    shapePool[shapeCount].type = SHAPE_RECT;
    shapePool[shapeCount].x = (int16_t)x;
    shapePool[shapeCount].y = (int16_t)y;
    shapePool[shapeCount].param1 = (int16_t)w;
    shapePool[shapeCount].param2 = (int16_t)h;
    shapeCount++;
}

void DRAW_AddCircle(int32_t x, int32_t y, int32_t r) {
    if (shapeCount >= MAX_SHAPES) return;
    shapePool[shapeCount].type = SHAPE_CIRCLE;
    shapePool[shapeCount].x = (int16_t)x;
    shapePool[shapeCount].y = (int16_t)y;
    shapePool[shapeCount].param1 = (int16_t)r;
    shapeCount++;
}

// --- 新增 API 实现 ---

void DRAW_SetLetter(char c) {
    DRAW_Clear();
    // Create a static string buffer for single char
    static char buf[2] = {0, 0};
    buf[0] = c;
    // Center it roughly
    DRAW_AddString(buf, 0, 2048, 2048, 100, 100);
}

void DRAW_SetScale(uint16_t scale_x_percent, uint16_t scale_y_percent) {
    global_scale_x_pct = scale_x_percent;
    global_scale_y_pct = scale_y_percent;
}

void DRAW_SetOffset(int16_t offset_x, int16_t offset_y) {
    global_offset_x = offset_x;
    global_offset_y = offset_y;
}

int16_t DRAW_AddString(const char *s, uint16_t spacing, int32_t x, int32_t y, uint16_t scale_x, uint16_t scale_y) {
    if (shapeCount >= MAX_SHAPES) return -1;
    int16_t slot = shapeCount;
    shapePool[slot].type = SHAPE_STRING;
    shapePool[slot].x = (int16_t)x;
    shapePool[slot].y = (int16_t)y;
    shapePool[slot].scale_x = scale_x;
    shapePool[slot].scale_y = scale_y;
    shapePool[slot].spacing = spacing;
    shapePool[slot].scroll = 0;
    shapePool[slot].text = s;
    
    // Calculate total width and store in param1
    int32_t width = 0;
    const char* p = s;
    while (*p) {
        if (*p == ' ') {
            width += 4 * scale_x;
        } else {
            CharPattern cp = _getPattern(*p);
            if (cp.lines) {
                float min_x, max_x;
                _getCharBounds(cp, min_x, max_x);
                float char_w = max_x - min_x;
                if (char_w < 1.0f) char_w = 4.0f;
                
                if (spacing > 0) {
                    width += spacing;
                } else {
                    width += (int32_t)((char_w + 1.5f) * scale_x);
                }
            }
        }
        p++;
    }
    shapePool[slot].param1 = (int16_t)width; // Store width
    
    shapeCount++;
    return slot;
}

int32_t DRAW_GetTextScroll(int16_t slot) {
    if (slot >= 0 && slot < shapeCount && shapePool[slot].type == SHAPE_STRING) {
        return shapePool[slot].scroll;
    }
    return 0;
}

void DRAW_SetTextScroll(int16_t slot, int32_t scroll) {
    if (slot >= 0 && slot < shapeCount && shapePool[slot].type == SHAPE_STRING) {
        shapePool[slot].scroll = scroll;
    }
}

void DRAW_Update(void) {
    // Handle scrolling
    for (uint16_t i = 0; i < shapeCount; i++) {
        Shape *s = &shapePool[i];
        if (s->type == SHAPE_STRING) {
            int32_t width = s->param1; // Retrieved cached width
            // Check if text extends beyond right edge (2047)
            // If x + width > 2047, enable scrolling
            if (s->x + width > 2047) {
                s->scroll += 10; // Scroll speed
                
                // If scrolled completely off-screen to the left
                // render_x = x - scroll
                // if render_x + width < 0 => x - scroll + width < 0
                if (s->x - s->scroll + width < 0) {
                    // Reset to appear from right edge
                    // render_x = 2047 => x - scroll = 2047 => scroll = x - 2047
                    s->scroll = s->x - 2047;
                }
            }
        }
    }
}

void DRAW_Terminal_Init(uint16_t scale_pct, int32_t spacing) {
    DRAW_Clear();
    term_scale_pct = scale_pct;
    term_spacing = spacing;
    term_cursor_y = 4096 - 200; // Start from top
}

void DRAW_Terminal_SetSpacing(int32_t spacing) {
    term_spacing = spacing;
}

void DRAW_Terminal_Print(const char *str) {
    // Simple terminal: add string at current cursor Y and move down
    // Note: str must be persistent! If it's a temporary buffer, this will fail.
    // Assuming str is a string literal or managed externally for now.
    
    DRAW_AddString(str, 0, 100, term_cursor_y, term_scale_pct, term_scale_pct);
    term_cursor_y -= term_line_height;
    
    // Wrap around or clear? For now, just let it go off screen or reset
    if (term_cursor_y < 0) {
        DRAW_Clear();
        term_cursor_y = 4096 - 200;
    }
}

static CharPattern _getPattern(char c) {
    CharPattern cp = {NULL, 0, false};
    
    // Macros for brevity
    #define P(x) pattern_##x
    #define S(x) (sizeof(P(x))/sizeof(Line_t))
    #define C(char_val, pat_suffix) case char_val: cp.lines = P(pat_suffix); cp.count = S(pat_suffix); break;
    #define CL(char_val, pat_suffix) case char_val: cp.lines = P(pat_suffix); cp.count = S(pat_suffix); cp.isLarge = true; break;

    switch (c) {
        C('A', A) C('B', B) C('C', C) C('D', D) C('E', E) C('F', F) C('G', G)
        C('H', H) C('I', I) C('J', J) C('K', K) C('L', L) C('M', M) C('N', N)
        C('O', O) C('P', P) C('Q', Q) C('R', R) C('S', S) C('T', T) C('U', U)
        C('V', V) C('W', W) C('X', X) C('Y', Y) C('Z', Z)
        
        C('a', a) C('b', b) C('c', c) C('d', d) C('e', e) C('f', f) C('g', g)
        C('h', h) C('i', i) C('j', j) C('k', k) C('l', l) C('m', m) C('n', n)
        C('o', o) C('p', p) C('q', q) C('r', r) C('s', s) C('t', t) C('u', u)
        C('v', v) C('w', w) C('x', x) C('y', y) C('z', z)

        C('0', 0) C('1', 1) C('2', 2) C('3', 3) C('4', 4)
        C('5', 5) C('6', 6) C('7', 7) C('8', 8) C('9', 9)

        CL('!', excl) CL('\'', apos) CL('#', hash) CL('%', pct) CL('^', caret)
        CL('*', ast) CL('_', under) CL('-', minus) CL('+', plus) CL('=', eq)
        CL('\\', bslash) CL('/', fslash) CL('(', lparen) CL(')', rparen)
        CL('[', lbrack) CL(']', rbrack) CL('{', lbrace) CL('}', rbrace)
        CL('"', quote) CL(';', semi) CL(':', colon) CL(',', comma)
        CL('.', period) CL('?', question) CL('@', at) CL('$', dollar)
        CL('<', lt) CL('>', gt) CL('|', pipe) CL('~', tilde)
    }
    return cp;
}

// 内部辅助：向后台缓冲区添加一条线
void _addRawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1) {
    uint16_t idx = rawLineCounts[backBufferIdx];
    if (idx >= MAX_RAW_LINES) return;

    rawLines[backBufferIdx][idx][0] = x0;
    rawLines[backBufferIdx][idx][1] = y0;
    rawLines[backBufferIdx][idx][2] = x1;
    rawLines[backBufferIdx][idx][3] = y1;
    rawLineCounts[backBufferIdx]++;
}

void _addDmaPoint(int16_t x, int16_t y) {
    if (dmaBuffers[backDmaIdx] && dmaBufferCounts[backDmaIdx] < dmaBufferCapacity) {
        dmaBuffers[backDmaIdx][dmaBufferCounts[backDmaIdx]++] = ((uint32_t)x << 16) | (uint16_t)y;
    }
}

void _rasterizeLineToDMA(int16_t x0, int16_t y0, int16_t x1, int16_t y1) {
    int16_t dx = x1 - x0;
    int16_t dy = y1 - y0;
    int16_t dist = (i_abs(dx) > i_abs(dy)) ? i_abs(dx) : i_abs(dy);
    
    uint16_t steps = dist / drawStepSize;
    if (steps == 0) steps = 1;

    for (uint16_t i = 0; i < steps; i++) {
        int32_t x = x0 + ((int32_t)dx * i / steps);
        int32_t y = y0 + ((int32_t)dy * i / steps);
        _addDmaPoint((int16_t)x, (int16_t)y);
    }
}

void _processLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1) {
    // Apply global transform
    int32_t tx0 = (int32_t)x0 * global_scale_x_pct / 100 + global_offset_x;
    int32_t ty0 = (int32_t)y0 * global_scale_y_pct / 100 + global_offset_y;
    int32_t tx1 = (int32_t)x1 * global_scale_x_pct / 100 + global_offset_x;
    int32_t ty1 = (int32_t)y1 * global_scale_y_pct / 100 + global_offset_y;

    // Clip to 12-bit DAC range (0-4095) roughly, or let it wrap?
    // For now, just cast back to int16_t.
    
    // Boundary Check: Ignore lines that are completely out of bounds (0-2047)
    if (tx0 < 0 || tx0 > 2047 || ty0 < 0 || ty0 > 2047 || 
        tx1 < 0 || tx1 > 2047 || ty1 < 0 || ty1 > 2047) {
        return;
    }

    if (currentDrawMode == DRAW_MODE_CPU) {
        _addRawLine((int16_t)tx0, (int16_t)ty0, (int16_t)tx1, (int16_t)ty1);
    } else {
        _rasterizeLineToDMA((int16_t)tx0, (int16_t)ty0, (int16_t)tx1, (int16_t)ty1);
    }
}

// 计算字符的边界 (用于自动字间距)
static void _getCharBounds(const CharPattern& cp, float& min_x, float& max_x) {
    min_x = 10000.0f;
    max_x = -10000.0f;
    
    for (uint8_t k = 0; k < cp.count; k++) {
        float lx0 = cp.lines[k].x0;
        float lx1 = cp.lines[k].x1;
        
        if (cp.isLarge) {
            lx0 /= 400.0f;
            lx1 /= 400.0f;
        }
        
        if (lx0 < min_x) min_x = lx0;
        if (lx0 > max_x) max_x = lx0;
        if (lx1 < min_x) min_x = lx1;
        if (lx1 > max_x) max_x = lx1;
    }
}

void DRAW_Render() {
    // 1. 重置后台缓冲区计数
    if (currentDrawMode == DRAW_MODE_CPU) {
        rawLineCounts[backBufferIdx] = 0;
    } else {
        dmaBufferCounts[backDmaIdx] = 0;
    }

    // 2. 遍历对象池，生成线段
    for (uint16_t i = 0; i < shapeCount; i++) {
        Shape *s = &shapePool[i];
        if (s->type == SHAPE_LINE) {
            _processLine(s->x, s->y, s->param1, s->param2);
        } else if (s->type == SHAPE_RECT) {
            int16_t x = s->x;
            int16_t y = s->y;
            int16_t w = s->param1;
            int16_t h = s->param2;
            // 顺时针绘制矩形
            _processLine(x, y, x, y + h);         // 左边
            _processLine(x, y + h, x + w, y + h); // 上边
            _processLine(x + w, y + h, x + w, y); // 右边
            _processLine(x + w, y, x, y);         // 下边
        } else if (s->type == SHAPE_CIRCLE) {
            int16_t cx = s->x;
            int16_t cy = s->y;
            int16_t r = s->param1;
            // Draw circle using 16 segments
            const int segs = 16;
            int16_t lastX = cx + r;
            int16_t lastY = cy;
            for (int i = 1; i <= segs; i++) {
                float angle = (float)i * 6.283185f / (float)segs;
                int16_t nextX = cx + (int16_t)(cos(angle) * r);
                int16_t nextY = cy + (int16_t)(sin(angle) * r);
                _processLine(lastX, lastY, nextX, nextY);
                lastX = nextX;
                lastY = nextY;
            }
        } else if (s->type == SHAPE_STRING && s->text != NULL) {
            int16_t cursorX = s->x - (int16_t)s->scroll; // Apply scroll to X (Horizontal Marquee)
            int16_t cursorY = s->y; 
            // Use new scale fields if available (param1 was old scale)
            // If param1 is set (old API), use it. If scale_x/y set (new API), use them.
            // Actually, addString sets param1, DRAW_AddString sets scale_x/y.
            // Let's unify: addString sets scale_x=scale_y=scale.
            uint16_t sx = s->scale_x;
            uint16_t sy = s->scale_y;
            
            const char* p = s->text;
            
            while (*p) {
                if (*p == ' ') {
                    cursorX += 4 * sx; // Space width
                } else {
                    CharPattern cp = _getPattern(*p);
                    if (cp.lines) {
                        float min_x, max_x;
                        _getCharBounds(cp, min_x, max_x);

                        for (uint8_t k = 0; k < cp.count; k++) {
                            float lx0 = cp.lines[k].x0;
                            float ly0 = cp.lines[k].y0;
                            float lx1 = cp.lines[k].x1;
                            float ly1 = cp.lines[k].y1;

                            if (cp.isLarge) {
                                lx0 /= 400.0f; ly0 /= 400.0f;
                                lx1 /= 400.0f; ly1 /= 400.0f;
                            }

                            // 转换到屏幕坐标 (运行时转整型)
                            int16_t x0 = cursorX + (int16_t)(lx0 * sx);
                            int16_t y0 = cursorY + (int16_t)(ly0 * sy);
                            int16_t x1 = cursorX + (int16_t)(lx1 * sx);
                            int16_t y1 = cursorY + (int16_t)(ly1 * sy);
                            
                            _processLine(x0, y0, x1, y1);
                        }
                        
                        // 动态字间距
                        float width = max_x - min_x;
                        if (width < 1.0f) width = 4.0f; 
                        
                        // Use custom spacing if set, else default calculation
                        if (s->spacing > 0) {
                             cursorX += s->spacing;
                        } else {
                             cursorX += (int16_t)((width + 1.5f) * sx);
                        }
                    }
                }
                p++;
            }
        }
    }

    // 3. 交换缓冲区
    if (currentDrawMode == DRAW_MODE_CPU) {
        uint8_t temp = activeBufferIdx;
        activeBufferIdx = backBufferIdx;
        backBufferIdx = temp;
    } else {
        uint8_t temp = activeDmaIdx;
        activeDmaIdx = backDmaIdx;
        backDmaIdx = temp;
        dmaReadIndex = 0;
    }
}

// --- 渲染逻辑 ---

void IRAM_ATTR _resetLine() {
    uint16_t total = rawLineCounts[activeBufferIdx];
    if (total == 0) {
        // 如果没有线，停留在 0,0
        curr_x_fp = 0;
        curr_y_fp = 0;
        inc_x_fp = 0;
        inc_y_fp = 0;
        steps_remaining = 100; // 随意延时
        return;
    }

    // 确保索引有效
    if (currentLineIndex >= total) {
        currentLineIndex = 0;
    }

    // 加载当前线段坐标
    int16_t x0 = rawLines[activeBufferIdx][currentLineIndex][0];
    int16_t y0 = rawLines[activeBufferIdx][currentLineIndex][1];
    int16_t x1 = rawLines[activeBufferIdx][currentLineIndex][2];
    int16_t y1 = rawLines[activeBufferIdx][currentLineIndex][3];

    // 计算距离和步数
    int16_t dx = x1 - x0;
    int16_t dy = y1 - y0;
    int16_t dist = (i_abs(dx) > i_abs(dy)) ? i_abs(dx) : i_abs(dy);
    
    // 计算步数，至少1步
    uint16_t steps = dist / drawStepSize;
    if (steps == 0) steps = 1;
    
    steps_remaining = steps;

    // 初始化定点数坐标 (左移16位)
    curr_x_fp = (int32_t)x0 << 16;
    curr_y_fp = (int32_t)y0 << 16;

    // 计算每步增量 (左移16位后除以步数)
    inc_x_fp = ((int32_t)dx << 16) / steps;
    inc_y_fp = ((int32_t)dy << 16) / steps;
}

void DRAW_Init() {
    // 初始化默认图形 (例如一个 X)
    DRAW_Clear();
    DRAW_AddLine(0, 0, 2047, 2047);
    DRAW_AddLine(0, 2047, 2047, 0);
    DRAW_Render();

    _resetLine();
}

void IRAM_ATTR DRAW_GetNextPoint(uint16_t &outX, uint16_t &outY) {
    if (currentDrawMode == DRAW_MODE_DMA) {
        if (dmaBuffers[activeDmaIdx] && dmaBufferCounts[activeDmaIdx] > 0) {
            uint32_t val = dmaBuffers[activeDmaIdx][dmaReadIndex];
            // Decode and scale to 16-bit DAC range (0..2047 -> 0..65504)
            // Same scaling as CPU mode: x << 5
            int16_t x = (int16_t)(val >> 16);
            int16_t y = (int16_t)(val & 0xFFFF);
            outX = (uint16_t)(x << 5);
            outY = (uint16_t)(y << 5);
            
            dmaReadIndex++;
            if (dmaReadIndex >= dmaBufferCounts[activeDmaIdx]) {
                dmaReadIndex = 0;
            }
        } else {
            outX = 32768; // Center
            outY = 32768;
        }
        return;
    }

    // 1. 输出当前点
    outX = (uint16_t)(curr_x_fp >> 11);
    outY = (uint16_t)(curr_y_fp >> 11);

    // 2. 计算下一点
    if (steps_remaining == 0) {
        // 切换到下一条线
        currentLineIndex++;
        uint16_t total = rawLineCounts[activeBufferIdx];
        if (currentLineIndex >= total) {
            currentLineIndex = 0;
        }
        _resetLine(); 
    } else {
        curr_x_fp += inc_x_fp;
        curr_y_fp += inc_y_fp;
        steps_remaining--;
    }
}
