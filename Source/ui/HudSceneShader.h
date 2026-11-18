#pragma once

/**
    HUD 背景的场景着色器（内嵌，不经过皮肤清单）。

    画面语言（参考终末地角色展示的空间背景，但主色调为黑）：
      1. 黑色纵深空间：顶部压暗、地平线一条冷雾接缝、整体暗角。
      2. 地面：Perlin 噪声 → 等值线带，随时间向观察者流动；远处线更粗、被雾吃掉。
      3. 中景：六层透视网格墙 + 少量指向灭点的放射线，构成稀疏的白色 3D 线阵。
      4. 前景：十二片锐利的玻璃三角碎片自下而上漂浮、淡入淡出。
         每片都绕固定倾斜轴做真实的 3D 翻滚（外加面内自转，两者都单向），
         屏幕上的压缩方向随时间转动、明暗随法线变化；没有描边，
         轮廓就是玻璃的硬边。透射画面做两次不同旋转采样 + 倒角带 +
         内层反射 + 斜向反射层 + 后表面侧壁，做出液态玻璃的透与厚度。

    参数映射：pDecay → 雾的厚度；pMod → 地面流动速度；
              pLevel → 线阵 / 等高线 / 碎片的呼吸；pType → 三档密度与频率（不换色）。

    契约见 docs/shader-guide.md：不写 #version；不声明 pixelPos / frontColour /
    pixelAlpha；只输出 gl_FragColor；全篇不使用 in / out / texture( / fragColor。
*/
inline constexpr const char* hudSceneShaderSource = R"GLSL(
uniform vec2  iResolution;
uniform vec2  iOffset;
uniform float iTime;
uniform float pDecay;
uniform float pMod;
uniform float pLevel;
uniform float pType;

const float HORIZON = 0.045;
const float TAU     = 6.28318531;

float hash11 (float n)
{
    return fract (sin (n * 127.1 + 11.7) * 43758.5453123);
}

float hash21 (vec2 v)
{
    return fract (sin (dot (v, vec2 (127.1, 311.7))) * 43758.5453123);
}

vec2 hash22 (vec2 v)
{
    return fract (sin (vec2 (dot (v, vec2 (127.1, 311.7)),
                             dot (v, vec2 (269.5, 183.3)))) * 43758.5453123);
}

// 经典 Perlin 梯度噪声
float perlin (vec2 p)
{
    vec2 i = floor (p);
    vec2 f = fract (p);
    vec2 u = f * f * (3.0 - 2.0 * f);

    vec2 g00 = normalize (hash22 (i + vec2 (0.0, 0.0)) * 2.0 - 1.0 + vec2 (0.0001, 0.0002));
    vec2 g10 = normalize (hash22 (i + vec2 (1.0, 0.0)) * 2.0 - 1.0 + vec2 (0.0001, 0.0002));
    vec2 g01 = normalize (hash22 (i + vec2 (0.0, 1.0)) * 2.0 - 1.0 + vec2 (0.0001, 0.0002));
    vec2 g11 = normalize (hash22 (i + vec2 (1.0, 1.0)) * 2.0 - 1.0 + vec2 (0.0001, 0.0002));

    float v00 = dot (g00, f - vec2 (0.0, 0.0));
    float v10 = dot (g10, f - vec2 (1.0, 0.0));
    float v01 = dot (g01, f - vec2 (0.0, 1.0));
    float v11 = dot (g11, f - vec2 (1.0, 1.0));

    return mix (mix (v00, v10, u.x), mix (v01, v11, u.x), u.y);
}

float fbm (vec2 p)
{
    float v   = 0.0;
    float amp = 0.55;

    for (int i = 0; i < 3; i++)
    {
        v  += amp * perlin (p);
        p   = p * 2.07 + vec2 (13.7, 7.3);
        amp *= 0.5;
    }

    return v;
}

// 绕原点旋转（玻璃的局部坐标与折射采样都用它）
vec2 rot2 (vec2 v, float a)
{
    float c = cos (a);
    float s = sin (a);
    return vec2 (c * v.x - s * v.y, s * v.x + c * v.y);
}

// 绕任意单位轴旋转（Rodrigues）——玻璃碎片真正的 3D 翻滚
vec3 rotAbout (vec3 v, vec3 k, float a)
{
    float c = cos (a);
    float s = sin (a);
    return v * c + cross (k, v) * s + k * (dot (k, v) * (1.0 - c));
}

// 等边三角形 SDF（尖端朝上）
float sdTriangle (vec2 p, float r)
{
    float k = 1.7320508;
    p.x = abs (p.x) - r;
    p.y = p.y + r / k;

    if (p.x + k * p.y > 0.0)
        p = vec2 (p.x - k * p.y, -k * p.x - p.y) * 0.5;

    p.x -= clamp (p.x, -2.0 * r, 0.0);
    return -length (p) * sign (p.y);
}

