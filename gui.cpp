#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <windows.h>
#include <graphics.h>
#pragma comment(lib, "EasyXw.lib")

#define MAX_N 800          /* 地点 + 路口节点总数上限 */
#define INF 1e9
static double meterPerPx = 5.0;   /* 地图比例尺：407 宽背景时 1 像素 ≈ 5 米，按背景实际尺寸调整 */
static int snapRange = 8;       /* 路口合并半径（像素），按背景尺寸动态调整 */
#define WALK_MPS 1.25      /* 步行速度：1.25 米/秒（75 米/分钟）*/

/* 所有节点（地点 + 路口）的坐标；路口节点由 buildRoadGraph 从标定拐点创建 */
static int nodeX[MAX_N], nodeY[MAX_N], nodeCount;
static double dispScale = 1.0;   /* 窗口缩放比例（屏幕放不下时缩小显示）*/
#define DX(x) ((int)((x) * dispScale))

/* ========== 与 main.c 相同的部分：图结构、建图、加载、Dijkstra ========== */

typedef struct {
    int id;
    char name[50];
    char type[20];
    int hot;
} Place;

typedef struct EdgeNode {
    int to;
    double dist;
    double time;
    int metro;              /* 1=地铁边，只在地铁方式下通行 */
    struct EdgeNode *next;
} EdgeNode;

typedef struct {
    Place info;
    EdgeNode *first;
} VertexNode;

typedef struct {
    VertexNode v[MAX_N];
    int vn, en;
} MapGraph;

void addEdge(MapGraph *g, int from, int to, double dist, double time, int metro) {
    EdgeNode *newEdge = (EdgeNode *)malloc(sizeof(EdgeNode));
    newEdge->to = to;
    newEdge->dist = dist;
    newEdge->time = time;
    newEdge->metro = metro;
    newEdge->next = g->v[from].first;
    g->v[from].first = newEdge;
}

int loadMap(MapGraph *g, const char *placeFile, const char *roadFile) {
    FILE *fp;
    char line[256];
    g->vn = 0;
    g->en = 0;
    fp = fopen(placeFile, "r");
    if (!fp) { printf("无法打开地点文件 %s\n", placeFile); return 0; }
    while (fgets(line, sizeof(line), fp)) {
        int id, hot;
        char name[50], type[20];
        /* 按 | 分隔解析：编号|名称|类型|热度 */
        if (sscanf(line, "%d|%49[^|]|%19[^|]|%d", &id, name, type, &hot) == 4) {
            g->v[id].info.id = id;
            strcpy(g->v[id].info.name, name);
            strcpy(g->v[id].info.type, type);
            g->v[id].info.hot = hot;
            g->v[id].first = NULL;
            g->vn++;
        }
    }
    fclose(fp);

    fp = fopen(roadFile, "r");
    if (!fp) { printf("无法打开道路文件 %s\n", roadFile); return 0; }
    while (fgets(line, sizeof(line), fp)) {
        int a, b;
        double dist, time;
        /* 按 | 分隔解析：起点|终点|距离|耗时 */
        if (sscanf(line, "%d|%d|%lf|%lf", &a, &b, &dist, &time) == 4) {
            addEdge(g, a, b, dist, time, 0);
            addEdge(g, b, a, dist, time, 0);
            g->en++;
        }
    }
    fclose(fp);
    return 1;
}

/* 出行方式：名称与平均速度（km/h），下标即方式编号（1 骑自行车 2 驾车 3 电动车 4 步行 5 地铁）*/
static const wchar_t *modeName[6] = {L"", L"骑自行车", L"驾车", L"电动车", L"步行", L"地铁"};
static const double speedKmh[6] = {0, 10, 25, 25, 5, 60};

/* 地铁线路端点对：郑大科技园C口(25)、郑大科技园B口(26) ↔ 河南工业大学E口(27)，
 * 郑州大学C口(28) ↔ 上面三个地铁口；
 * 地铁口不连校园路网，同一对端点之间只能坐地铁直达 */
static const int metroLine[][2] = {
    {25, 27}, {26, 27}, {28, 25}, {28, 26}, {28, 27}
};
#define METRO_LINES (int)(sizeof(metroLine) / sizeof(metroLine[0]))

/* 地铁口判定：类型为"地铁口"的地点 */
static int isMetroPlace(MapGraph *g, int id) {
    return id >= 1 && id <= g->vn && strcmp(g->v[id].info.type, "地铁口") == 0;
}

/* 校门判定：校内到地铁口可经校门走出校园 */
static int isGatePlace(MapGraph *g, int id) {
    return id >= 1 && id <= g->vn && strcmp(g->v[id].info.type, "校门") == 0;
}

/* 两个地点是否在同一段地铁线路上（郑大科技园C口/B口 ↔ 河南工业大学E口）*/
static int metroPair(MapGraph *g, int a, int b) {
    int k;
    if (!isMetroPlace(g, a) || !isMetroPlace(g, b)) return 0;
    for (k = 0; k < METRO_LINES; k++)
        if ((a == metroLine[k][0] && b == metroLine[k][1]) ||
            (a == metroLine[k][1] && b == metroLine[k][0]))
            return 1;
    return 0;
}

/* Dijkstra 按时间求最快路径，路径存入 path
 * mode: 1 骑车 2 驾车 3 电动车 4 步行 5 地铁
 * 边权统一为"预估通行秒数"：步行用边数据里的 time 字段（秒），其他方式按 距离/速度 换算秒
 * 状态 = (节点u, 来路p)：每次转弯加微小罚时，总耗时相同的路线自动选更直的，
 * 避免在等距的矩形路网里走成锯齿形（看起来像多出分支路段）*/
#define TURN_PEN 0.5   /* 每次转弯罚时（秒），远小于任何绕行代价 */
void dijkstraTime(MapGraph *g, int start, int end, int mode, int *path, int *pathLen) {
    static double ts[MAX_N * MAX_N];        /* ts[p*MAX_N+u]: 从 p 来到达 u 的最快耗时 */
    static int pre[MAX_N * MAX_N];          /* 该状态的前驱状态 id */
    static unsigned char vis[MAX_N * MAX_N];
    static int reach[MAX_N * 8];            /* 已触达状态列表（选择当前最优用）*/
    int rn = 0, i, n;
    EdgeNode *e;

    n = (nodeCount > g->vn) ? nodeCount : g->vn;
    memset(vis, 0, (size_t)(n + 1) * MAX_N);
    for (i = 0; i < (n + 1) * MAX_N; i++) ts[i] = INF;

    /* 起点状态：来路 0 = 无方向，无转弯罚时 */
    ts[start] = 0;
    pre[start] = -1;
    reach[rn++] = start;

    while (1) {
        /* 未定状态里找耗时最小的 */
        int sid = -1, u, p, v;
        double best = INF;
        for (i = 0; i < rn; i++)
            if (!vis[reach[i]] && ts[reach[i]] < best) { best = ts[reach[i]]; sid = reach[i]; }
        if (sid == -1) break;
        u = sid % MAX_N;
        if (u == end) break;
        vis[sid] = 1;
        p = sid / MAX_N;
        for (e = g->v[u].first; e; e = e->next) {
            if (e->metro != (mode == 5)) continue;   /* 地铁边只在地铁方式走，地面方式禁走地铁边 */
            double w = (mode == 4) ? e->time
                                   : e->dist / 1000.0 / speedKmh[mode] * 3600.0;
            int nsid, first = 1, k;
            v = e->to;
            if (nodeCount > g->vn && v <= g->vn && v != end && !isGatePlace(g, v))
                continue;   /* 路口路网：中间不经过其他地点（校门除外，出校门可到地铁口）*/
            if (p != 0) {   /* 转弯检测：来路方向和去路方向不共线就加罚时 */
                int d1x = nodeX[u] - nodeX[p], d1y = nodeY[u] - nodeY[p];
                int d2x = nodeX[v] - nodeX[u], d2y = nodeY[v] - nodeY[u];
                if (d1x * d2y - d1y * d2x != 0) w += TURN_PEN;
            }
            nsid = u * MAX_N + v;
            if (vis[nsid] || best + w >= ts[nsid]) continue;
            ts[nsid] = best + w;
            pre[nsid] = sid;
            for (k = 0; k < rn; k++)          /* 已在列表里就不用重复加 */
                if (reach[k] == nsid) { first = 0; break; }
            if (first && rn < MAX_N * 8) reach[rn++] = nsid;
        }
    }

    /* 找到 end 的最优状态（任意来路），沿前驱还原路径 */
    {
        int sid = -1, cnt = 0;
        double best = INF;
        for (i = 0; i < rn; i++)
            if (reach[i] % MAX_N == end && ts[reach[i]] < best) { best = ts[reach[i]]; sid = reach[i]; }
        if (sid == -1) { *pathLen = 0; return; }   /* 不可达 */
        while (sid != -1 && cnt < MAX_N) {
            path[cnt++] = sid % MAX_N;
            sid = pre[sid];
        }
        *pathLen = cnt;
        for (i = 0; i < cnt; i++) { int t2 = path[i]; path[i] = path[cnt - 1 - i]; path[cnt - 1 - i] = t2; }
    }
}

/* ========== 坐标 ==========
 * 优先读 coords.dat（用 calib.exe 在真实地图上标定出来的坐标）；
 * 没有 coords.dat 就使用示意图坐标，程序照样能画图、选路线。 */

static IMAGE bg;          /* 真实地图背景图（可选）*/
static int useImage = 0;
static int coordsFromFile = 0;   /* coords.dat 是否标定过 */
static int px[MAX_N], py[MAX_N];
static int s_mx = 0, s_my = 0;   /* 最近一次鼠标位置（按钮悬停效果用）*/
static int s_ox = 0, s_oy = 0;   /* 地图拖动偏移（平移量）*/
static int s_dragging = 0;       /* 左键是否按住中 */
static int s_dragLastX = 0, s_dragLastY = 0;
static int s_dragMoved = 0;      /* 移动超过阈值=拖动，否则=点击 */
static int s_mapW = 1000, s_mapH = 680;  /* 地图尺寸 */
static int s_winW = 1020, s_winH = 700;  /* 窗口尺寸 */

static int g_loggedIn = 0;        /* 登录状态（本次运行内有效）*/
static wchar_t g_stuId[32];       /* 当前登录学号 */

/* 当前选定的起点/终点和路线结果：点击和搜索都会修改，所以放在全局 */
static int s_start = -1, s_end = -1;
static int s_path[MAX_N], s_len = 0;
static double s_totalDist = 0, s_totalTime = 0;

/* 示意图坐标（与报告中的地图示意图一致）*/
static const int def_px[MAX_N] = {0, 540, 950, 500, 500, 50, 650, 430, 420, 560, 660,
                                  700, 290, 230, 310, 160, 430, 120, 760, 830, 770};
static const int def_py[MAX_N] = {0, 300, 350, 45, 660, 350, 230, 190, 430, 470, 420,
                                  310, 170, 310, 470, 530, 330, 430, 180, 280, 510};

void loadCoords(void) {
    FILE *fp = fopen("coords.dat", "r");
    int id, x, y, i;
    if (!fp) {
        for (i = 1; i <= 20; i++) { px[i] = def_px[i]; py[i] = def_py[i]; }
        return;
    }
    coordsFromFile = 1;
    while (fscanf(fp, "%d|%d|%d", &id, &x, &y) == 3 && id >= 1 && id < MAX_N) {
        px[id] = x;
        py[id] = y;
    }
    fclose(fp);
}

/* ========== 道路拐点：路线沿真实道路画折线 ==========
 * roads_points.dat 每行：a|b|x1|y1|x2|y2|...
 * 用 roadcalib.exe 在地图上标定生成；没有该文件时路线画直线 */
#define MAX_P 12          /* 旧拐点格式（roads_points.dat）每对节点折线点数 */
#define MAX_PL 64         /* 新标定格式（roads_custom.dat）一条路折线点数上限 */
static int rpN[MAX_N][MAX_N];
static int rpX[MAX_N][MAX_N][MAX_P], rpY[MAX_N][MAX_N][MAX_P];

/* ========== 路口节点路网 ==========
 * 节点 = 地点(1..g->vn) + 路口(g->vn+1..nodeCount)，
 * 路口就是每条路标定的拐点；边 = 相邻节点之间的路段，
 * 边权统一为"预估通行秒数"（步行 1.25 米/秒）*/

/* 直达列表（探索功能显示用）：读 roads.dat 的地点直达关系，不参与寻路 */
static int dl[MAX_N][MAX_N], dln[MAX_N];
static double dld[MAX_N][MAX_N];
void loadDirectList(void) {
    FILE *fp = fopen("roads.dat", "r");
    char lin[2048];
    int a, b;
    double dist;
    if (!fp) return;
    while (fgets(lin, sizeof(lin), fp)) {
        if (sscanf(lin, "%d|%d|%lf", &a, &b, &dist) == 3 && a >= 1 && a < MAX_N && b >= 1 && b < MAX_N) {
            dl[a][dln[a]] = b;  dld[a][dln[a]] = dist;  dln[a]++;
            dl[b][dln[b]] = a;  dld[b][dln[b]] = dist;  dln[b]++;
        }
    }
    fclose(fp);
}

void loadRoadPoints(void) {
    FILE *fp = fopen("roads_points.dat", "r");
    char line[2048];
    if (!fp) return;
    while (fgets(line, sizeof(line), fp)) {
        int a = 0, b = 0, k = 0;
        char *tok = strtok(line, "|");
        if (tok) a = atoi(tok);
        tok = strtok(NULL, "|");
        if (tok) b = atoi(tok);
        if (a >= 1 && b >= 1 && a < MAX_N && b < MAX_N) {
            while (k < MAX_P) {
                int x, y;
                tok = strtok(NULL, "|");
                if (!tok) break;
                x = atoi(tok);
                tok = strtok(NULL, "|");
                if (!tok) break;
                y = atoi(tok);
                rpX[a][b][k] = x;  rpY[a][b][k] = y;
                rpX[b][a][k] = x;  rpY[b][a][k] = y;   /* 双向同一条路 */
                k++;
            }
            rpN[a][b] = k;  rpN[b][a] = k;
        }
    }
    fclose(fp);
}

/* ========== 地点-路口路网 ==========
 * 节点 = 地点(1..g->vn) + 路口(g->vn+1..nodeCount)。
 * 路口是用户在地图上为每个地点点的"最近路口"（place_junctions.dat），
 * 每条路按折线吸附到经过的路口/端点拆成路段，
 * 边权 = 预估通行秒数（步行 1.25 米/秒）。
 * 寻路：出发地 → 它的最近路口 → 沿道路 → 目的地最近路口 → 目的地 */
#define MAX_J 500
static int jx[MAX_J], jy[MAX_J], jCount;
static int jlist[MAX_N][MAX_J], jln[MAX_N];

/* 读 place_junctions.dat：地点id|x|y|x|y|...（8 像素内合并同一路口）*/
void loadJunctions(void) {
    FILE *fp = fopen("place_junctions.dat", "r");
    char lin[2048];
    int i;
    jCount = 0;
    for (i = 0; i < MAX_N; i++) jln[i] = 0;
    if (!fp) return;
    while (fgets(lin, sizeof(lin), fp)) {
        int pid = 0, k = 0, xs[MAX_J], ys[MAX_J];
        char *tok = strtok(lin, "|");
        if (tok) pid = atoi(tok);
        while (k < MAX_J) {
            tok = strtok(NULL, "|");
            if (!tok) break;
            xs[k] = atoi(tok);
            tok = strtok(NULL, "|");
            if (!tok) break;
            ys[k] = atoi(tok);
            k++;
        }
        if (pid < 1 || pid >= MAX_N) continue;
        for (i = 0; i < k; i++) {
            int j = 0;
            while (j < jCount && !(abs(jx[j] - xs[i]) <= snapRange && abs(jy[j] - ys[i]) <= snapRange)) j++;
            if (j == jCount && jCount < MAX_J) { jx[jCount] = xs[i];  jy[jCount] = ys[i];  jCount++; }
            if (j < jCount && jln[pid] < MAX_J) jlist[pid][jln[pid]++] = j;
        }
    }
    fclose(fp);
}

/* 找路口节点：8 像素内与已有"路口"重合就返回，否则 -1。
 * 刻意不合并到地点——地点只能通过"最近路口"连接路网，
 * 保证路线从一个地点到另一个地点中间只经过路口、不经过地点 */
static int snapOnly(MapGraph *g, int x, int y) {
    int i;
    for (i = g->vn + 1; i <= nodeCount; i++)
        if (abs(nodeX[i] - x) <= snapRange && abs(nodeY[i] - y) <= snapRange) return i;
    return -1;
}

/* 找路口节点，找不到就新建一个路口（路的端点必须是路口）*/
static int snapOrCreate(MapGraph *g, int x, int y) {
    int id = snapOnly(g, x, y);
    if (id != -1) return id;
    nodeCount++;
    nodeX[nodeCount] = x;  nodeY[nodeCount] = y;
    return nodeCount;
}

/* 路段折线表：路径绘制时沿折线画，不会拉直 */
#define MAX_SEG 1200
typedef struct { int u, v, n; int x[MAX_PL], y[MAX_PL]; } RoadSeg;
static RoadSeg rsegs[MAX_SEG];
static int rsegCount;

static void addRoadSeg(MapGraph *g, int u, int v, double meters,
                       const int *pts, int np) {
    /* pts: x,y 交错排列；np = 折线点数（不含两端节点）*/
    int secs = (int)(meters / WALK_MPS + 0.5);
    int k;
    if (secs < 1) secs = 1;
    addEdge(g, u, v, meters, secs, 0);
    addEdge(g, v, u, meters, secs, 0);   /* 道路双向通行 */
    if (rsegCount < MAX_SEG) {
        RoadSeg *s = &rsegs[rsegCount++];
        s->u = u;  s->v = v;  s->n = 0;
        for (k = 0; k < np && s->n < MAX_PL; k++) {
            s->x[s->n] = pts[k * 2];  s->y[s->n] = pts[k * 2 + 1];  s->n++;
        }
    }
}

/* 用标定的路口和路口到路口的路构建路网：
 * 节点 = 地点 + junctions.dat 的路口 + 路的端点（8 像素内合并）；
 * 每条路拆成 路口→…→路口 的路段（中间弯道点只作折线）；
 * 地点只连它点过的最近路口。边权 = 预估通行秒数 */
void buildRoadGraph(MapGraph *g) {
    FILE *fp;
    char lin[2048];
    int i, p;
    nodeCount = g->vn;
    for (i = 1; i <= g->vn; i++) { nodeX[i] = px[i];  nodeY[i] = py[i]; }
    /* 先标的路口 */
    fp = fopen("junctions.dat", "r");
    if (fp) {
        while (fgets(lin, sizeof(lin), fp)) {
            int xs[MAX_J], ys[MAX_J], n = 0;
            char *tok = strtok(lin, "|");
            while (n < MAX_J && tok) {
                xs[n] = atoi(tok);
                tok = strtok(NULL, "|");
                if (!tok) break;
                ys[n] = atoi(tok);
                tok = strtok(NULL, "|");
                n++;
            }
            for (i = 0; i < n; i++) snapOrCreate(g, xs[i], ys[i]);
        }
        fclose(fp);
    }
    for (i = 1; i < MAX_N; i++) g->v[i].first = NULL;
    g->en = 0;
    rsegCount = 0;
    /* 路口到路口的路：端点和经过的路口都是节点，弯道点只作折线 */
    fp = fopen("roads_custom.dat", "r");
    if (fp) {
        while (fgets(lin, sizeof(lin), fp)) {
            int xs[MAX_PL * 2], ys[MAX_PL * 2], n = 0;
            char *tok = strtok(lin, "|");
            while (n < MAX_PL * 2 && tok) {
                xs[n] = atoi(tok);
                tok = strtok(NULL, "|");
                if (!tok) break;
                ys[n] = atoi(tok);
                tok = strtok(NULL, "|");
                n++;
            }
            if (n < 2) continue;
            {
                int prev = snapOrCreate(g, xs[0], ys[0]);
                int pts[MAX_PL * 2], pn2 = 0, lx = xs[0], ly = ys[0];
                double acc = 0;
                for (i = 1; i < n; i++) {
                    acc += hypot((double)(xs[i] - lx), (double)(ys[i] - ly));
                    if (pn2 < MAX_PL * 2) { pts[pn2] = xs[i];  pts[pn2 + 1] = ys[i];  pn2 += 2; }
                    {
                        int nid = snapOnly(g, xs[i], ys[i]);
                        if (nid != -1 && nid != prev) {
                            addRoadSeg(g, prev, nid, acc * meterPerPx, pts, pn2 / 2);
                            prev = nid;  acc = 0;  pn2 = 0;
                        }
                    }
                    lx = xs[i];  ly = ys[i];
                }
                {
                    int endN = snapOrCreate(g, xs[n - 1], ys[n - 1]);
                    if (endN != prev)
                        addRoadSeg(g, prev, endN, acc * meterPerPx, pts, pn2 / 2);
                }
            }
        }
        fclose(fp);
    }
    /* 地点 ↔ 它点的每个最近路口（按坐标找节点）*/
    for (p = 1; p <= g->vn; p++)
        for (i = 0; i < jln[p]; i++) {
            int nid = snapOnly(g, jx[jlist[p][i]], jy[jlist[p][i]]);
            if (nid != -1) {
                double m = hypot((double)(px[p] - nodeX[nid]), (double)(py[p] - nodeY[nid])) * meterPerPx;
                addRoadSeg(g, p, nid, m, NULL, 0);
            }
        }

    /* 地铁线路：郑大科技园C口(25)、郑大科技园B口(26) ↔ 河南工业大学E口(27)、
     * 郑州大学C口(28) ↔ 上面三个；只在地铁方式(5)下通行 */
    for (i = 0; i < METRO_LINES; i++) {
        int a = metroLine[i][0], b = metroLine[i][1];
        double m = hypot((double)(nodeX[a] - nodeX[b]), (double)(nodeY[a] - nodeY[b])) * meterPerPx;
        int secs = (int)(m / (speedKmh[5] / 3.6) + 0.5);
        if (secs < 1) secs = 1;
        addEdge(g, a, b, m, secs, 1);
        addEdge(g, b, a, m, secs, 1);
    }
    /* 地铁口 → 最近校门（步行边）：校内地点可经校门走到地铁口 */
    for (i = 1; i <= g->vn; i++) {
        int gate = -1, k2;
        double best2 = INF;
        if (!isMetroPlace(g, i)) continue;
        for (k2 = 1; k2 <= g->vn; k2++) {
            if (k2 == i || !isGatePlace(g, k2)) continue;
            double d2 = hypot((double)(nodeX[i] - nodeX[k2]), (double)(nodeY[i] - nodeY[k2]));
            if (d2 < best2) { best2 = d2; gate = k2; }
        }
        if (gate != -1) {
            double m = best2 * meterPerPx;
            int secs = (int)(m / WALK_MPS + 0.5);
            if (secs < 1) secs = 1;
            addEdge(g, i, gate, m, secs, 0);
            addEdge(g, gate, i, m, secs, 0);
        }
    }
}

/* 地点名/类型 UTF-8 -> 宽字符，EasyX 画中文必须用宽字符 */
static wchar_t wname[MAX_N][50];
static wchar_t wtype[MAX_N][20];
void initNames(MapGraph *g) {
    int i;
    for (i = 1; i <= g->vn; i++) {
        MultiByteToWideChar(CP_UTF8, 0, g->v[i].info.name, -1, wname[i], 50);
        MultiByteToWideChar(CP_UTF8, 0, g->v[i].info.type, -1, wtype[i], 20);
    }
}

/* 地点类型 -> 颜色（与报告示意图一致）*/
COLORREF colorOf(const char *type) {
    if (strcmp(type, "校门") == 0) return RED;
    if (strcmp(type, "教学区") == 0) return BLUE;
    if (strcmp(type, "宿舍") == 0) return GREEN;
    if (strcmp(type, "地标") == 0) return RGB(243, 156, 18);
    if (strcmp(type, "运动") == 0) return CYAN;
    if (strcmp(type, "医疗") == 0) return MAGENTA;
    return RGB(128, 0, 128);   /* 核心（图书馆）*/
}

/* 鼠标点到哪个地点：命中半径按显示比例换算（客户区 30 像素），重叠时取最近 */
int pickNode(MapGraph *g, int mx, int my) {
    int i, best = -1, bestD = 0x7fffffff;
    int R = (int)(30.0 / dispScale);
    for (i = 1; i <= g->vn; i++) {
        int dx = mx - px[i], dy = my - py[i];
        int d = dx * dx + dy * dy;
        if (d <= R * R && d < bestD) { bestD = d; best = i; }
    }
    return best;
}