// 任意三角形 SDF（屏幕空间，负值在内）。顶点直接来自 3D 投影，
// 所以碎片侧对镜头时会自然退化成一条线，而不是被"压扁"。
float sdTri2 (vec2 p, vec2 a, vec2 b, vec2 c)
{
    vec2 e0 = b - a;
    vec2 e1 = c - b;
    vec2 e2 = a - c;

    float l0 = max (length (e0), 1e-5);
    float l1 = max (length (e1), 1e-5);
    float l2 = max (length (e2), 1e-5);

    float s0 = (e0.x * (p.y - a.y) - e0.y * (p.x - a.x)) / l0;
    float s1 = (e1.x * (p.y - b.y) - e1.y * (p.x - b.x)) / l1;
    float s2 = (e2.x * (p.y - c.y) - e2.y * (p.x - c.x)) / l2;

    float sg = (e0.x * e1.y - e0.y * e1.x) >= 0.0 ? 1.0 : -1.0;

    return -min (s0 * sg, min (s1 * sg, s2 * sg));
}

// 一条边沿 d 扫出的平行四边形（棱柱侧壁）的近似有符号距离。
// 必须解 2×2 线性方程组拿到 (u,v)：分别投影只在 e⊥d 时成立，
// 否则每条侧壁会变成一条穿出三角形的无限长窄带（"零件散开"）。
float sdStrip (vec2 p, vec2 a, vec2 b, vec2 d)
{
    vec2  e = b - a;
    vec2  w = p - a;

    float det = e.x * d.y - e.y * d.x;

    if (abs (det) < 1e-9)
        return 1.0;

    float u = (w.x * d.y - w.y * d.x) / det;
    float v = (e.x * w.y - e.y * w.x) / det;

    float du = min (u, 1.0 - u) * length (e);
    float dv = min (v, 1.0 - v) * length (d);

    return -min (du, dv);
}

// 地面等高线：Perlin → 等值线带；返回 0…1 的线强度
float floorContours (vec2 p, float t, float bands, float flow)
{
    float qy = HORIZON - p.y;

    if (qy < 0.003)
        return 0.0;

    float z = 0.62 / qy;

    // 世界坐标：跟着相机向观察者推进，再加一点横向漂移，避免像一张静止的图
    vec2 w = vec2 (p.x * z, z + t * 0.40 * flow);
    w.x += 0.22 * sin (t * 0.13 + z * 0.09);

    float n = fbm (w * 0.42) * 0.5 + 0.5;
    float c = abs (fract (n * bands) - 0.5) * 2.0;   // 0 = 等高线中心

    float thick = 0.05 + 0.28 * smoothstep (0.02, 0.42, qy);
    float line  = 1.0 - smoothstep (0.0, thick, c);

    // 第二层频率更高的细等高线，给地面补细节（只取弱权重，避免抢主线的形）
    float n2 = fbm (w * 0.95 + vec2 (31.7, 17.3)) * 0.5 + 0.5;
    float c2 = abs (fract (n2 * (bands * 2.0)) - 0.5) * 2.0;
    line += (1.0 - smoothstep (0.0, thick * 0.75, c2)) * 0.35;

    float crest = smoothstep (0.30, 0.92, n);
    float fog   = exp (-z * 0.085);
    float nearF = smoothstep (0.004, 0.040, qy);

    return line * (0.35 + 0.85 * crest) * fog * nearF;
}

// 稀疏的白色 3D 线阵：若干层透视网格墙 + 少量指向灭点的放射线
float lattice (vec2 p, float t, float px, float keep)
{
    float acc = 0.0;
    vec2  q   = p - vec2 (0.0, HORIZON);

    for (int i = 0; i < 6; i++)
    {
        float fi = float (i);
        float z  = 1.25 * pow (1.95, fi);
        float s  = 0.62 / z;
        vec2  w  = q / s;

        w.x += 0.35 * fi;   // 每层错开，避免六层线在屏幕上重合

        float dx = abs (fract (w.x + 0.5) - 0.5);
        float dy = abs (fract (w.y + 0.5) - 0.5);

        float vert = step (dx, dy);          // 1 = 竖线更近
        float d    = min (dx, dy) * s;       // 换成屏幕单位

        float keepV = step (keep, hash11 (floor (w.x + 0.5) * 3.7 + fi * 17.0));
        float keepH = step (keep, hash11 (floor (w.y + 0.5) * 5.1 + fi * 23.0));

        float width = px * (1.1 + fi * 0.85);   // 远处稍粗，压住闪烁
        float line  = 1.0 - smoothstep (0.0, width, d);
        line *= mix (keepH, keepV, vert);

        acc += line / (1.0 + fi * 0.75);
    }

    // 沿纵深方向的放射线：从灭点向外，稀疏、只取中段
    float rad = length (q);
    float a01 = (atan (q.y, q.x) / TAU + 0.5) * 24.0;
    float ad  = abs (fract (a01 + 0.5) - 0.5) * (TAU / 24.0) * rad;

    float spoke = 1.0 - smoothstep (0.0, px * 1.5, ad);
    spoke *= step (0.72, hash11 (floor (a01 + 0.5) * 2.3 + 1.0));
    spoke *= smoothstep (0.06, 0.26, rad) * (1.0 - smoothstep (0.55, 0.95, rad));

    acc += spoke * 0.55;

    return acc * smoothstep (HORIZON - 0.008, HORIZON + 0.055, p.y);
}