/* 沿路段折线画两个节点之间的路线（找不到折线记录就画直线）*/
static void drawSegPolyline(int u, int v, int ox, int oy) {
    int k, j;
    for (k = 0; k < rsegCount; k++) {
        RoadSeg *s = &rsegs[k];
        if ((s->u == u && s->v == v) || (s->u == v && s->v == u)) {
            int cx = DX(nodeX[u]) + ox, cy = DX(nodeY[u]) + oy;
            for (j = 0; j < s->n; j++) {
                line(cx, cy, DX(s->x[j]) + ox, DX(s->y[j]) + oy);
                cx = DX(s->x[j]) + ox;  cy = DX(s->y[j]) + oy;
            }
            line(cx, cy, DX(nodeX[v]) + ox, DX(nodeY[v]) + oy);
            return;
        }
    }
    line(DX(nodeX[u]) + ox, DX(nodeY[u]) + oy, DX(nodeX[v]) + ox, DX(nodeY[v]) + oy);
}

/* 画整幅地图：背景、道路、路线、地点、起终点标记、状态栏 */
void drawMap(MapGraph *g, int start, int end, int *path, int len, double totalDist) {
    int i;
    EdgeNode *e;
    wchar_t buf[128];

    /* 每帧先清成浅色背景，再画地图（不清的话上一帧会留下残影）*/
    setbkcolor(RGB(240, 244, 250));
    cleardevice();
    if (useImage) putimage(s_ox, s_oy, &bg);    /* 真实地图做背景（跟随偏移）*/
    {
        int ox = s_ox, oy = s_oy;   /* 地图偏移量，下面所有坐标都要加 */

    /* 道路（灰色细线 + 距离标注）——真实地图模式下不画，保持照片干净 */
    if (!useImage) {
        setlinecolor(LIGHTGRAY);
        for (i = 1; i <= g->vn; i++)
            for (e = g->v[i].first; e; e = e->next) {
                line(DX(px[i]) + ox, DX(py[i]) + oy, DX(px[e->to]) + ox, DX(py[e->to]) + oy);
                swprintf(buf, 128, L"%d", (int)e->dist);
                settextstyle(12, 0, L"微软雅黑");
                setbkmode(TRANSPARENT);
                settextcolor(DARKGRAY);
                outtextxy((DX(px[i]) + DX(px[e->to])) / 2 + ox - textwidth(buf) / 2,
                          (DX(py[i]) + DX(py[e->to])) / 2 + oy - 6, buf);
            }
    }

    /* 最短路线（红色粗线 + 浅色光晕）：沿路段折线画，贴真实道路 */
    if (len > 0) {
        setlinestyle(PS_SOLID, 7);
        setlinecolor(RGB(255, 130, 130));
        for (i = 0; i + 1 < len; i++)
            drawSegPolyline(path[i], path[i + 1], ox, oy);
        setlinestyle(PS_SOLID, 3);
        setlinecolor(RED);
        for (i = 0; i + 1 < len; i++)
            drawSegPolyline(path[i], path[i + 1], ox, oy);
        setlinestyle(PS_SOLID, 1);
    }

    /* 路口蓝点只在标定工具里显示，导航界面不画，保持地图干净 */

    /* 地点圆点 + 名称——示意图模式画大圆+名称，真实地图模式画白底彩点按键 */
    if (!useImage) {
        for (i = 1; i <= g->vn; i++) {
            int r = 7 + g->v[i].info.hot / 10;   /* 热度越高圆越大，与报告示意图一致 */
            setfillcolor(colorOf(g->v[i].info.type));
            setlinecolor(WHITE);
            fillcircle(DX(px[i]) + ox, DX(py[i]) + oy, r);
            setbkmode(OPAQUE);
            setbkcolor(WHITE);
            settextcolor(BLACK);
            settextstyle(14, 0, L"微软雅黑");
            outtextxy(DX(px[i]) + ox - textwidth(wname[i]) / 2, DX(py[i]) + oy + r + 4, wname[i]);
        }
    } else {
        /* 真实地图模式：每个地点一个白底彩色圆点按键，悬停时再显示名称 */
        for (i = 1; i <= g->vn; i++) {
            int cx = DX(px[i]) + ox, cy = DX(py[i]) + oy;
            setfillcolor(WHITE);
            setlinecolor(WHITE);
            fillcircle(cx, cy, 8);
            setfillcolor(colorOf(g->v[i].info.type));
            setlinecolor(WHITE);
            fillcircle(cx, cy, 5);
        }
    }

    /* 真实地图模式下：鼠标靠近地点时画提示环 + 名称，方便知道哪里能点 */
    if (useImage) {
        int hov = pickNode(g, (int)((s_mx - s_ox) / dispScale), (int)((s_my - s_oy) / dispScale));
        if (hov != -1) {
            setlinecolor(RGB(150, 190, 235));
            setlinestyle(PS_SOLID, 2);
            circle(DX(px[hov]) + ox, DX(py[hov]) + oy, 16);
            setlinestyle(PS_SOLID, 1);
            settextcolor(RGB(30, 60, 110));
            settextstyle(13, 0, L"微软雅黑");
            outtextxy(DX(px[hov]) + ox + 12, DX(py[hov]) + oy - 20, wname[hov]);
        }
    }

    /* 起点绿色实心圆（标"起"），终点红色实心圆（标"终"）*/
    settextstyle(12, 0, L"微软雅黑");
    setbkmode(TRANSPARENT);
    if (start != -1) {
        setfillcolor(GREEN);
        setlinecolor(WHITE);
        fillcircle(DX(px[start]) + ox, DX(py[start]) + oy, 14);
        settextcolor(WHITE);
        outtextxy(DX(px[start]) + ox - textwidth(L"起") / 2, DX(py[start]) + oy - 8, L"起");
    }
    if (end != -1) {
        setfillcolor(RED);
        setlinecolor(WHITE);
        fillcircle(DX(px[end]) + ox, DX(py[end]) + oy, 14);
        settextcolor(WHITE);
        outtextxy(DX(px[end]) + ox - textwidth(L"终") / 2, DX(py[end]) + oy - 8, L"终");
    }
    }

    /* 顶部信息条：实时时钟 + 状态提示 */
    setfillcolor(RGB(216, 236, 255));
    setlinecolor(RGB(150, 190, 235));
    fillrectangle(0, 0, getwidth() - 1, 36);
    settextstyle(16, 0, L"微软雅黑");
    setbkmode(TRANSPARENT);
    settextcolor(RGB(30, 60, 110));
    {
        SYSTEMTIME st;                 /* 系统当前时间 */
        GetLocalTime(&st);
        swprintf(buf, 128, L"%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);
        outtextxy(10, 8, buf);         /* 左上角实时时钟 */
    }
    if (start == -1)
        swprintf(buf, 128, L"点击地点确认为出发地　F1路线检测 F2问题反馈　右键清空 ESC退出");
    else if (end == -1)
        swprintf(buf, 128, L"出发地：%s　请点击地点并确认为目的地", wname[start]);
    else
        swprintf(buf, 128, L"出发地：%s → 目的地：%s　总距离：%.0f 米",
                 wname[start], wname[end], totalDist);
    outtextxy(100, 8, buf);            /* 状态提示放在时钟右边 */

    /* 功能按钮：圆角 + 悬停高亮 */
    {
        const wchar_t *btnName[5] = {L"搜索", L"收藏", L"收藏列表", L"取消收藏", L"美食"};
        int b;
        settextstyle(14, 0, L"微软雅黑");
        setbkmode(TRANSPARENT);
        for (b = 0; b < 5; b++) {
            int bx = 10 + b * 70;
            int hover = (s_mx >= bx && s_mx <= bx + 64 &&
                         s_my >= 40 && s_my <= 68);
            if (hover) {
                setfillcolor(RGB(140, 190, 255));
                setlinecolor(RGB(70, 130, 220));
                settextcolor(WHITE);
            } else {
                setfillcolor(RGB(225, 230, 238));
                setlinecolor(RGB(160, 170, 185));
                settextcolor(RGB(50, 60, 75));
            }
            fillroundrect(bx, 40, bx + 64, 68, 10, 10);
            outtextxy(bx + (64 - textwidth(btnName[b])) / 2,
                      40 + (28 - textheight(btnName[b])) / 2, btnName[b]);
        }
    }

    /* 右上角账号按钮：未登录显示“登录”，已登录显示“我的”（与搜索按钮同风格）*/
    {
        int bx = s_winW - 74;
        const wchar_t *txt = g_loggedIn ? L"我的" : L"登录";
        int hover = (s_mx >= bx && s_mx <= bx + 64 && s_my >= 40 && s_my <= 68);
        if (hover) {
            setfillcolor(RGB(140, 190, 255));
            setlinecolor(RGB(70, 130, 220));
            settextcolor(WHITE);
        } else {
            setfillcolor(RGB(225, 230, 238));
            setlinecolor(RGB(160, 170, 185));
            settextcolor(RGB(50, 60, 75));
        }
        fillroundrect(bx, 40, bx + 64, 68, 10, 10);
        outtextxy(bx + (64 - textwidth(txt)) / 2,
                  40 + (28 - textheight(txt)) / 2, txt);
    }

    /* 左下角图例：类型颜色说明（与报告示意图一致）*/
    {
        static const wchar_t *legName[7] = {L"校门", L"教学区", L"宿舍", L"地标",
                                            L"运动", L"医疗", L"核心"};
        static const COLORREF legColor[7] = {RED, BLUE, GREEN, RGB(243, 156, 18),
                                             CYAN, MAGENTA, RGB(128, 0, 128)};
        int lx = 10, ly = getheight() - 150, k;
        setfillcolor(WHITE);
        setlinecolor(RGB(160, 170, 185));
        fillroundrect(lx, ly, lx + 130, ly + 140, 8, 8);
        settextstyle(12, 0, L"微软雅黑");
        setbkmode(TRANSPARENT);
        settextcolor(RGB(60, 60, 60));
        for (k = 0; k < 7; k++) {
            setfillcolor(legColor[k]);
            setlinecolor(WHITE);
            fillcircle(lx + 16, ly + 16 + k * 18, 6);
            outtextxy(lx + 30, ly + 10 + k * 18, legName[k]);
        }
    }
}

/* 按宽字符名称找地点：先精确匹配，再模糊匹配（名称包含），找不到返回 -1 */
int findWName(MapGraph *g, const wchar_t *key) {
    int i;
    for (i = 1; i <= g->vn; i++)
        if (wcscmp(wname[i], key) == 0) return i;
    for (i = 1; i <= g->vn; i++)
        if (wcsstr(wname[i], key)) return i;
    return -1;
}

/* 按出行方式求最快路线：Dijkstra 按时间找路，同时算出总距离和总用时 */
void computeFastestRoute(MapGraph *g, int s, int e, int mode,
                         int *path, int *len, double *dist, double *time) {
    int i;
    EdgeNode *edge;
    dijkstraTime(g, s, e, mode, path, len);
    *dist = 0;
    *time = 0;
    if (*len == 0) return;
    for (i = 0; i + 1 < *len; i++)
        for (edge = g->v[path[i]].first; edge; edge = edge->next)
            if (edge->to == path[i + 1]) {
                *dist += edge->dist;
                *time += (mode == 4) ? edge->time
                                     : edge->dist / 1000.0 / speedKmh[mode] * 3600.0;
                break;
            }
}

/* ========== 弹窗封装：弹窗必须退出批量绘制模式，否则可能显示不出来 ========== */

int popupInput(wchar_t *buf, int max, const wchar_t *prompt, const wchar_t *title) {
    int ok;
    EndBatchDraw();
    ok = InputBox(buf, max, prompt, title, L"", 0, 0, false);
    BeginBatchDraw();
    return ok;
}

void popupMsg(const wchar_t *text, const wchar_t *title) {
    EndBatchDraw();
    MessageBoxW(GetHWnd(), text, title, MB_OK);
    BeginBatchDraw();
}

/* 是/否确认弹窗：返回 1=是 0=否 */
int popupYesNo(const wchar_t *text, const wchar_t *title) {
    int r;
    EndBatchDraw();
    r = MessageBoxW(GetHWnd(), text, title, MB_YESNO | MB_ICONQUESTION);
    BeginBatchDraw();
    return r == IDYES;
}

/* 以当前出发地/目的地计算最快路线：先选出行方式，再算并弹窗显示结果 */
static void planFromState(MapGraph *g);
/* 地铁口导航校验：涉及地铁口时必须同一线路，不满足则弹窗并返回 0 */
static int checkMetroPair(MapGraph *g, int a, int b);
static void searchHistRecord(const wchar_t *key);   /* 登录用户记录搜索历史 */
static void navHistRecord(int id);   /* 登录用户记录导航地点（设为出发地/目的地时）*/
static int navHistPick(void);        /* 选择历史导航地点，返回地点 id，取消返回 -1 */
static int foodNav(MapGraph *g, int zoneId);   /* 探索园区：显示美食并可选菜自动导航 */
static int foodSearchNav(MapGraph *g, const wchar_t *key);   /* 搜索美食：直接导航到对应园区 */
static void showRouteResult(MapGraph *g, int *path, int len, double totalDist,
                            double totalTime, int mode);   /* 显示路线结果弹窗 */
/* 探索：显示地点的类型、热度和能直达的地方 */
static void explorePlace(MapGraph *g, int id);

/* 搜索地点：选一个地点后弹出三个选择：1 设为出发地 2 设为目的地 3 探索；
 * 登录后输入 0 可查看历史导航地点快速选择 */
void guiSearch(MapGraph *g) {
    wchar_t key[64], msg[1024], tmp[128], buf[16];
    int hits[MAX_N], hitCount = 0, i, sel;
    key[0] = 0;
    if (!popupInput(key, 64, L"请输入搜索关键词（登录后输入 0 查看历史导航地点）", L"地点搜索")
        || key[0] == 0) return;
    searchHistRecord(key);   /* 登录用户每次搜索都记入历史 */
    if (key[0] == L'0' && key[1] == 0) {
        sel = navHistPick();
        if (sel == -1) return;   /* 无记录或取消 */
    } else {
        for (i = 1; i <= g->vn; i++)
            if (wcsstr(wname[i], key)) hits[hitCount++] = i;
        if (hitCount == 0) {
            if (foodSearchNav(g, key)) return;   /* 地点没匹配时试试美食 */
            popupMsg(L"没有找到包含该关键词的地点或美食", L"搜索结果");
            return;
        }
        if (hitCount > 1) {
            /* 匹配多个：先列出来，再输编号选一个 */
            msg[0] = 0;
            for (i = 0; i < hitCount; i++) {
                swprintf(tmp, 128, L"%d. %s（%s）\n", i + 1, wname[hits[i]], wtype[hits[i]]);
                wcscat(msg, tmp);
            }
            popupMsg(msg, L"搜索结果");
            buf[0] = 0;
            if (!popupInput(buf, 16, L"请输入要选择的地点编号", L"地点搜索") || buf[0] == 0) return;
            sel = _wtoi(buf);
            if (sel < 1 || sel > hitCount) { popupMsg(L"编号无效", L"提示"); return; }
            sel = hits[sel - 1];
        } else {
            sel = hits[0];
        }
    }
    /* 三个选择：1 设为出发地 2 设为目的地 3 探索 */
    buf[0] = 0;
    if (!popupInput(buf, 16, L"1.设为出发地  2.设为目的地  3.探索", L"选择操作")
        || buf[0] == 0) return;
    if (_wtoi(buf) == 1) {
        s_start = sel;  s_end = -1;  s_len = 0;   /* 设为出发地，等点击目的地 */
        navHistRecord(sel);
    } else if (_wtoi(buf) == 2) {
        if (s_start == -1) { popupMsg(L"还没有出发地，请先设置", L"提示"); return; }
        if (sel == s_start) { popupMsg(L"起点和终点相同", L"提示"); return; }
        if (!checkMetroPair(g, s_start, sel)) return;   /* 地铁口只能与同线路地铁口互达 */
        s_end = sel;  s_len = 0;
        navHistRecord(sel);
        planFromState(g);
    } else {
        explorePlace(g, sel);
    }
}

/* ========== 美食：搜索园区（荷园/松园/菊园/柳园），弹出对应美食 ==========
 * foods.dat：园区|美食名|价格|特色（UTF-8，价格单位元），可自行增删
 * 列出后可挑最爱吃的饭，再输入出发地自动导航到该园区 */
#define MAX_FOOD 200
typedef struct {
    wchar_t zone[32];   /* 园区名 */
    wchar_t name[64];   /* 美食名 */
    wchar_t feat[128];  /* 特色 */
    int price;          /* 价格（元） */
} FoodItem;
static FoodItem flist[MAX_FOOD];

/* 美食导航公共段：输入出发地 → 自动导航到园区 zoneId */
static void foodNavGo(MapGraph *g, int zoneId) {
    wchar_t buf[64];
    int src;
    buf[0] = 0;
    if (!popupInput(buf, 64, L"请输入出发地名称", L"美食导航") || buf[0] == 0) return;
    src = findWName(g, buf);
    if (src == -1) { popupMsg(L"出发地不存在，请检查名称", L"提示"); return; }
    if (isMetroPlace(g, src)) { popupMsg(L"地铁口不能作为美食导航出发地", L"提示"); return; }
    if (src == zoneId) { popupMsg(L"出发地就在该园区", L"提示"); return; }
    s_start = src;
    s_end = zoneId;
    s_len = 0;
    computeFastestRoute(g, src, zoneId, 4, s_path, &s_len, &s_totalDist, &s_totalTime);
    if (s_len == 0) {
        popupMsg(L"出发地到该园区之间没有通路", L"提示");
        s_end = -1;  s_len = 0;
    } else {
        navHistRecord(src);   /* 导航起点/终点记入历史导航地点 */
        navHistRecord(zoneId);
        showRouteResult(g, s_path, s_len, s_totalDist, s_totalTime, 4);
    }
}

static void guiFood(MapGraph *g) {
    wchar_t key[64], msg[4096], tmp[256], line[256], buf[64];
    static wchar_t lastZone[32];   /* 上一个园区标题，跨园区分组显示 */
    char raw[256];
    FILE *fp;
    int n = 0, sel;
    lastZone[0] = 0;
    key[0] = 0;
    if (!popupInput(key, 64, L"请输入园区名称（荷园/松园/菊园/柳园，输入“园”看全部）", L"美食")
        || key[0] == 0) return;
    fp = fopen("foods.dat", "r");
    if (!fp) { popupMsg(L"暂无美食数据（foods.dat）", L"美食"); return; }
    msg[0] = 0;
    while (fgets(raw, sizeof(raw), fp) && n < MAX_FOOD) {
        wchar_t zone[32], fname[64], feat[128], *p, *q;
        int price;
        raw[strcspn(raw, "\r\n")] = 0;
        if (!raw[0]) continue;
        MultiByteToWideChar(CP_UTF8, 0, raw, -1, line, 256);
        p = line;
        q = wcschr(p, L'|');
        if (!q) continue;
        *q = 0;
        wcscpy(zone, p);
        if (!wcsstr(zone, key)) continue;      /* 园区名模糊匹配 */
        p = q + 1;
        q = wcschr(p, L'|');
        if (!q) continue;
        *q = 0;
        wcscpy(fname, p);
        p = q + 1;
        price = _wtoi(p);
        q = wcschr(p, L'|');
        if (!q) continue;
        wcscpy(feat, q + 1);
        wcscpy(flist[n].zone, zone);
        wcscpy(flist[n].name, fname);
        flist[n].price = price;
        wcscpy(flist[n].feat, feat);
        if (wcscmp(zone, lastZone) != 0) {     /* 新的园区：先打标题 */
            wcscpy(lastZone, zone);
            swprintf(tmp, 256, L"\n【%s】\n", zone);
            if (wcslen(msg) + wcslen(tmp) < 4000) wcscat(msg, tmp);
        }
        swprintf(tmp, 256, L"  %d. %s　%d 元　%s\n", n + 1, fname, price, feat);
        if (wcslen(msg) + wcslen(tmp) < 4000) wcscat(msg, tmp);
        n++;
    }
    fclose(fp);
    if (n == 0) {
        popupMsg(L"没有找到该园区的美食\n可输入：荷园/松园/菊园/柳园", L"美食");
        return;
    }
    popupMsg(msg, L"美食推荐");
    /* 挑最爱吃的饭 → 输入出发地 → 自动导航到该园区 */
    buf[0] = 0;
    if (!popupInput(buf, 16, L"输入你最爱吃的饭编号（0=不导航）", L"美食导航") || buf[0] == 0) return;
    sel = _wtoi(buf);
    if (sel < 1 || sel > n) return;
    sel--;
    swprintf(tmp, 256, L"已选：「%s」%d 元（%s）\n是否自动导航？",
             flist[sel].name, flist[sel].price, flist[sel].zone);
    if (!popupYesNo(tmp, L"美食导航")) return;
    {
        int zid, i;
        zid = -1;
        for (i = 1; i <= g->vn; i++)
            if (wcscmp(wname[i], flist[sel].zone) == 0) { zid = i; break; }
        if (zid == -1) { popupMsg(L"该园区不是地图上的地点，无法导航", L"提示"); return; }
        foodNavGo(g, zid);
    }
}

/* 探索园区：列出该园区美食（foods.dat），可挑最爱吃的饭并输入出发地自动导航
 * 返回 0 = 该地点不是美食园区 */
static int foodNav(MapGraph *g, int zoneId) {
    const wchar_t *zone = wname[zoneId];
    wchar_t msg[2048], tmp[256], line[256], buf[64];
    char raw[256];
    FILE *fp;
    int n = 0, sel;
    fp = fopen("foods.dat", "r");
    if (!fp) return 0;
    msg[0] = 0;
    swprintf(msg, 2048, L"【%s】美食推荐：\n", zone);
    while (fgets(raw, sizeof(raw), fp) && n < MAX_FOOD) {
        wchar_t fzone[32], fname[64], feat[128], *p, *q;
        int price;
        raw[strcspn(raw, "\r\n")] = 0;
        if (!raw[0]) continue;
        MultiByteToWideChar(CP_UTF8, 0, raw, -1, line, 256);
        p = line;
        q = wcschr(p, L'|');
        if (!q) continue;
        *q = 0;
        wcscpy(fzone, p);
        if (wcscmp(fzone, zone) != 0) continue;   /* 只要该园区的 */
        p = q + 1;
        q = wcschr(p, L'|');
        if (!q) continue;
        *q = 0;
        wcscpy(fname, p);
        p = q + 1;
        price = _wtoi(p);
        q = wcschr(p, L'|');
        if (!q) continue;
        wcscpy(feat, q + 1);
        wcscpy(flist[n].zone, fzone);
        wcscpy(flist[n].name, fname);
        flist[n].price = price;
        wcscpy(flist[n].feat, feat);
        swprintf(tmp, 256, L"  %d. %s　%d 元　%s\n", n + 1, fname, price, feat);
        if (wcslen(msg) + wcslen(tmp) < 2000) wcscat(msg, tmp);
        n++;
    }
    fclose(fp);
    if (n == 0) return 0;   /* 不是美食园区 */
    if (!popupYesNo(L"是否要挑选美食？", L"美食")) return 1;   /* 不挑就直接结束 */
    popupMsg(msg, L"美食推荐");
    /* 挑最爱吃的饭 → 输入出发地 → 自动导航到该园区 */
    buf[0] = 0;
    if (!popupInput(buf, 16, L"输入你最爱吃的饭编号（0=不导航）", L"美食导航") || buf[0] == 0) return 1;
    sel = _wtoi(buf);
    if (sel < 1 || sel > n) return 1;
    sel--;
    swprintf(tmp, 256, L"已选：「%s」%d 元（%s）\n是否自动导航？",
             flist[sel].name, flist[sel].price, flist[sel].zone);
    if (!popupYesNo(tmp, L"美食导航")) return 1;
    foodNavGo(g, zoneId);
    return 1;
}

/* 搜索美食：关键词匹配 foods.dat 美食名/特色，选一条后输入出发地导航到对应园区
 * 返回 1 = 有匹配美食（已处理），0 = 无匹配 */
static int foodSearchNav(MapGraph *g, const wchar_t *key) {
    FILE *fp = fopen("foods.dat", "r");
    wchar_t msg[2048], tmp[256], line[256], buf[16];
    char raw[256];
    int n = 0, sel;
    if (!fp) return 0;
    msg[0] = 0;
    wcscat(msg, L"找到相关美食：\n");
    while (fgets(raw, sizeof(raw), fp) && n < MAX_FOOD) {
        wchar_t fzone[32], fname[64], feat[128], *p, *q;
        int price;
        raw[strcspn(raw, "\r\n")] = 0;
        if (!raw[0]) continue;
        MultiByteToWideChar(CP_UTF8, 0, raw, -1, line, 256);
        p = line;
        q = wcschr(p, L'|');
        if (!q) continue;
        *q = 0;
        wcscpy(fzone, p);
        p = q + 1;
        q = wcschr(p, L'|');
        if (!q) continue;
        *q = 0;
        wcscpy(fname, p);
        p = q + 1;
        price = _wtoi(p);
        q = wcschr(p, L'|');
        if (!q) continue;
        wcscpy(feat, q + 1);
        if (!wcsstr(fname, key) && !wcsstr(feat, key)) continue;   /* 美食名或特色匹配 */
        wcscpy(flist[n].zone, fzone);
        wcscpy(flist[n].name, fname);
        flist[n].price = price;
        wcscpy(flist[n].feat, feat);
        swprintf(tmp, 256, L"  %d. %s　%d 元（%s）\n", n + 1, fname, price, fzone);
        if (wcslen(msg) + wcslen(tmp) < 2000) wcscat(msg, tmp);
        n++;
    }
    fclose(fp);
    if (n == 0) return 0;
    if (n > 1) {
        popupMsg(msg, L"搜索结果");
        buf[0] = 0;
        if (!popupInput(buf, 16, L"请输入要选择的编号", L"搜索结果") || buf[0] == 0) return 1;
        sel = _wtoi(buf);
        if (sel < 1 || sel > n) { popupMsg(L"编号无效", L"提示"); return 1; }
        sel--;
    } else {
        sel = 0;
    }
    swprintf(tmp, 256, L"已选：「%s」%d 元（%s）\n是否导航到该园区？",
             flist[sel].name, flist[sel].price, flist[sel].zone);
    if (!popupYesNo(tmp, L"美食导航")) return 1;
    {   /* 园区名 -> 地图地点，交给公共导航段 */
        int zid, i;
        zid = -1;
        for (i = 1; i <= g->vn; i++)
            if (wcscmp(wname[i], flist[sel].zone) == 0) { zid = i; break; }
        if (zid == -1) { popupMsg(L"该园区不是地图上的地点，无法导航", L"提示"); return 1; }
        foodNavGo(g, zid);
    }
    return 1;
}

/* 收藏地点：登录后按学号收藏，追加到 favorites.dat（学号|地点名）*/
void guiFavAdd(MapGraph *g) {
    wchar_t name[64], msg[256], wl[64];
    char buf[256], line[128];
    FILE *fp;
    int id, dup = 0;
    if (!g_loggedIn) { popupMsg(L"请先登录（点右上角「登录」按钮）", L"提示"); return; }
    name[0] = 0;
    if (!popupInput(name, 64, L"请输入要收藏的地点名称", L"收藏地点") || name[0] == 0) return;
    id = findWName(g, name);
    if (id == -1) { popupMsg(L"地点不存在，请检查名称", L"提示"); return; }
    /* 查重：只查当前学号 */
    fp = fopen("favorites.dat", "r");
    if (fp) {
        while (fgets(line, sizeof(line), fp)) {
            wchar_t *sep;
            line[strcspn(line, "\r\n")] = 0;
            MultiByteToWideChar(CP_UTF8, 0, line, -1, wl, 64);
            sep = wcschr(wl, L'|');
            if (sep) {
                *sep = 0;
                if (wcscmp(wl, g_stuId) == 0 && wcscmp(sep + 1, wname[id]) == 0) dup = 1;
            }
        }
        fclose(fp);
    }
    if (dup) { popupMsg(L"已经收藏过该地点了", L"提示"); return; }
    fp = fopen("favorites.dat", "a");
    if (!fp) { popupMsg(L"无法写入 favorites.dat", L"提示"); return; }
    swprintf(msg, 256, L"%s|%s", g_stuId, wname[id]);
    WideCharToMultiByte(CP_UTF8, 0, msg, -1, buf, sizeof(buf), NULL, NULL);
    fprintf(fp, "%s\n", buf);
    fclose(fp);
    swprintf(msg, 256, L"已收藏：%s", wname[id]);
    popupMsg(msg, L"收藏成功");
}

/* 查看收藏列表（只显示当前学号的）*/
void guiFavList(void) {
    wchar_t msg[1024], wl[64];
    char line[128];
    FILE *fp;
    int count = 0;
    if (!g_loggedIn) { popupMsg(L"请先登录（点右上角「登录」按钮）", L"提示"); return; }
    fp = fopen("favorites.dat", "r");
    if (!fp) { popupMsg(L"还没有收藏任何地点", L"收藏列表"); return; }
    msg[0] = 0;
    wcscat(msg, L"我的收藏：\n");
    while (fgets(line, sizeof(line), fp)) {
        wchar_t *sep;
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0]) continue;
        MultiByteToWideChar(CP_UTF8, 0, line, -1, wl, 64);
        sep = wcschr(wl, L'|');
        if (!sep) continue;                      /* 旧格式无学号，跳过 */
        *sep = 0;
        if (wcscmp(wl, g_stuId) != 0) continue;  /* 只看自己的收藏 */
        wcscat(msg, L"  ");
        wcscat(msg, sep + 1);
        wcscat(msg, L"\n");
        count++;
    }
    fclose(fp);
    if (count == 0) wcscpy(msg, L"还没有收藏任何地点");
    popupMsg(msg, L"收藏列表");
}

/* 取消收藏：只删除当前学号的那一行，其余行重写回文件 */
void guiFavDel(void) {
    wchar_t name[64], msg[256];
    char buf[64], line[128];
    FILE *fp, *fpw;
    int found = 0;
    if (!g_loggedIn) { popupMsg(L"请先登录（点右上角「登录」按钮）", L"提示"); return; }
    name[0] = 0;
    if (!popupInput(name, 64, L"请输入要取消收藏的地点名称", L"取消收藏") || name[0] == 0) return;
    WideCharToMultiByte(CP_UTF8, 0, name, -1, buf, 64, NULL, NULL);
    fp = fopen("favorites.dat", "r");
    if (fp) {
        fpw = fopen("favorites.tmp", "w");
        while (fgets(line, sizeof(line), fp)) {
            wchar_t wl[64], *sep;
            line[strcspn(line, "\r\n")] = 0;
            if (!line[0]) continue;
            MultiByteToWideChar(CP_UTF8, 0, line, -1, wl, 64);
            sep = wcschr(wl, L'|');
            if (!sep) { fprintf(fpw, "%s\n", line); continue; }   /* 旧格式行保留 */
            *sep = 0;
            if (wcscmp(wl, g_stuId) == 0 && wcscmp(sep + 1, name) == 0) { found = 1; continue; }
            fprintf(fpw, "%s\n", line);
        }
        fclose(fp);
        if (fpw) fclose(fpw);
        if (found) {
            remove("favorites.dat");
            rename("favorites.tmp", "favorites.dat");
        } else remove("favorites.tmp");
    }
    if (found) {
        swprintf(msg, 256, L"已取消收藏：%s", name);
        popupMsg(msg, L"提示");
    } else popupMsg(L"收藏列表中没有该地点", L"提示");
}