// 背景空间（地面 + 线阵 + 雾 + 暗角）。玻璃碎片会带着折射偏移再次调用它。
vec3 space (vec2 p, float px, float t, float lvl, float dens, float flow, float keep, float bands)
{
    vec3 col = vec3 (0.004, 0.005, 0.007);

    vec2 q = p - vec2 (0.0, HORIZON);

    // 上方纵深：中间一点冷雾，顶部压暗
    col += vec3 (0.016, 0.019, 0.026) * smoothstep (-0.06, 0.40, q.y)
         * (1.0 - smoothstep (0.16, 0.58, q.y));

    // 地平线雾带
    col += vec3 (0.060, 0.067, 0.083) * exp (-abs (q.y) * 15.0) * (0.55 + 0.45 * dens);

    // 稀疏线阵
    col += vec3 (0.54, 0.58, 0.67) * lattice (p, t, px, keep) * 0.36
         * (0.72 + 0.22 * lvl) * (0.88 + 0.12 * dens);

    // 地面
    if (p.y < HORIZON)
    {
        float qy  = HORIZON - p.y;
        float z   = 0.62 / max (qy, 0.001);
        float fog = exp (-z * 0.075);

        vec3 floorCol = vec3 (0.030, 0.033, 0.041) * (0.45 + 0.55 * smoothstep (0.0, 0.30, qy));

        float mask = smoothstep (HORIZON + 0.002, HORIZON - 0.006, p.y);
        col = mix (col, floorCol, mask);

        float line = floorContours (p, t, bands, flow);
        vec3  lineCol = vec3 (0.72, 0.78, 0.92);

        col += lineCol * line * (0.42 + 0.26 * lvl) * (0.55 + 0.45 * fog);
    }

    // 暗角
    float vig = smoothstep (1.22, 0.34, length (p * vec2 (0.86, 1.04)));
    col *= 0.34 + 0.66 * vig;

    return col;
}