/* 出行方式弹窗：返回 0=取消，1..4=方式编号（取消时调用方默认步行）*/
int askMode(int *mode) {
    wchar_t buf[16];
    buf[0] = 0;
    if (!popupInput(buf, 16,
                    L"请输入出行方式编号：1.骑自行车 2.驾车 3.电动车 4.步行",
                    L"出行方式")) return 0;
    if (buf[0] == 0) return 0;
    *mode = _wtoi(buf);
    return *mode >= 1 && *mode <= 4;
}

/* 弹窗显示最快路线结果：路线 + 距离 + 方式 + 预计用时 + 到达时刻 */
void showRouteResult(MapGraph *g, int *path, int len, double totalDist,
                     double totalTime, int mode) {
    wchar_t msg[512], buf[128];
    SYSTEMTIME st;
    int arriveMinute, total;
    msg[0] = 0;
    wcscat(msg, L"最快路线：");
    wcscat(msg, wname[path[0]]);
    wcscat(msg, L" → ");
    wcscat(msg, wname[path[len - 1]]);
    swprintf(buf, 128, L"\n总距离：%.0f 米\n出行方式：%s", totalDist, modeName[mode]);
    wcscat(msg, buf);
    total = (int)(totalTime / 60.0 + 0.5);   /* 秒 -> 分钟 */
    if (total < 1) total = 1;
    GetLocalTime(&st);
    arriveMinute = st.wHour * 60 + st.wMinute + total;
    if (total >= 60)
        swprintf(buf, 128, L"\n预计用时：约 %d 小时 %d 分钟\n预计到达：%02d:%02d",
                 total / 60, total % 60, (arriveMinute / 60) % 24, arriveMinute % 60);
    else
        swprintf(buf, 128, L"\n预计用时：约 %d 分钟\n预计到达：%02d:%02d",
                 total, (arriveMinute / 60) % 24, arriveMinute % 60);
    wcscat(msg, buf);
    popupMsg(msg, L"最快路线");
}

/* 地铁口导航校验：出发地/目的地只要涉及地铁口，就必须是同一段地铁线路的两个端点；
 * 不满足时弹窗说明原因并返回 0（调用方应放弃这次选择）*/
/* 导航配对校验：两个地铁口必须在地铁线路上才能互达；
 * 校内地点与地铁口之间可经校门互达（步行边），不再拦截 */
static int checkMetroPair(MapGraph *g, int a, int b) {
    if (isMetroPlace(g, a) && isMetroPlace(g, b) && !metroPair(g, a, b)) {
        popupMsg(L"这两个地铁口之间没有直达地铁线路", L"提示");
        return 0;
    }
    return 1;
}

/* 以当前出发地/目的地计算最快路线：先选出行方式，再算并弹窗显示结果 */
static void planFromState(MapGraph *g) {
    int mode = 4;                      /* 默认步行 */
    wchar_t msg[512];
    if (metroPair(g, s_start, s_end)) {
        mode = 5;                      /* 地铁口之间只能坐地铁，不用选方式 */
    } else {
        askMode(&mode);                /* 先选出行方式 */
    }
    computeFastestRoute(g, s_start, s_end, mode, s_path, &s_len, &s_totalDist, &s_totalTime);
    if (s_len == 0) {
        swprintf(msg, 512, L"%s 到 %s 之间没有通路", wname[s_start], wname[s_end]);
        popupMsg(msg, L"提示");
        s_end = -1;  s_len = 0;
    } else {
        showRouteResult(g, s_path, s_len, s_totalDist, s_totalTime, mode);
    }
}

/* ========== 图书馆：登录注册 + 图书管理系统 ==========
 * 入口：探索图书馆时选择进入；未登录先注册/登录（学号+密码，存 users.dat），
 * 登录后可查询/借书/还书/查看借阅/按分类浏览图书（books.dat，借阅记录 borrows.dat）*/
#define MAX_BOOK 200

typedef struct {
    int id;
    wchar_t name[80];
    wchar_t author[40];
    wchar_t pub[40];
    int total, avail;
    wchar_t loc[40];
    wchar_t cat[20];
} Book;

static Book books[MAX_BOOK];
static int bookN;

/* 读 books.dat 到内存（书号|书名|作者|出版社|总库存|可借数|馆藏位置|分类）*/
static void loadBooks(void) {
    FILE *fp = fopen("books.dat", "r");
    char line[256];
    bookN = 0;
    if (!fp) return;
    while (fgets(line, sizeof(line), fp) && bookN < MAX_BOOK) {
        int id, total, avail;
        char name[80], author[40], pub[40], loc[40], cat[20];
        if (sscanf(line, "%d|%79[^|]|%39[^|]|%39[^|]|%d|%d|%39[^|]|%19[^\r\n]",
                   &id, name, author, pub, &total, &avail, loc, cat) == 8) {
            Book *b = &books[bookN];
            b->id = id;
            MultiByteToWideChar(CP_UTF8, 0, name, -1, b->name, 80);
            MultiByteToWideChar(CP_UTF8, 0, author, -1, b->author, 40);
            MultiByteToWideChar(CP_UTF8, 0, pub, -1, b->pub, 40);
            MultiByteToWideChar(CP_UTF8, 0, loc, -1, b->loc, 40);
            MultiByteToWideChar(CP_UTF8, 0, cat, -1, b->cat, 20);
            b->total = total;
            b->avail = avail;
            bookN++;
        }
    }
    fclose(fp);
}

/* 内存图书表写回 books.dat（临时文件重写，防止写坏原文件）*/
static void saveBooks(void) {
    FILE *fp = fopen("books.tmp", "w");
    int i;
    if (!fp) return;
    for (i = 0; i < bookN; i++) {
        char name[80], author[40], pub[40], loc[40], cat[20];
        Book *b = &books[i];
        WideCharToMultiByte(CP_UTF8, 0, b->name, -1, name, 80, NULL, NULL);
        WideCharToMultiByte(CP_UTF8, 0, b->author, -1, author, 40, NULL, NULL);
        WideCharToMultiByte(CP_UTF8, 0, b->pub, -1, pub, 40, NULL, NULL);
        WideCharToMultiByte(CP_UTF8, 0, b->loc, -1, loc, 40, NULL, NULL);
        WideCharToMultiByte(CP_UTF8, 0, b->cat, -1, cat, 20, NULL, NULL);
        fprintf(fp, "%d|%s|%s|%s|%d|%d|%s|%s\n",
                b->id, name, author, pub, b->total, b->avail, loc, cat);
    }
    fclose(fp);
    remove("books.dat");
    rename("books.tmp", "books.dat");
}