void main()
{
    vec2  res = max (iResolution, vec2 (1.0));
    vec2  uv  = (pixelPos - iOffset) / res;
    float asp = res.x / res.y;
    vec2  p   = (uv - 0.5) * vec2 (asp, -1.0);
    float px  = 1.0 / res.y;

    float t    = iTime;
    float lvl  = pow (clamp (pLevel * 3.0, 0.0, 1.0), 0.7);
    float dens = clamp (pDecay, 0.0, 1.0);
    float flow = 0.70 + 0.70 * clamp (pMod, 0.0, 1.0);

    // pType 只改密度与频率，不换色：板式 6 带 / 房间 4 带 / 弹簧 8 带
    float bands = pType < 0.5 ? 6.0 : (pType < 1.5 ? 4.0 : 8.0);
    float keep  = pType < 0.5 ? 0.60 : (pType < 1.5 ? 0.44 : 0.72);

    vec3 col = space (p, px, t, lvl, dens, flow, keep, bands);

    // ---- 玻璃三角碎片：自下而上漂浮，3D 翻滚，淡入淡出 ------------------
    for (int i = 0; i < 12; i++)
    {
        float fi   = float (i);
        float seed = hash11 (fi * 7.13 + 3.7);

        float speed = 0.040 + 0.030 * seed;
        float life  = fract (t * speed + hash11 (fi * 3.31 + 9.4));

        float cy = mix (-0.62, 0.70, life);
        float cx = (hash11 (fi * 5.71 + 1.3) - 0.5) * asp * 0.92
                 + 0.10 * sin (t * 0.23 + seed * TAU);

        float size = 0.020 + 0.031 * hash11 (fi * 2.17 + 6.6);
        float spin = seed * TAU + t * (0.50 + 0.45 * seed);   // 面内自转（单向）
        float tum  = t * (0.55 + 0.55 * seed) + seed * 4.7;   // 3D 翻滚（单向）

        vec2 c  = vec2 (cx, cy);
        vec2 qq = p - c;

        if (dot (qq, qq) > size * size * 4.0)
            continue;

        // 局部 x / y 轴：先绕 Z 自转，再绕固定倾斜轴翻滚
        float cs = cos (spin);
        float sn = sin (spin);
        vec3  axis = normalize (vec3 (0.86, 0.42, 0.28));

        vec3 u3 = rotAbout (vec3 (cs, sn, 0.0), axis, tum);
        vec3 v3 = rotAbout (vec3 (-sn, cs, 0.0), axis, tum);
        vec3 n3 = cross (u3, v3);

        // 正面三个顶点直接投影到屏幕（正交投影）。不再做逆矩阵压扁，
        // 所以侧对镜头时自然收成一条线，不会被周期性拉长。
        vec2 qu = u3.xy * size;
        vec2 qv = v3.xy * size;

        vec2 f0 = c + qv;
        vec2 f1 = c - qu * 0.8660254 - qv * 0.5;
        vec2 f2 = c + qu * 0.8660254 - qv * 0.5;

        float sdF = sdTri2 (p, f0, f1, f2);

        // 厚度：后表面沿法线退 h，投影到屏幕就是侧壁的宽度
        const float h = 0.10;
        vec2 dth = n3.xy * (-2.0 * h * size);

        // 三条边各扫出一片侧壁，三者并起来就是棱柱的侧面（不是两个三角形叠加）
        float sdS = min (sdStrip (p, f0, f1, dth),
                    min (sdStrip (p, f1, f2, dth),
                         sdStrip (p, f2, f0, dth)));

        float appear = smoothstep (0.0, 0.12, life) * (1.0 - smoothstep (0.70, 1.0, life));

        if (appear < 0.01 || (sdF > 1.8 * size && sdS > 1.8 * size))
            continue;

        float body  = smoothstep (px * 1.1, -px * 1.1, sdF);   // 硬边，1px 抗锯齿
        float strip = smoothstep (px * 1.2, -px * 1.2, sdS);
        strip *= step (px * 0.35, length (dth));               // 厚度不足半像素不画侧壁
        float side  = strip * (1.0 - body);

        // 倒角只用来加强轮廓附近的折射，不再画亮带（不要磨砂边）
        float e    = clamp (-sdF / (0.26 * size), 0.0, 1.0);
        float edge = 1.0 - e;

        // 法线明暗：翻滚时朝向在变，这是"看得出 3D"的关键
        float facing = abs (n3.z);
        float ndl    = clamp (dot (n3, normalize (vec3 (-0.45, 0.75, 0.55))), -1.0, 1.0);
        float light  = 0.45 + 0.55 * max (ndl, 0.0);
        float fres   = pow (1.0 - facing, 3.0);

        // 透射画面：两次不同旋转的采样 + 边缘向外的透镜位移
        float strength = 0.012 + 0.050 * edge * edge;
        vec2  base = normalize (qq + vec2 (0.0004, 0.0007)) * strength
                   + vec2 (cos (spin), sin (spin)) * 0.006;

        vec2 s1 = c + rot2 (p - c - base,         0.30 * (0.5 + edge));
        vec2 s2 = c + rot2 (p - c - base * 1.7,  -0.55 * (0.4 + edge));

        vec3 behind = space (s1, px, t, lvl, dens, flow, keep, bands) * 0.58
                    + space (s2, px, t, lvl, dens, flow, keep, bands) * 0.42;

        vec3 tint = mix (vec3 (0.66, 0.74, 0.90), vec3 (0.86, 0.90, 0.98), seed * 0.6);

        vec2 lq = rot2 (qq, -spin) / max (size, 0.0001);
        float vgrad = smoothstep (-0.6, 0.6, lq.y);
        float band  = exp (-pow ((lq.x * 0.72 + lq.y * 0.62 - 0.10) / 0.34, 2.0));

        // 正面：透射 + 极淡冷白 + 掠射反光；没有亮边 / 磨砂
        vec3 glassCol = behind * 1.30;
        glassCol += tint * (0.030 + 0.045 * vgrad) * (0.6 + 0.4 * light);
        glassCol += tint * fres * 0.12;
        glassCol += tint * band * 0.045 * light;

        // 侧壁：清澈玻璃，比正面略暗（多穿了一段玻璃），不是磨砂亮边
        glassCol = mix (glassCol, behind * 0.72 + tint * 0.02, side * 0.90);

        float faceA = body * (0.40 + 0.10 * fres) * smoothstep (0.02, 0.12, facing);
        float alpha = (faceA + side * 0.50) * appear;

        col = mix (col, glassCol, clamp (alpha, 0.0, 0.90));
    }

    // 静态细噪点：压暗部色带，不随时间跳
    col += (hash21 (pixelPos * 0.7) - 0.5) * 0.006;

    gl_FragColor = vec4 (clamp (col, 0.0, 1.0), 1.0);
}
)GLSL";