/* 读一行 UTF-8 文本转宽字符（去掉换行），返回 0=读完 */
static int readLineW(FILE *fp, wchar_t *out, int max) {
    char line[256];
    if (!fgets(line, sizeof(line), fp)) return 0;
    line[strcspn(line, "\r\n")] = 0;
    MultiByteToWideChar(CP_UTF8, 0, line, -1, out, max);
    return 1;
}

/* 注册：学号+密码（输两遍），查重后写入 users.dat，成功后自动登录 */
static void libRegister(void) {
    wchar_t id[32], pw[32], pw2[32], msg[256];
    FILE *fp;
    int i, dup = 0;
    id[0] = pw[0] = pw2[0] = 0;
    if (!popupInput(id, 32, L"请输入学号（4~12 位数字）", L"注册") || id[0] == 0) return;
    for (i = 0; id[i]; i++)
        if (!wcschr(L"0123456789", id[i])) { popupMsg(L"学号只能由数字组成", L"注册"); return; }
    if (wcslen(id) < 4 || wcslen(id) > 12) { popupMsg(L"学号应为 4~12 位数字", L"注册"); return; }
    if (!popupInput(pw, 32, L"请设置密码（至少 4 位）", L"注册") || pw[0] == 0) return;
    if (wcslen(pw) < 4) { popupMsg(L"密码至少 4 位", L"注册"); return; }
    if (!popupInput(pw2, 32, L"请再次输入密码", L"注册")) return;
    if (wcscmp(pw, pw2) != 0) { popupMsg(L"两次输入的密码不一致", L"注册"); return; }
    /* 查重 */
    fp = fopen("users.dat", "r");
    if (fp) {
        wchar_t line[64], *sep;
        while (readLineW(fp, line, 64)) {
            sep = wcschr(line, L'|');
            if (sep) {
                *sep = 0;
                if (wcscmp(line, id) == 0) { dup = 1; break; }
            }
        }
        fclose(fp);
    }
    if (dup) { popupMsg(L"该学号已注册，请直接登录", L"注册"); return; }
    fp = fopen("users.dat", "a");
    if (!fp) { popupMsg(L"无法写入 users.dat", L"注册"); return; }
    {
        char line[128];
        swprintf(msg, 256, L"%s|%s", id, pw);
        WideCharToMultiByte(CP_UTF8, 0, msg, -1, line, 128, NULL, NULL);
        fprintf(fp, "%s\n", line);
    }
    fclose(fp);
    wcscpy(g_stuId, id);
    g_loggedIn = 1;
    swprintf(msg, 256, L"注册成功！欢迎 %s", id);
    popupMsg(msg, L"注册成功");
}

/* 登录：比对 users.dat，成功返回 1 */
static int libLoginDo(void) {
    wchar_t id[32], pw[32], line[64];
    FILE *fp;
    int ok = 0;
    id[0] = pw[0] = 0;
    if (!popupInput(id, 32, L"请输入学号", L"登录") || id[0] == 0) return 0;
    if (!popupInput(pw, 32, L"请输入密码", L"登录") || pw[0] == 0) return 0;
    fp = fopen("users.dat", "r");
    if (!fp) { popupMsg(L"还没有账号，请先注册", L"登录"); return 0; }
    while (readLineW(fp, line, 64)) {
        wchar_t *sep = wcschr(line, L'|');
        if (sep) {
            *sep = 0;
            if (wcscmp(line, id) == 0 && wcscmp(sep + 1, pw) == 0) { ok = 1; break; }
        }
    }
    fclose(fp);
    if (!ok) { popupMsg(L"学号或密码错误", L"登录"); return 0; }
    wcscpy(g_stuId, id);
    g_loggedIn = 1;
    return 1;
}

/* 登录/注册菜单（未登录进入图书馆时调用）*/
static void libLogin(void) {
    wchar_t buf[16];
    buf[0] = 0;
    if (!popupInput(buf, 16, L"进入图书馆需要登录\n1.登录  2.注册", L"图书馆") || buf[0] == 0) return;
    if (_wtoi(buf) == 1) libLoginDo();
    else if (_wtoi(buf) == 2) libRegister();
    else popupMsg(L"编号无效", L"图书馆");
}

/* ========== 主界面账号：右上角登录按钮（不强制登录），登录后可查搜索历史 ========== */
#define MAX_SEARCH 200

/* 记录搜索历史：登录用户每次搜索关键词追加到 search_history.dat（学号|关键词|时间）*/
static void searchHistRecord(const wchar_t *key) {
    FILE *fp;
    char sid8[64], key8[128];
    SYSTEMTIME st;
    if (!g_loggedIn) return;
    fp = fopen("search_history.dat", "a");
    if (!fp) return;
    WideCharToMultiByte(CP_UTF8, 0, g_stuId, -1, sid8, sizeof(sid8), NULL, NULL);
    WideCharToMultiByte(CP_UTF8, 0, key, -1, key8, sizeof(key8), NULL, NULL);
    GetLocalTime(&st);
    fprintf(fp, "%s|%s|%04d-%02d-%02d %02d:%02d\n", sid8, key8,
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    fclose(fp);
}

/* 查询搜索历史：显示当前登录学号最近 20 条记录（新→旧）*/
static void searchHistList(void) {
    FILE *fp;
    wchar_t line[256], *p, *q;
    wchar_t msg[2048], tmp[160];
    static wchar_t hist[MAX_SEARCH][96];
    int n = 0, i, show;
    fp = fopen("search_history.dat", "r");
    if (fp) {
        while (readLineW(fp, line, 256) && n < MAX_SEARCH) {
            p = line;
            q = wcschr(p, L'|');
            if (!q) continue;
            *q = 0;
            if (wcscmp(p, g_stuId) != 0) continue;   /* 只看自己的记录 */
            p = q + 1;
            q = wcschr(p, L'|');
            if (!q) continue;
            *q = 0;
            swprintf(hist[n], 96, L"%s　%s", p, q + 1);   /* 关键词　时间 */
            n++;
        }
        fclose(fp);
    }
    if (n == 0) { popupMsg(L"暂无搜索记录", L"搜索历史"); return; }
    show = (n > 20) ? 20 : n;
    msg[0] = 0;
    wcscat(msg, L"最近搜索记录（新→旧）：\n");
    for (i = 0; i < show; i++) {
        swprintf(tmp, 160, L"  %s\n", hist[n - 1 - i]);
        wcscat(msg, tmp);
    }
    popupMsg(msg, L"搜索历史");
}

/* 记录导航地点：登录用户把某地点设为出发地/目的地时记入 nav_history.dat（学号|地点id|时间）*/
static void navHistRecord(int id) {
    FILE *fp;
    char sid8[64];
    SYSTEMTIME st;
    if (!g_loggedIn) return;
    fp = fopen("nav_history.dat", "a");
    if (!fp) return;
    WideCharToMultiByte(CP_UTF8, 0, g_stuId, -1, sid8, sizeof(sid8), NULL, NULL);
    GetLocalTime(&st);
    fprintf(fp, "%s|%d|%04d-%02d-%02d %02d:%02d\n", sid8, id,
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    fclose(fp);
}

/* 历史导航地点：列出当前学号最近导航过的地点（去重，新→旧），选编号返回地点 id */
static int navHistPick(void) {
    FILE *fp;
    static int ids[MAX_SEARCH];
    static wchar_t names[MAX_SEARCH][50];
    wchar_t msg[2048], tmp[128], line[96], buf[16];
    int n = 0, i, k, sel;
    fp = fopen("nav_history.dat", "r");
    if (fp) {
        while (readLineW(fp, line, 96) && n < MAX_SEARCH) {
            wchar_t *p = line, *q;
            int id;
            q = wcschr(p, L'|');
            if (!q) continue;
            *q = 0;
            if (wcscmp(p, g_stuId) != 0) continue;   /* 只看自己的 */
            p = q + 1;
            id = _wtoi(p);
            if (id < 1 || id >= MAX_N || wname[id][0] == 0) continue;   /* 非法/已下线地点 */
            for (k = 0; k < n; k++)
                if (ids[k] == id) break;
            if (k < n) continue;   /* 重复地点保留最早出现（最新）的那次 */
            ids[n] = id;
            wcscpy(names[n], wname[id]);
            n++;
        }
        fclose(fp);
    }
    if (n == 0) { popupMsg(L"暂无历史导航地点", L"历史导航"); return -1; }
    msg[0] = 0;
    wcscat(msg, L"历史导航地点（新→旧）：\n");
    for (i = 0; i < n; i++) {
        swprintf(tmp, 128, L"%d. %s\n", i + 1, names[i]);
        wcscat(msg, tmp);
    }
    popupMsg(msg, L"历史导航");
    buf[0] = 0;
    if (!popupInput(buf, 16, L"请输入要选择的地点编号", L"历史导航") || buf[0] == 0) return -1;
    sel = _wtoi(buf);
    if (sel < 1 || sel > n) { popupMsg(L"编号无效", L"提示"); return -1; }
    return ids[sel - 1];
}

/* 右上角账号按钮：未登录→登录/注册；已登录→查询搜索历史/退出登录 */
static void logMenu(void) {
    wchar_t buf[16];
    buf[0] = 0;
    if (!g_loggedIn) {
        if (!popupInput(buf, 16, L"1.登录  2.注册", L"账号") || buf[0] == 0) return;
        if (_wtoi(buf) == 1) {
            if (libLoginDo()) popupMsg(L"登录成功", L"提示");
        } else if (_wtoi(buf) == 2) libRegister();
        else popupMsg(L"编号无效", L"账号");
    } else {
        if (!popupInput(buf, 16, L"1.查询搜索历史  2.退出登录", L"我的账号") || buf[0] == 0) return;
        if (_wtoi(buf) == 1) searchHistList();
        else if (_wtoi(buf) == 2) {
            g_loggedIn = 0;
            g_stuId[0] = 0;
            popupMsg(L"已退出登录", L"提示");
        } else popupMsg(L"编号无效", L"我的账号");
    }
}

/* 当前学号是否借了某本书（未还）*/
static int hasBorrowed(int bid) {
    FILE *fp = fopen("borrows.dat", "r");
    wchar_t line[64];
    int hit = 0;
    if (!fp) return 0;
    while (readLineW(fp, line, 64)) {
        wchar_t *sep = wcschr(line, L'|');
        if (sep) {
            *sep = 0;
            if (wcscmp(line, g_stuId) == 0 && _wtoi(sep + 1) == bid) { hit = 1; break; }
        }
    }
    fclose(fp);
    return hit;
}

/* 查询图书：按书名/作者关键词模糊匹配，输入空白提示查询为空 */
static void libListBooks(void) {
    wchar_t key[64], msg[2048], tmp[256];
    int i, n = 0;
    key[0] = 0;
    if (!popupInput(key, 64, L"输入书名/作者关键词", L"查询图书")) return;
    {   /* 输入空白（含全空格）视为查询为空 */
        int k2;
        for (k2 = 0; key[k2] == L' '; k2++);
        if (key[k2] == 0) { popupMsg(L"查询为空", L"查询图书"); return; }
    }
    msg[0] = 0;
    wcscat(msg, L"查询结果：\n");
    for (i = 0; i < bookN; i++) {
        if (!wcsstr(books[i].name, key) && !wcsstr(books[i].author, key))
            continue;
        swprintf(tmp, 256, L"%d.《%s》%s（%s）可借 %d/%d\n",
                 books[i].id, books[i].name, books[i].author,
                 books[i].cat, books[i].avail, books[i].total);
        wcscat(msg, tmp);
        n++;
    }
    if (n == 0) wcscat(msg, L"（没有匹配的图书）");
    popupMsg(msg, L"查询结果");
}

/* ========== 借期辅助：应还日期解析与校验（最长 30 天） ========== */
static int daysInMonth2(int y, int m) {
    static const int dm[13] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m < 1 || m > 12) return 0;
    if (m == 2 && (y % 400 == 0 || (y % 4 == 0 && y % 100 != 0))) return 29;
    return dm[m];
}

/* 把 YYYY-MM-DD 转成自 1970-01-01 起的天数（同一进程内差值不受时区影响）*/
static int dateToDay(int y, int m, int d) {
    struct tm t;
    memset(&t, 0, sizeof(t));
    t.tm_year = y - 1900;
    t.tm_mon = m - 1;
    t.tm_mday = d;
    return (int)(mktime(&t) / 86400);
}

/* 解析并校验应还日期：格式合法返回 1，diff = 距今天数（今天=0）*/
static int parseDueDate(const wchar_t *s, int *diff) {
    int y, m, d, n = 0;
    SYSTEMTIME st;
    if (swscanf(s, L"%4d-%d-%d%n", &y, &m, &d, &n) != 3 || n != (int)wcslen(s)) return 0;
    if (m < 1 || m > 12 || d < 1 || d > daysInMonth2(y, m)) return 0;
    GetLocalTime(&st);
    *diff = dateToDay(y, m, d) - dateToDay(st.wYear, st.wMonth, st.wDay);
    return 1;
}

/* 借书：扣库存 + 记借阅记录（借期可自选应还日期，最长 30 天；同一本书未还前不能重复借）*/
static void libBorrow(void) {
    wchar_t buf[16], msg[512], line[160];
    char out[256];
    SYSTEMTIME st;
    int bid, i, diff;
    FILE *fp;
    buf[0] = 0;
    if (!popupInput(buf, 16, L"请输入要借的书号", L"借书") || buf[0] == 0) return;
    bid = _wtoi(buf);
    for (i = 0; i < bookN; i++)
        if (books[i].id == bid) break;
    if (i >= bookN) { popupMsg(L"书号不存在，请先查询图书", L"借书"); return; }
    if (books[i].avail <= 0) { popupMsg(L"该书已全部借出，暂无可借", L"借书"); return; }
    if (hasBorrowed(bid)) { popupMsg(L"你已经借过这本书，归还前不能重复借", L"借书"); return; }
    /* 选择应还日期：YYYY-MM-DD，今天到 30 天内 */
    while (1) {
        buf[0] = 0;
        if (!popupInput(buf, 16, L"请输入应还日期（格式 2026-09-25，最长 30 天）", L"借期")
            || buf[0] == 0) return;
        if (!parseDueDate(buf, &diff)) { popupMsg(L"日期格式不正确，请按 YYYY-MM-DD 输入", L"借期"); continue; }
        if (diff < 0) { popupMsg(L"应还日期不能早于今天", L"借期"); continue; }
        if (diff > 30) { popupMsg(L"借期最长 30 天，请重新选择日期", L"借期"); continue; }
        break;
    }
    books[i].avail--;
    saveBooks();
    GetLocalTime(&st);
    fp = fopen("borrows.dat", "a");
    if (!fp) {
        books[i].avail++;
        saveBooks();
        popupMsg(L"无法写入 borrows.dat", L"借书");
        return;
    }
    swprintf(line, 160, L"%s|%d|%04d-%02d-%02d|%s",
             g_stuId, bid, st.wYear, st.wMonth, st.wDay, buf);
    WideCharToMultiByte(CP_UTF8, 0, line, -1, out, 256, NULL, NULL);
    fprintf(fp, "%s\n", out);
    fclose(fp);
    swprintf(msg, 512, L"借书成功：《%s》\n借出 %04d-%02d-%02d，应还 %s（共 %d 天），请按时归还",
             books[i].name, st.wYear, st.wMonth, st.wDay, buf, diff);
    popupMsg(msg, L"借书成功");
}

/* 还书：加库存 + 删除借阅记录 */
static void libReturn(void) {
    wchar_t buf[16], msg[512];
    int bid, i, found = 0;
    buf[0] = 0;
    if (!popupInput(buf, 16, L"请输入要还的书号", L"还书") || buf[0] == 0) return;
    bid = _wtoi(buf);
    for (i = 0; i < bookN; i++)
        if (books[i].id == bid) break;
    if (i >= bookN) { popupMsg(L"书号不存在", L"还书"); return; }
    if (!hasBorrowed(bid)) { popupMsg(L"你没有借这本书，无需归还", L"还书"); return; }
    /* 删 borrows.dat 中该学号+书号的行（临时文件重写）*/
    {
        FILE *in = fopen("borrows.dat", "r");
        FILE *out = fopen("borrows.tmp", "w");
        if (in && out) {
            char raw[256];
            while (fgets(raw, sizeof(raw), in)) {
                wchar_t line[64], *sep;
                raw[strcspn(raw, "\r\n")] = 0;
                MultiByteToWideChar(CP_UTF8, 0, raw, -1, line, 64);
                sep = wcschr(line, L'|');
                if (sep) {
                    *sep = 0;
                    if (wcscmp(line, g_stuId) == 0 && _wtoi(sep + 1) == bid) {
                        found = 1;
                        continue;   /* 跳过被还的这行 */
                    }
                }
                fprintf(out, "%s\n", raw);
            }
            fclose(in);
            fclose(out);
            if (found) {
                remove("borrows.dat");
                rename("borrows.tmp", "borrows.dat");
            } else {
                remove("borrows.tmp");
            }
        } else {
            if (in) fclose(in);
            if (out) fclose(out);
        }
    }
    if (!found) { popupMsg(L"借阅记录异常，请稍后再试", L"还书"); return; }
    books[i].avail++;
    saveBooks();
    swprintf(msg, 512, L"还书成功：《%s》", books[i].name);
    popupMsg(msg, L"还书成功");
}

/* 我的借阅：列出当前学号的借阅记录（书号转书名）*/
static void libMyBorrows(void) {
    FILE *fp = fopen("borrows.dat", "r");
    wchar_t msg[2048], tmp[256];
    int n = 0;
    msg[0] = 0;
    wcscat(msg, L"我的借阅记录：\n");
    if (fp) {
        wchar_t line[96], *sep;
        while (readLineW(fp, line, 96)) {
            wchar_t *date, *date2;
            int b2, i;
            sep = wcschr(line, L'|');
            if (!sep) continue;
            *sep = 0;
            if (wcscmp(line, g_stuId) != 0) continue;
            b2 = _wtoi(sep + 1);
            date = wcschr(sep + 1, L'|');                 /* 借出日期 */
            date2 = date ? wcschr(date + 1, L'|') : NULL; /* 应还日期 */
            for (i = 0; i < bookN; i++)
                if (books[i].id == b2) break;
            if (i < bookN) {
                if (date2)
                    swprintf(tmp, 256, L"《%s》（书号 %d）借出 %s，应还 %s\n",
                             books[i].name, b2, date + 1, date2 + 1);
                else
                    swprintf(tmp, 256, L"《%s》（书号 %d）借出 %s\n",
                             books[i].name, b2, date ? date + 1 : L"未知");
            } else {
                if (date2)
                    swprintf(tmp, 256, L"书号 %d（书已下架）借出 %s，应还 %s\n",
                             b2, date + 1, date2 + 1);
                else
                    swprintf(tmp, 256, L"书号 %d（书已下架）借出 %s\n",
                             b2, date ? date + 1 : L"未知");
            }
            wcscat(msg, tmp);
            n++;
        }
        fclose(fp);
    }
    if (n == 0) wcscat(msg, L"（暂无借阅记录）");
    popupMsg(msg, L"我的借阅");
}

/* 图书分类浏览：动态收集 books.dat 里的分类，选分类后列出该分类下所有书 */
static void libCategory(void) {
    wchar_t cats[MAX_BOOK][20], msg[2048], tmp[256], buf[16];
    int catN = 0, i, k, sel, n = 0;
    for (i = 0; i < bookN; i++) {
        for (k = 0; k < catN; k++)
            if (wcscmp(cats[k], books[i].cat) == 0) break;
        if (k == catN && catN < MAX_BOOK) wcscpy(cats[catN++], books[i].cat);
    }
    if (catN == 0) { popupMsg(L"暂无图书数据", L"图书分类"); return; }
    msg[0] = 0;
    wcscat(msg, L"图书分类：\n");
    for (k = 0; k < catN; k++) {
        swprintf(tmp, 256, L"%d. %s\n", k + 1, cats[k]);
        wcscat(msg, tmp);
    }
    popupMsg(msg, L"图书分类");
    buf[0] = 0;
    if (!popupInput(buf, 16, L"请输入分类编号", L"图书分类") || buf[0] == 0) return;
    sel = _wtoi(buf);
    if (sel < 1 || sel > catN) { popupMsg(L"编号无效", L"图书分类"); return; }
    msg[0] = 0;
    swprintf(tmp, 256, L"「%s」分类图书：\n", cats[sel - 1]);
    wcscat(msg, tmp);
    for (i = 0; i < bookN; i++) {
        if (wcscmp(books[i].cat, cats[sel - 1]) != 0) continue;
        swprintf(tmp, 256, L"%d.《%s》%s 可借 %d/%d\n",
                 books[i].id, books[i].name, books[i].author,
                 books[i].avail, books[i].total);
        wcscat(msg, tmp);
        n++;
    }
    if (n == 0) wcscat(msg, L"（该分类暂无图书）");
    popupMsg(msg, L"图书分类");
}

/* 图书管理系统主菜单：弹窗循环，0 退出返回地图 */
static void libSystem(void) {
    wchar_t buf[16], msg[256];
    while (1) {
        buf[0] = 0;
        swprintf(msg, 256, L"图书管理系统\n当前用户：%s\n"
                 L"1.查询图书\n2.借书\n3.还书\n4.我的借阅\n5.图书分类\n0.退出",
                 g_stuId);
        if (!popupInput(buf, 16, msg, L"图书管理系统") || buf[0] == 0) return;
        if (_wtoi(buf) == 0) return;
        else if (_wtoi(buf) == 1) libListBooks();
        else if (_wtoi(buf) == 2) libBorrow();
        else if (_wtoi(buf) == 3) libReturn();
        else if (_wtoi(buf) == 4) libMyBorrows();
        else if (_wtoi(buf) == 5) libCategory();
        else popupMsg(L"编号无效", L"图书管理系统");
    }
}

/* 进入图书馆：加载图书数据，未登录先登录/注册，然后进入图书管理系统 */
static void libEnter(void) {
    loadBooks();
    if (bookN == 0) { popupMsg(L"图书数据 books.dat 不存在或为空", L"图书馆"); return; }
    if (!g_loggedIn) libLogin();
    if (!g_loggedIn) return;   /* 未登录/未注册则返回地图 */
    libSystem();
}

/* 探索：显示地点的类型、热度和能直达的地方（用 roads.dat 的直达关系）*/
static void explorePlace(MapGraph *g, int id) {
    wchar_t msg[1024], tmp[128];
    int k, n = 0;
    swprintf(msg, 1024, L"%s\n类型：%s　热度：%d\n可直达：",
             wname[id], wtype[id], g->v[id].info.hot);
    for (k = 0; k < dln[id]; k++) {
        swprintf(tmp, 128, L"\n  %s（%d 米，步行约 %d 分钟）",
                 wname[dl[id][k]], (int)dld[id][k], (int)(dld[id][k] / 75.0 + 0.5));
        wcscat(msg, tmp);
        n++;
    }
    if (n == 0) wcscat(msg, L"\n  （该地点没有直达道路）");
    popupMsg(msg, L"地点探索");
    foodNav(g, id);   /* 探索到荷园/松园/菊园/柳园时显示美食，可选菜自动导航 */
    /* 图书馆入口：探索图书馆时可进入图书管理系统 */
    if (wcsstr(wname[id], L"图书馆")) {
        if (popupYesNo(L"是否进入图书馆？\n（需要登录后使用图书管理系统）", L"图书馆"))
            libEnter();
    }
}

/* ========== F1 路线检测：检查路网质量并弹窗报告 ========== */
void checkNetwork(MapGraph *g) {
    wchar_t msg[2048], tmp[256];
    int i, u, deepCnt = 0, deadEnd = 0, isolated = 0;
    EdgeNode *e;
    msg[0] = 0;
    wcscat(msg, L"【路线检测报告】\n");

    /* 1) 穿楼路段：折线深入建筑内部（周围没有路的非白色像素）*/
    SetWorkingImage(&bg);   /* 读取背景图像素 */
    for (i = 0; i < rsegCount; i++) {
        RoadSeg *s = &rsegs[i];
        int pts[MAX_PL * 2 + 2], np = 0, bad = 0, n = 0, m;
        pts[np++] = nodeX[s->u];  pts[np++] = nodeY[s->u];
        for (m = 0; m < s->n; m++) { pts[np++] = s->x[m];  pts[np++] = s->y[m]; }
        pts[np++] = nodeX[s->v];  pts[np++] = nodeY[s->v];
        for (m = 0; m + 1 < np / 2; m++) {
            int x1 = pts[m * 2], y1 = pts[m * 2 + 1];
            int x2 = pts[(m + 1) * 2], y2 = pts[(m + 1) * 2 + 1];
            int d = max(abs(x2 - x1), abs(y2 - y1)), t;
            for (t = 0; t <= d; t++) {
                int x = x1 + (x2 - x1) * t / max(d, 1);
                int y = y1 + (y2 - y1) * t / max(d, 1);
                if (x < 0 || y < 0 || x >= bg.getwidth() || y >= bg.getheight()) continue;
                COLORREF c = getpixel(x, y);
                int rr = GetRValue(c), gg = GetGValue(c), bb = GetBValue(c);
                n++;
                if (!(rr > 226 && gg > 226 && bb > 216) && !(gg > 110 && gg > rr)) {
                    int sur = 0, dy, dx;
                    for (dy = -3; dy <= 3; dy += 3)
                        for (dx = -3; dx <= 3; dx += 3) {
                            COLORREF c2 = getpixel(x + dx, y + dy);
                            if (GetRValue(c2) > 226) sur++;
                        }
                    if (sur <= 2) bad++;
                }
            }
        }
        if (bad * 20 > n) deepCnt++;
    }
    SetWorkingImage();   /* 恢复默认绘图目标 */
    swprintf(tmp, 256, L"穿楼路段：%d / %d 条\n", deepCnt, rsegCount);
    wcscat(msg, tmp);

    /* 2) 断头路口：只有一条路连接的路口 */
    for (u = g->vn + 1; u <= nodeCount; u++) {
        int deg = 0;
        for (e = g->v[u].first; e; e = e->next) deg++;
        if (deg <= 1) deadEnd++;
    }
    swprintf(tmp, 256, L"断头路口：%d 个\n", deadEnd);
    wcscat(msg, tmp);

    /* 3) 孤立地点：从地点 1 出发 BFS 全图，到不了的地点 */
    {
        int vis[MAX_N] = {0}, queue[MAX_N], head = 0, tail = 0;
        vis[1] = 1;  queue[tail++] = 1;
        while (head < tail) {
            u = queue[head++];
            for (e = g->v[u].first; e; e = e->next)
                if (!vis[e->to]) { vis[e->to] = 1;  queue[tail++] = e->to; }
        }
        for (u = 1; u <= g->vn; u++)
            if (!vis[u]) {
                if (isMetroPlace(g, u)) continue;   /* 地铁口只通地铁线路，不算孤立 */
                isolated++;
                swprintf(tmp, 256, L"  孤立地点：%s\n", wname[u]);
                wcscat(msg, tmp);
            }
        swprintf(tmp, 256, L"孤立地点：%d 个\n", isolated);
        wcscat(msg, tmp);
    }
    if (deepCnt == 0 && deadEnd <= 3 && isolated == 0)
        wcscat(msg, L"\n路网状况良好，可以放心使用！");
    else
        wcscat(msg, L"\n发现的问题可按 F2 反馈，统一解决。");
    popupMsg(msg, L"路线检测");
}

/* ========== F2 问题反馈：保存到 feedback.txt 统一收集 ========== */
void feedbackDialog(void) {
    wchar_t buf[512], line[600];
    char utf8[1200];
    SYSTEMTIME st;
    FILE *fp;
    buf[0] = 0;
    if (!popupInput(buf, 512, L"请输入您的问题或建议（保存到 feedback.txt）", L"问题反馈")
        || buf[0] == 0) return;
    GetLocalTime(&st);
    swprintf(line, 600, L"[%02d-%02d %02d:%02d] %s\r\n",
             st.wMonth, st.wDay, st.wHour, st.wMinute, buf);
    WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, 1200, NULL, NULL);
    fp = fopen("feedback.txt", "a");
    if (fp) {
        fputs(utf8, fp);
        fclose(fp);
        popupMsg(L"反馈已保存到 feedback.txt", L"反馈成功");
    } else {
        popupMsg(L"无法打开 feedback.txt", L"错误");
    }
}

int main(void) {
    MapGraph g;
    MOUSEMSG m;
    int n;

    if (!loadMap(&g, "places.dat", "roads.dat")) return 1;
    initNames(&g);
    loadCoords();

    /* 有 map.jpg 且标定过坐标，才用真实地图做背景（否则节点对不齐）*/
    loadimage(&bg, L"map.jpg");
    if (bg.getwidth() > 0) {
        int imgW = bg.getwidth(), imgH = bg.getheight();
        int scrW = GetSystemMetrics(SM_CXSCREEN), scrH = GetSystemMetrics(SM_CYSCREEN);
        meterPerPx = 5.0 * 407.0 / imgW;              /* 按原始图宽算比例尺 */
        snapRange = (int)(8.0 * imgW / 407.0 + 0.5);  /* 路口合并半径 */
        dispScale = 1.0;
        if (imgH > scrH - 120 || imgW > scrW - 40) {  /* 屏幕放不下时缩小到整图可见（所有地点都显示）*/
            double s = (scrH - 120.0) / imgH;
            if ((scrW - 40.0) / imgW < s) s = (scrW - 40.0) / imgW;
            if (s < 1.0) dispScale = s;
        }
        if (dispScale < 1.0) {   /* 手动双线性缩小，避开 loadimage 缩放加载的尺寸 bug */
            int sw = (int)(imgW * dispScale), sh = (int)(imgH * dispScale);
            DWORD *src = GetImageBuffer(&bg);
            DWORD *backup = (DWORD *)malloc((size_t)imgW * imgH * 4);
            if (backup) {
                memcpy(backup, src, (size_t)imgW * imgH * 4);
                bg.Resize(sw, sh);
                DWORD *dst = GetImageBuffer(&bg);
                for (int y = 0; y < sh; y++) {
                    double fy = (y + 0.5) * imgH / sh - 0.5;
                    if (fy < 0) fy = 0;
                    int y0 = (int)fy, y1 = y0 + 1 < imgH ? y0 + 1 : y0;
                    double dy = fy - y0;
                    for (int x = 0; x < sw; x++) {
                        double fx = (x + 0.5) * imgW / sw - 0.5;
                        if (fx < 0) fx = 0;
                        int x0 = (int)fx, x1 = x0 + 1 < imgW ? x0 + 1 : x0;
                        double dx = fx - x0;
                        DWORD p00 = backup[y0 * imgW + x0], p01 = backup[y0 * imgW + x1];
                        DWORD p10 = backup[y1 * imgW + x0], p11 = backup[y1 * imgW + x1];
                        int b = (int)(((p00 & 0xFF) * (1 - dx) + (p01 & 0xFF) * dx) * (1 - dy)
                                   + ((p10 & 0xFF) * (1 - dx) + (p11 & 0xFF) * dx) * dy);
                        int g = (int)((((p00 >> 8) & 0xFF) * (1 - dx) + ((p01 >> 8) & 0xFF) * dx) * (1 - dy)
                                   + (((p10 >> 8) & 0xFF) * (1 - dx) + ((p11 >> 8) & 0xFF) * dx) * dy);
                        int r = (int)((((p00 >> 16) & 0xFF) * (1 - dx) + ((p01 >> 16) & 0xFF) * dx) * (1 - dy)
                                   + (((p10 >> 16) & 0xFF) * (1 - dx) + ((p11 >> 16) & 0xFF) * dx) * dy);
                        dst[y * sw + x] = RGB(r, g, b);
                    }
                }
                free(backup);
            }
        }
    }
    if (coordsFromFile && bg.getwidth() > 0) useImage = 1;
    loadDirectList();                 /* 探索功能的地点直达列表 */
    loadJunctions();                  /* 用户为每个地点点的最近路口 */
    if (useImage) loadRoadPoints();   /* 真实地图模式下读标定拐点 */
    if (useImage) buildRoadGraph(&g); /* 地点-路口路网：路段权重=预估通行秒数 */

    initgraph(1020, 700);   /* 窗口统一 1020×700：地图居中，四周留浅色背景 */
    SetWindowTextW(GetHWnd(), L"校园地图导航系统 - 点击选择出发地");
    s_winW = 1020;  s_winH = 700;
    if (useImage) {
        s_mapW = bg.getwidth();  s_mapH = bg.getheight();   /* 地图实际显示尺寸 */
        s_ox = (s_winW - s_mapW) / 2;                        /* 初始居中显示 */
        s_oy = (s_winH - s_mapH) / 2;
    } else {
        s_mapW = 1000;  s_mapH = 680;   /* 示意图按坐标范围算 */
    }
    BeginBatchDraw();   /* 批量绘制，画面不闪烁 */

    drawMap(&g, s_start, s_end, s_path, s_len, s_totalDist);
    FlushBatchDraw();

    while (1) {
        /* 把积压的鼠标消息全部处理掉（Peek 不阻塞，没消息立刻返回）*/
        while (PeekMouseMsg(&m)) {
            s_mx = m.x;  s_my = m.y;   /* 记录鼠标位置（按钮悬停效果用）*/
            if (m.uMsg == WM_LBUTTONDOWN) {
                /* 按下：先记位置，松开时判断是点击还是拖动 */
                s_dragLastX = m.x;  s_dragLastY = m.y;
                s_dragging = 1;
                s_dragMoved = 0;
            } else if (m.uMsg == WM_MOUSEMOVE && s_dragging && m.mkLButton) {
                /* 按住左键移动 = 拖动地图 */
                int dx = m.x - s_dragLastX, dy = m.y - s_dragLastY;
                if (dx * dx + dy * dy > 9) s_dragMoved = 1;   /* 移动超 3 像素算拖动 */
                s_ox += dx;  s_oy += dy;
                /* 限制范围：地图至少要留一块在窗口里 */
                if (s_ox < -(s_mapW - 80)) s_ox = -(s_mapW - 80);
                if (s_ox > s_winW - 80) s_ox = s_winW - 80;
                if (s_oy < -(s_mapH - 80)) s_oy = -(s_mapH - 80);
                if (s_oy > s_winH - 80) s_oy = s_winH - 80;
                s_dragLastX = m.x;  s_dragLastY = m.y;
            } else if (m.uMsg == WM_LBUTTONUP && s_dragging) {
                s_dragging = 0;
                if (s_dragMoved) continue;   /* 是拖动，不是点击 */
                /* 是点击：先看是否点到顶部的功能按钮 */
                if (m.y >= 40 && m.y <= 68 && m.x >= 10 && m.x <= 354) {
                    int btn = (m.x - 10) / 70;
                    if (btn >= 0 && btn < 5) {
                        if (btn == 0) guiSearch(&g);
                        else if (btn == 1) guiFavAdd(&g);
                        else if (btn == 2) guiFavList();
                        else if (btn == 3) guiFavDel();
                        else guiFood(&g);
                        continue;
                    }
                }
                /* 右上角账号按钮 */
                if (m.y >= 40 && m.y <= 68 && m.x >= s_winW - 74 && m.x <= s_winW - 10) {
                    logMenu();
                    continue;
                }
                /* 地图上的地点（坐标要减去地图偏移量）*/
                n = pickNode(&g, (int)((m.x - s_ox) / dispScale), (int)((m.y - s_oy) / dispScale));
                if (n == -1) continue;              /* 点到空处，忽略 */
                {
                    wchar_t ask[128];
                    if (s_start == -1) {
                        /* 第一次点击：确认是否为出发地 */
                        swprintf(ask, 128, L"确认「%s」是出发地吗？", wname[n]);
                        if (popupYesNo(ask, L"出发地确认")) {
                            s_start = n;  s_end = -1;  s_len = 0;
                            navHistRecord(n);
                        }
                        /* 选「否」= 取消这次点击 */
                    } else if (n == s_start) {
                        popupMsg(L"起点和终点相同，请点击其他地点", L"提示");
                    } else if (!checkMetroPair(&g, s_start, n)) {
                        /* 地铁口规则：非法配对已弹窗说明，这次点击作废 */
                    } else {
                        /* 第二次点击：确认是否为目的地 */
                        swprintf(ask, 128, L"确认「%s」是目的地吗？", wname[n]);
                        if (popupYesNo(ask, L"目的地确认")) {
                            s_end = n;  s_len = 0;
                            navHistRecord(n);
                            planFromState(&g);   /* 选出行方式并出最快路线 */
                        }
                        /* 选「否」= 取消这次点击 */
                    }
                }
            } else if (m.uMsg == WM_RBUTTONDOWN) {
                s_start = -1; s_end = -1; s_len = 0;   /* 右键清空 */
            }
        }
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) break;   /* ESC 退出 */
        if (GetAsyncKeyState(VK_F1) & 0x8000) checkNetwork(&g);    /* F1 路线检测 */
        if (GetAsyncKeyState(VK_F2) & 0x8000) feedbackDialog();    /* F2 问题反馈 */
        drawMap(&g, s_start, s_end, s_path, s_len, s_totalDist);
        FlushBatchDraw();
        Sleep(30);   /* 每 30ms 刷新一次 */
    }

    EndBatchDraw();
    closegraph();
    return 0;
}
