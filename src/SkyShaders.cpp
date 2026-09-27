#include "bactro/SkyShaders.hpp"
#include "bactro/Status.hpp"

#include <pl/ModMenu.hpp>
#include <pl/memory/Hook.hpp>

#include <EGL/egl.h>
#include <android/log.h>
#include <dlfcn.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#define SKY_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "BactroNative", __VA_ARGS__)

using GLenum = unsigned int;
using GLuint = unsigned int;
using GLint = int;
using GLsizei = int;
using GLboolean = unsigned char;
using GLfloat = float;
using GLbitfield = unsigned int;
using GLchar = char;

constexpr GLenum GL_VERTEX_SHADER = 0x8B31;
constexpr GLenum GL_FRAGMENT_SHADER = 0x8B30;
constexpr GLenum GL_COMPILE_STATUS = 0x8B81;
constexpr GLenum GL_LINK_STATUS = 0x8B82;
constexpr GLenum GL_ARRAY_BUFFER = 0x8892;
constexpr GLenum GL_ELEMENT_ARRAY_BUFFER = 0x8893;
constexpr GLenum GL_STATIC_DRAW = 0x88E4;
constexpr GLenum GL_FLOAT = 0x1406;
constexpr GLenum GL_UNSIGNED_SHORT = 0x1403;
constexpr GLenum GL_TRIANGLES = 0x0004;
constexpr GLenum GL_BLEND = 0x0BE2;
constexpr GLenum GL_DEPTH_TEST = 0x0B71;
constexpr GLenum GL_CULL_FACE = 0x0B44;
constexpr GLenum GL_SCISSOR_TEST = 0x0C11;
constexpr GLenum GL_VIEWPORT = 0x0BA2;
constexpr GLenum GL_SRC_ALPHA = 0x0302;
constexpr GLenum GL_ONE_MINUS_SRC_ALPHA = 0x0303;
constexpr GLenum GL_ONE = 1;
constexpr GLenum GL_ZERO = 0;
constexpr GLenum GL_FUNC_ADD = 0x8006;

namespace bactro::skyshaders {
namespace {

constexpr const char* kModuleId = "bactro.skyshaders";

enum class SkyMode : int {
    Midnight = 0,
    Plasma = 1,
    Aurora = 2,
    Water = 3,
    Caustic = 4,
    Thunder = 5,
    Pulsar = 6,
    Count = 7
};

std::atomic_bool g_enabled{false};
std::atomic<int> g_mode{static_cast<int>(SkyMode::Plasma)};
std::atomic<float> g_speed{1.0f};
std::atomic<float> g_intensity{1.0f};
std::atomic<float> g_alpha{0.85f};
std::atomic<float> g_scale{5.0f};
std::atomic<float> g_starDensity{0.985f};
std::atomic<float> g_cloudSpeed{0.5f};
std::atomic<float> g_cloudDensity{0.5f};
std::atomic_bool g_showMoon{true};
std::atomic<float> g_auroraSpeed{0.06f};
std::atomic<float> g_auroraIntensity{1.8f};
std::atomic_bool g_auroraReflect{true};
std::atomic<float> g_thunderInterval{4.0f};
std::atomic<float> g_thunderChance{0.65f};
std::atomic<float> g_thunderGlow{1.0f};
std::atomic<float> g_themeR{0.6f};
std::atomic<float> g_themeG{0.1f};
std::atomic<float> g_themeB{0.4f};
std::atomic_bool g_useTheme{false};

std::mutex g_glMu;
bool g_glReady = false;
GLuint g_vbo = 0, g_ibo = 0;
GLuint g_progs[static_cast<int>(SkyMode::Count)]{};
bool g_progOk[static_cast<int>(SkyMode::Count)]{};
GLint g_aPos = -1;

using EglSwapBuffersFn = EGLBoolean (*)(EGLDisplay, EGLSurface);
EglSwapBuffersFn g_swapOriginal = nullptr;
bool g_swapHooked = false;

using PFN_glCreateShader = GLuint (*)(GLenum);
using PFN_glShaderSource = void (*)(GLuint, GLsizei, const GLchar* const*, const GLint*);
using PFN_glCompileShader = void (*)(GLuint);
using PFN_glGetShaderiv = void (*)(GLuint, GLenum, GLint*);
using PFN_glGetShaderInfoLog = void (*)(GLuint, GLsizei, GLsizei*, GLchar*);
using PFN_glCreateProgram = GLuint (*)(void);
using PFN_glAttachShader = void (*)(GLuint, GLuint);
using PFN_glLinkProgram = void (*)(GLuint);
using PFN_glGetProgramiv = void (*)(GLuint, GLenum, GLint*);
using PFN_glGetAttribLocation = GLint (*)(GLuint, const GLchar*);
using PFN_glGetUniformLocation = GLint (*)(GLuint, const GLchar*);
using PFN_glGenBuffers = void (*)(GLsizei, GLuint*);
using PFN_glBindBuffer = void (*)(GLenum, GLuint);
using PFN_glBufferData = void (*)(GLenum, GLsizei, const void*, GLenum);
using PFN_glUseProgram = void (*)(GLuint);
using PFN_glUniform1f = void (*)(GLint, GLfloat);
using PFN_glUniform2f = void (*)(GLint, GLfloat, GLfloat);
using PFN_glUniform3f = void (*)(GLint, GLfloat, GLfloat, GLfloat);
using PFN_glEnableVertexAttribArray = void (*)(GLuint);
using PFN_glVertexAttribPointer = void (*)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*);
using PFN_glDrawElements = void (*)(GLenum, GLsizei, GLenum, const void*);
using PFN_glDisable = void (*)(GLenum);
using PFN_glEnable = void (*)(GLenum);
using PFN_glGetIntegerv = void (*)(GLenum, GLint*);
using PFN_glDeleteShader = void (*)(GLuint);
using PFN_glBlendFunc = void (*)(GLenum, GLenum);
using PFN_glBlendEquation = void (*)(GLenum);
using PFN_glDepthMask = void (*)(GLboolean);
using PFN_glColorMask = void (*)(GLboolean, GLboolean, GLboolean, GLboolean);

PFN_glCreateShader p_glCreateShader = nullptr;
PFN_glShaderSource p_glShaderSource = nullptr;
PFN_glCompileShader p_glCompileShader = nullptr;
PFN_glGetShaderiv p_glGetShaderiv = nullptr;
PFN_glGetShaderInfoLog p_glGetShaderInfoLog = nullptr;
PFN_glCreateProgram p_glCreateProgram = nullptr;
PFN_glAttachShader p_glAttachShader = nullptr;
PFN_glLinkProgram p_glLinkProgram = nullptr;
PFN_glGetProgramiv p_glGetProgramiv = nullptr;
PFN_glGetAttribLocation p_glGetAttribLocation = nullptr;
PFN_glGetUniformLocation p_glGetUniformLocation = nullptr;
PFN_glGenBuffers p_glGenBuffers = nullptr;
PFN_glBindBuffer p_glBindBuffer = nullptr;
PFN_glBufferData p_glBufferData = nullptr;
PFN_glUseProgram p_glUseProgram = nullptr;
PFN_glUniform1f p_glUniform1f = nullptr;
PFN_glUniform2f p_glUniform2f = nullptr;
PFN_glUniform3f p_glUniform3f = nullptr;
PFN_glEnableVertexAttribArray p_glEnableVertexAttribArray = nullptr;
PFN_glVertexAttribPointer p_glVertexAttribPointer = nullptr;
PFN_glDrawElements p_glDrawElements = nullptr;
PFN_glDisable p_glDisable = nullptr;
PFN_glEnable p_glEnable = nullptr;
PFN_glGetIntegerv p_glGetIntegerv = nullptr;
PFN_glDeleteShader p_glDeleteShader = nullptr;
PFN_glBlendFunc p_glBlendFunc = nullptr;
PFN_glBlendEquation p_glBlendEquation = nullptr;
PFN_glDepthMask p_glDepthMask = nullptr;
PFN_glColorMask p_glColorMask = nullptr;

void logLine(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    bactro::statusLine(buf);
    SKY_LOGI("%s", buf);
}

void* glProc(const char* name) {
    if (void* p = reinterpret_cast<void*>(eglGetProcAddress(name))) return p;
    void* lib = dlopen("libGLESv2.so", RTLD_NOW);
    if (!lib) lib = dlopen("libGLESv3.so", RTLD_NOW);
    return lib ? dlsym(lib, name) : nullptr;
}

#define LOAD(name) p_##name = reinterpret_cast<decltype(p_##name)>(glProc(#name))

bool loadGles() {
    LOAD(glCreateShader);
    LOAD(glShaderSource);
    LOAD(glCompileShader);
    LOAD(glGetShaderiv);
    LOAD(glGetShaderInfoLog);
    LOAD(glCreateProgram);
    LOAD(glAttachShader);
    LOAD(glLinkProgram);
    LOAD(glGetProgramiv);
    LOAD(glGetAttribLocation);
    LOAD(glGetUniformLocation);
    LOAD(glGenBuffers);
    LOAD(glBindBuffer);
    LOAD(glBufferData);
    LOAD(glUseProgram);
    LOAD(glUniform1f);
    LOAD(glUniform2f);
    LOAD(glUniform3f);
    LOAD(glEnableVertexAttribArray);
    LOAD(glVertexAttribPointer);
    LOAD(glDrawElements);
    LOAD(glDisable);
    LOAD(glEnable);
    LOAD(glGetIntegerv);
    LOAD(glDeleteShader);
    LOAD(glBlendFunc);
    LOAD(glBlendEquation);
    LOAD(glDepthMask);
    LOAD(glColorMask);
    return p_glCreateShader && p_glUseProgram && p_glDrawElements && p_glUniform1f;
}

// Shared vertex: fullscreen quad → ray direction from UV + camera
static constexpr const char* kVS = R"(
attribute vec2 aPosition;
varying vec3 vRayDir;
varying vec2 vUV;
uniform float uYaw;
uniform float uPitch;
uniform float uFov;
uniform float uAspect;
void main() {
    gl_Position = vec4(aPosition, 0.9999, 1.0);
    vUV = aPosition * 0.5 + 0.5;
    float tanV = tan(radians(uFov) * 0.5);
    vec3 rayV = normalize(vec3(aPosition.x * tanV * uAspect, aPosition.y * tanV, 1.0));
    float cy = cos(uYaw), sy = sin(uYaw);
    float cp = cos(uPitch), sp = sin(uPitch);
    // rotY * rotX
    mat3 R = mat3(
        cy, 0.0, sy,
        sy*sp, cp, -cy*sp,
        -sy*cp, sp, cy*cp
    );
    vRayDir = R * rayV;
}
)";

// ---- Midnight (Lexora midnight_sky) ----
static constexpr const char* kFS_Midnight = R"(
precision mediump float;
varying vec3 vRayDir;
varying vec2 vUV;
uniform float GameTime;
uniform float StarDensity;
uniform float CloudSpeed;
uniform float CloudDensity;
uniform float ShowMoon;
uniform vec2 iResolution;

float hash11(float p) {
    vec3 p3 = fract(vec3(p) * 0.1031);
    p3 += dot(p3, p3.yzx + 19.19);
    return fract((p3.x + p3.y) * p3.z);
}
float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 19.19);
    return fract((p3.x + p3.y) * p3.z);
}
float smoothNoise13(vec3 x) {
    vec3 p = floor(x), f = smoothstep(0.0, 1.0, fract(x));
    float n = p.x + p.y * 57.0 + 113.0 * p.z;
    return mix(
        mix(mix(hash11(n), hash11(n+1.), f.x), mix(hash11(n+57.), hash11(n+58.), f.x), f.y),
        mix(mix(hash11(n+113.), hash11(n+114.), f.x), mix(hash11(n+170.), hash11(n+171.), f.x), f.y),
        f.z);
}
mat3 fbmMat = mat3(0.0,1.6,1.2, -1.6,0.72,-0.96, -1.2,-0.96,1.28);
float fbm(vec3 p) {
    float f = 0.5*smoothNoise13(p); p = fbmMat*p*1.2;
    f += 0.25*smoothNoise13(p); p = fbmMat*p*1.3;
    f += 0.1666*smoothNoise13(p); p = fbmMat*p*1.4;
    f += 0.0834*smoothNoise13(p);
    return f;
}
float noisyStar(vec2 p, float thr) {
    float v = hash12(p);
    return v >= thr ? pow((v-thr)/(1.-thr), 6.0) : 0.0;
}
float stableStar(vec2 p, float thr) {
    float fx = fract(p.x), fy = fract(p.y);
    vec2 fp = floor(p);
    return noisyStar(fp,thr)*(1.-fx)*(1.-fy)
          +noisyStar(fp+vec2(0,1),thr)*(1.-fx)*fy
          +noisyStar(fp+vec2(1,0),thr)*fx*(1.-fy)
          +noisyStar(fp+vec2(1,1),thr)*fx*fy;
}
void main() {
    vec3 rd = normalize(vRayDir);
    vec3 col = vec3(0.02, 0.04, 0.12) * (abs(rd.y) * 0.5 + 0.5);
    if (ShowMoon > 0.5) {
        vec3 moonDir = normalize(vec3(0.3, 0.6, 0.7));
        float md = max(0.0, dot(rd, moonDir));
        col += vec3(0.7) * pow(md, 350.0);
        col += vec3(0.3,0.35,0.4) * pow(md, 6.0) * 0.25;
    }
    vec2 starUV = rd.xz / max(abs(rd.y), 0.001);
    float angle = 0.0005 * GameTime * 60.0 + atan(starUV.y, starUV.x);
    vec2 samplePos = (0.5*length(starUV)*vec2(cos(angle),sin(angle))+0.5) * iResolution.y;
    col += vec3(stableStar(samplePos, StarDensity));
    float t = GameTime * CloudSpeed * 0.1;
    vec3 fbmIn = vec3(rd.x/(abs(rd.y)+0.1)-t, rd.z/(abs(rd.y)+0.1), 0.0);
    col += vec3(0.5,0.5,0.75) * fbm(fbmIn) * CloudDensity;
    gl_FragColor = vec4(col, 1.0);
}
)";

// ---- Plasma (Lexora plasma_sky) ----
static constexpr const char* kFS_Plasma = R"(
precision mediump float;
varying vec3 vRayDir;
uniform float GameTime;
uniform float PlasmaSpeed;
uniform float PlasmaIntensity;
uniform vec3 ThemeColor;
uniform float UseTheme;
mat3 m3 = mat3(
    0.36,  0.48, -0.80,
   -0.80,  0.60,  0.00,
    0.48,  0.64,  0.60
);
void main() {
    vec3 rd = normalize(vRayDir);
    float t = GameTime * PlasmaSpeed * 0.4;
    vec3 p = rd * 3.0;
    float flow = 0.0;
    float amp = 1.0;
    for (int i = 0; i < 5; i++) {
        p += t * 0.5;
        flow += amp * abs(sin(p.x) * cos(p.y) + sin(p.z));
        p = m3 * p * 1.3;
        amp *= 0.6;
    }
    flow = smoothstep(0.5, 2.5, flow);
    vec3 col;
    if (UseTheme > 0.5) {
        vec3 bg = ThemeColor * 0.05;
        col = mix(bg, ThemeColor, flow * 1.5);
    } else {
        vec3 bg = vec3(0.05, 0.01, 0.1);
        vec3 c1 = vec3(0.6, 0.1, 0.4);
        vec3 c2 = vec3(0.1, 0.7, 0.8);
        col = mix(bg, c1, flow);
        col = mix(col, c2, smoothstep(0.6, 1.0, flow));
    }
    gl_FragColor = vec4(col * PlasmaIntensity, 1.0);
}
)";

// ---- Aurora (Lexora aurora_sky, slightly reduced steps for mobile) ----
static constexpr const char* kFS_Aurora = R"(
precision mediump float;
varying vec3 vRayDir;
uniform float GameTime;
uniform float AuroraSpeed;
uniform float AuroraIntensity;
uniform float AuroraStars;
uniform float AuroraReflect;
uniform vec2 iResolution;
mat2 mm2(float a) { float c=cos(a),s=sin(a); return mat2(c,s,-s,c); }
mat2 m2 = mat2(0.95534,0.29552,-0.29552,0.95534);
float tri(float x) { return clamp(abs(fract(x)-.5),0.01,0.49); }
vec2 tri2(vec2 p) { return vec2(tri(p.x)+tri(p.y), tri(p.y+tri(p.x))); }
float triNoise2d(vec2 p, float spd) {
    float z=1.8, z2=2.5, rz=0.;
    p *= mm2(p.x*0.06);
    vec2 bp = p;
    for (float i=0.; i<5.; i++) {
        vec2 dg = tri2(bp*1.85)*.75;
        dg *= mm2(GameTime*spd);
        p -= dg/z2;
        bp *= 1.3; z2 *= .45; z *= .42;
        p *= 1.21+(rz-1.0)*.02;
        rz += tri(p.x+tri(p.y))*z;
        p *= -m2;
    }
    return clamp(1./pow(rz*29.,1.3),0.,.55);
}
float hash21(vec2 n) { return fract(sin(dot(n,vec2(12.9898,4.1414)))*43758.5453); }
vec4 aurora(vec3 ro, vec3 rd) {
    vec4 col=vec4(0), avgCol=vec4(0);
    for (float i=0.; i<28.; i++) {
        float of = 0.006*hash21(vRayDir.xz*100.)*smoothstep(0.,15.,i);
        float pt = ((.8+pow(i,1.4)*.002)-ro.y)/(rd.y*2.+0.4);
        pt -= of;
        vec3 bpos = ro+pt*rd;
        vec2 p = bpos.zx;
        float rzt = triNoise2d(p, AuroraSpeed);
        vec4 col2 = vec4(0,0,0,rzt);
        col2.rgb = (sin(1.-vec3(2.15,-.5,1.2)+i*0.043)*0.5+0.5)*rzt;
        avgCol = mix(avgCol, col2, .5);
        col += avgCol*exp2(-i*0.065-2.5)*smoothstep(0.,5.,i);
    }
    col *= clamp(rd.y*15.+.4, 0., 1.);
    return col * AuroraIntensity;
}
vec3 nmzHash33(vec3 q) {
    vec3 p = fract(q * vec3(0.1031, 0.1030, 0.0973));
    p += dot(p, p.yxz + 33.33);
    return fract((p.xxy + p.yxx) * p.zyx);
}
vec3 stars(vec3 p) {
    vec3 c = vec3(0.);
    float res = iResolution.x;
    for (float i=0.; i<4.; i++) {
        vec3 q  = fract(p*(.15*res))-.5;
        vec3 id = floor(p*(.15*res));
        vec2 rn = nmzHash33(id).xy;
        float c2 = 1.-smoothstep(0.,.6,length(q));
        c2 *= step(rn.x,.0005+i*i*0.001);
        c += c2*(mix(vec3(1.,0.49,0.1),vec3(0.75,0.9,1.),rn.y)*0.1+0.9);
        p *= 1.3;
    }
    return c*c*.8*AuroraStars;
}
vec3 bg(vec3 rd) {
    float sd = dot(normalize(vec3(-.5,-.6,.9)),rd)*0.5+0.5;
    sd = pow(sd,5.);
    return mix(vec3(0.05,0.1,0.2),vec3(0.1,0.05,0.2),sd)*.63;
}
void main() {
    vec3 rd = normalize(vRayDir);
    vec3 ro = vec3(0,0,-6.7);
    vec3 col = bg(rd);
    vec3 rrd = vec3(rd.x, abs(rd.y), rd.z);
    vec4 aur = smoothstep(0., 1.5, aurora(ro, rrd));
    col += stars(rd);
    if (AuroraReflect > 0.5 || rd.y > 0.0) {
        col = col*(1.-aur.a) + aur.rgb;
    }
    gl_FragColor = vec4(col, 1.0);
}
)";

// ---- Water (Lexora water) ----
static constexpr const char* kFS_Water = R"(
precision mediump float;
varying vec3 vRayDir;
uniform float uTime;
uniform vec3 uColor;
uniform float uAlpha;
uniform float uSpeed;
uniform float uScale;
uniform float uIntensity;
void main() {
    vec3 rayW = normalize(vRayDir);
    vec3 p = rayW * max(0.5, uScale);
    vec3 i = p;
    float c = 1.0;
    float inten = max(0.0005, uIntensity);
    for (int n = 0; n < 5; n++) {
        float t = uTime * uSpeed * (1.0 - (3.5 / float(n + 1)));
        i = p + vec3(
            cos(t - i.x) + sin(t + i.y),
            sin(t - i.y) + cos(t + i.z),
            cos(t - i.z) + sin(t + i.x)
        );
        vec3 sinCos = vec3(sin(i.x + t) / inten, cos(i.y + t) / inten, sin(i.z + t) / inten);
        c += 1.0 / length(p / sinCos);
    }
    c /= 5.0;
    c = 1.17 - pow(abs(c), 1.4);
    vec3 color = vec3(pow(abs(c), 8.0));
    color = clamp(color + uColor * 0.7, 0.0, 1.0);
    float alpha = uAlpha * clamp(c * 0.9 + 0.3, 0.0, 1.0);
    gl_FragColor = vec4(mix(uColor, color, 0.45), alpha);
}
)";

// ---- Caustic (Lexora caustic) ----
static constexpr const char* kFS_Caustic = R"(
precision mediump float;
varying vec3 vRayDir;
uniform float uTime;
uniform vec3 uColor;
uniform float uAlpha;
uniform float uSpeed;
uniform float uScale;
uniform float uIntensity;
void main() {
    vec3 rayW = normalize(vRayDir);
    vec3 p = rayW * uScale;
    vec3 i = p;
    float c = 1.0;
    vec3 p_inten = p * uIntensity;
    for (int n = 0; n < 4; n++) {
        float t = uTime * uSpeed * (1.0 - (3.0 / float(n + 1)));
        i = p + vec3(
            cos(t - i.x) + sin(t + i.y),
            sin(t - i.y) + cos(t + i.z),
            cos(t - i.z) + sin(t + i.x)
        );
        vec3 sinCosVal = vec3(sin(i.x + t), cos(i.y + t), sin(i.z + t));
        c += 1.0 / length(p_inten / sinCosVal);
    }
    c /= 4.0;
    c = 1.5 - sqrt(c);
    float brightness = c * c * c * c;
    vec3 color = uColor * brightness * 1.5 + uColor * 0.2;
    gl_FragColor = vec4(color, uAlpha);
}
)";

// ---- Thunder (Lexora thunder, mobile-friendly) ----
static constexpr const char* kFS_Thunder = R"(
precision mediump float;
varying vec3 vRayDir;
uniform float uTime;
uniform vec3 uColor;
uniform float uAlpha;
uniform float uSpeed;
uniform float uScale;
uniform float uIntensity;
uniform float uThunderInterval;
uniform float uThunderChance;
uniform float uThunderGlow;
float hash(vec3 p) {
    return fract(sin(dot(p, vec3(127.1, 311.7, 74.7))) * 43758.5453123);
}
float hash1(float n) { return fract(sin(n) * 43758.5453123); }
float noise(vec3 p) {
    vec3 i = floor(p);
    vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float n000 = hash(i);
    float n100 = hash(i + vec3(1.0, 0.0, 0.0));
    float n010 = hash(i + vec3(0.0, 1.0, 0.0));
    float n110 = hash(i + vec3(1.0, 1.0, 0.0));
    float n001 = hash(i + vec3(0.0, 0.0, 1.0));
    float n101 = hash(i + vec3(1.0, 0.0, 1.0));
    float n011 = hash(i + vec3(0.0, 1.0, 1.0));
    float n111 = hash(i + vec3(1.0, 1.0, 1.0));
    vec4 a = mix(vec4(n000, n010, n001, n011), vec4(n100, n110, n101, n111), f.x);
    vec2 b = mix(a.xz, a.yw, f.y);
    return mix(b.x, b.y, f.z);
}
float fbm(vec3 p) {
    float v = 0.0, a = 0.5;
    for (int i = 0; i < 4; ++i) {
        v += a * noise(p);
        p = p * 2.0 + vec3(100.0);
        a *= 0.5;
    }
    return v;
}
void main() {
    vec3 rayW = normalize(vRayDir);
    float cloudDensity = 0.0;
    if (rayW.y > -0.15) {
        vec3 p = rayW * uScale;
        p.x += uTime * uSpeed * 0.08;
        p.z += uTime * uSpeed * 0.04;
        cloudDensity = fbm(p) * smoothstep(-0.15, 0.3, rayW.y);
    }
    float interval = max(1.0, uThunderInterval);
    float timeIndex = floor(uTime / interval);
    float timeOffset = fract(uTime / interval);
    float strikeHash = hash1(timeIndex * 12.34);
    bool hasStrike = strikeHash < uThunderChance;
    float flash = 0.0;
    float boltGlow = 0.0;
    if (hasStrike) {
        float strikeAngle = hash1(timeIndex * 45.67) * 6.28318;
        vec3 boltStart = normalize(vec3(cos(strikeAngle), 1.0, sin(strikeAngle)));
        vec3 boltEnd = normalize(vec3(cos(strikeAngle + (hash1(timeIndex * 8.3) - 0.5) * 0.2), -0.2, sin(strikeAngle + (hash1(timeIndex * 8.3) - 0.5) * 0.2)));
        float t = timeOffset;
        if (t < 0.55) {
            float flicker = 0.8 + 0.2 * sin(uTime * 95.0) * cos(uTime * 135.0);
            flash = (exp(-t * 22.0) * 1.5 + exp(-abs(t - 0.15) * 25.0) * 1.0 + exp(-abs(t - 0.35) * 15.0) * 0.4) * flicker;
            float boltIntensity = max(exp(-t * 18.0), exp(-abs(t - 0.15) * 22.0));
            if (boltIntensity > 0.02) {
                vec3 segment = boltEnd - boltStart;
                float segLen = length(segment);
                vec3 segDir = segment / max(segLen, 0.001);
                float h = clamp(dot(rayW - boltStart, segDir), 0.0, segLen);
                vec3 projection = boltStart + segDir * h;
                float noiseSeed = timeIndex * 73.19;
                float jagged = sin(h * 30.0 + noiseSeed) * 0.035 + cos(h * 70.0 - noiseSeed) * 0.015 + sin(h * 150.0) * 0.007;
                vec3 offsetProj = projection + vec3(jagged, 0.0, jagged * 0.6);
                float distToBolt = length(rayW - normalize(offsetProj));
                boltGlow = (exp(-distToBolt * 280.0) * 2.5 + exp(-distToBolt * 30.0) * 0.6) * boltIntensity * flicker;
            }
        }
    }
    vec3 stormBase = mix(vec3(0.005, 0.005, 0.015), uColor * 0.05, 0.5);
    vec3 flashColor = vec3(0.75, 0.82, 1.0) * flash * uThunderGlow;
    vec3 finalSky = stormBase + flashColor * 0.25;
    vec3 cloudBaseColor = mix(vec3(0.01), vec3(0.06, 0.07, 0.12), cloudDensity);
    vec3 cloudLitColor = mix(vec3(0.02, 0.025, 0.05), vec3(0.8, 0.85, 1.0), cloudDensity * flash * uThunderGlow * 0.7);
    vec3 finalCloud = mix(cloudBaseColor, cloudLitColor, min(flash * uThunderGlow, 1.0));
    float cloudMix = smoothstep(0.18, 0.48, cloudDensity);
    vec3 finalColor = mix(finalSky, finalCloud, cloudMix);
    finalColor += vec3(0.85, 0.92, 1.0) * boltGlow * uThunderGlow;
    finalColor *= (1.0 + uIntensity * 10.0);
    gl_FragColor = vec4(finalColor, uAlpha);
}
)";

// ---- Pulsar (Lexora pulsar) ----
static constexpr const char* kFS_Pulsar = R"(
precision mediump float;
varying vec3 vRayDir;
uniform float uTime;
uniform vec3 uColor;
uniform float uAlpha;
uniform float uSpeed;
uniform float uScale;
uniform float uIntensity;
float hash(vec3 p) {
    return fract(sin(dot(p, vec3(127.1, 311.7, 74.7))) * 43758.5453123);
}
float noise(vec3 p) {
    vec3 i = floor(p);
    vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float n000 = hash(i);
    float n100 = hash(i + vec3(1.,0.,0.));
    float n010 = hash(i + vec3(0.,1.,0.));
    float n110 = hash(i + vec3(1.,1.,0.));
    float n001 = hash(i + vec3(0.,0.,1.));
    float n101 = hash(i + vec3(1.,0.,1.));
    float n011 = hash(i + vec3(0.,1.,1.));
    float n111 = hash(i + vec3(1.,1.,1.));
    vec4 a = mix(vec4(n000,n010,n001,n011), vec4(n100,n110,n101,n111), f.x);
    vec2 b = mix(a.xz, a.yw, f.y);
    return mix(b.x, b.y, f.z);
}
float fbm(vec3 p) {
    float v = 0.0, a = 0.5;
    for (int i = 0; i < 4; ++i) {
        v += a * noise(p);
        p = p * 2.0 + vec3(100.0);
        a *= 0.5;
    }
    return v;
}
vec3 starField(vec3 rd) {
    vec3 gridPos = floor(rd * 130.0);
    float starHash = hash(gridPos);
    vec3 starColor = vec3(0.0);
    if (starHash > 0.992) {
        vec3 starOffset = vec3(hash(gridPos+1.1), hash(gridPos+2.2), hash(gridPos+3.3)) - 0.5;
        vec3 starPos = (gridPos + 0.5 + starOffset * 0.85) / 130.0;
        float d = length(rd - normalize(starPos));
        float twinkle = 0.55 + 0.45 * sin(uTime * 3.5 + starHash * 120.0);
        float starIntensity = exp(-d * 600.0) * twinkle * (0.3 + 0.7 * hash(gridPos + 5.5));
        vec3 col = vec3(1.0);
        float colorHash = hash(gridPos + 7.7);
        if (colorHash < 0.22) col = vec3(0.65, 0.8, 1.0);
        else if (colorHash < 0.38) col = vec3(1.0, 0.82, 0.6);
        else if (colorHash < 0.43) col = vec3(1.0, 0.55, 0.55);
        starColor = col * starIntensity;
    }
    return starColor;
}
void main() {
    vec3 rayW = normalize(vRayDir);
    float n1 = fbm(rayW * uScale * 0.4 + vec3(uTime * 0.003, 0.0, 0.0));
    float n2 = fbm(rayW * uScale * 0.9 - vec3(0.0, uTime * 0.002, 0.0));
    vec3 gasColor = mix(vec3(0.001, 0.003, 0.012), vec3(0.035, 0.01, 0.055), n1);
    gasColor = mix(gasColor, vec3(0.008, 0.025, 0.035), n2 * n1);
    float dust = smoothstep(0.3, 0.7, fbm(rayW * uScale * 1.2 + vec3(0.1)));
    vec3 skyBackground = mix(gasColor, vec3(0.0005, 0.0005, 0.0015), dust * 0.85);
    vec3 col = skyBackground + starField(rayW);
    vec3 pulsarCenter = normalize(vec3(0.55, 0.65, -0.45));
    float frontMask = smoothstep(0.0, 0.4, dot(rayW, pulsarCenter));
    if (frontMask > 0.0) {
        vec3 tangent1 = normalize(cross(pulsarCenter, vec3(0.0, 1.0, 0.0)));
        vec3 tangent2 = cross(tangent1, pulsarCenter);
        vec2 p2d = vec2(dot(rayW, tangent1), dot(rayW, tangent2));
        float dCore = length(p2d);
        float pulse = 0.95 + 0.05 * sin(uTime * uSpeed * 8.0);
        float coreSize = 0.058 * pulse;
        float core = smoothstep(coreSize, 0.0, dCore);
        float flare = fbm(rayW * 12.0 + vec3(0.0, uTime * 0.25, 0.0));
        float halo = exp(-dCore * 16.0) * 3.2 + exp(-dCore * 3.5) * 0.9 * (0.7 + 0.3 * flare);
        float wobbleAngle = sin(uTime * uSpeed * 6.0) * 0.22;
        vec2 jetDir2d = vec2(sin(wobbleAngle), cos(wobbleAngle));
        float distAlong = dot(p2d, jetDir2d);
        float wave = sin(abs(distAlong) * 75.0 - uTime * uSpeed * 32.0) * 0.0055 * abs(distAlong);
        float distToJet = length(p2d - distAlong * jetDir2d) - wave;
        float jetIntensity = (exp(-distToJet * 320.0) * 3.0 + exp(-distToJet * 38.0) * 0.8);
        jetIntensity *= exp(-abs(distAlong) * 0.85);
        float jetFlicker = 0.82 + 0.18 * sin(uTime * 115.0) * cos(uTime * 145.0);
        vec3 coreColor = vec3(1.0);
        vec3 haloColor = vec3(0.18, 0.45, 1.0) * uColor;
        vec3 jetColor = vec3(0.35, 0.68, 1.0) * uColor;
        vec3 pulsarCol = mix(vec3(0.0), coreColor, core);
        pulsarCol += haloColor * halo;
        pulsarCol += jetColor * jetIntensity * jetFlicker;
        col += pulsarCol * frontMask;
    }
    col *= (1.0 + uIntensity * 10.0);
    gl_FragColor = vec4(col, uAlpha);
}
)";

const char* fragFor(SkyMode m) {
    switch (m) {
        case SkyMode::Midnight: return kFS_Midnight;
        case SkyMode::Plasma: return kFS_Plasma;
        case SkyMode::Aurora: return kFS_Aurora;
        case SkyMode::Water: return kFS_Water;
        case SkyMode::Caustic: return kFS_Caustic;
        case SkyMode::Thunder: return kFS_Thunder;
        case SkyMode::Pulsar: return kFS_Pulsar;
        default: return kFS_Plasma;
    }
}

bool compileShader(GLenum type, const char* src, GLuint& out) {
    out = p_glCreateShader(type);
    p_glShaderSource(out, 1, &src, nullptr);
    p_glCompileShader(out);
    GLint ok = 0;
    p_glGetShaderiv(out, GL_COMPILE_STATUS, &ok);
    if (!ok && p_glGetShaderInfoLog) {
        char log[256];
        p_glGetShaderInfoLog(out, 256, nullptr, log);
        logLine("SkyShader compile err: %s", log);
    }
    return ok != 0;
}

bool buildProg(SkyMode mode) {
    const int idx = static_cast<int>(mode);
    if (g_progOk[idx]) return true;
    GLuint vs = 0, fs = 0;
    if (!compileShader(GL_VERTEX_SHADER, kVS, vs) || !compileShader(GL_FRAGMENT_SHADER, fragFor(mode), fs)) {
        logLine("SkyShaders: compile failed mode=%d", idx);
        return false;
    }
    GLuint prog = p_glCreateProgram();
    p_glAttachShader(prog, vs);
    p_glAttachShader(prog, fs);
    p_glLinkProgram(prog);
    GLint linked = 0;
    p_glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    p_glDeleteShader(vs);
    p_glDeleteShader(fs);
    if (!linked) {
        logLine("SkyShaders: link failed mode=%d", idx);
        return false;
    }
    g_progs[idx] = prog;
    g_progOk[idx] = true;
    if (g_aPos < 0) g_aPos = p_glGetAttribLocation(prog, "aPosition");
    return true;
}

bool initGl() {
    if (g_glReady) return true;
    if (!loadGles()) {
        logLine("SkyShaders: GLES procs missing");
        return false;
    }
    const GLfloat verts[] = {-1.f, -1.f, 1.f, -1.f, -1.f, 1.f, 1.f, 1.f};
    const unsigned short idx[] = {0, 1, 2, 1, 3, 2};
    p_glGenBuffers(1, &g_vbo);
    p_glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    p_glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    p_glGenBuffers(1, &g_ibo);
    p_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_ibo);
    p_glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(idx), idx, GL_STATIC_DRAW);
    g_glReady = true;
    logLine("SkyShaders: GL ready (7 Lexora skies)");
    return true;
}

float gameTimeSeconds() {
    static const auto t0 = std::chrono::steady_clock::now();
    return std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count();
}

void setUniform1f(GLuint prog, const char* name, float v) {
    GLint loc = p_glGetUniformLocation(prog, name);
    if (loc >= 0) p_glUniform1f(loc, v);
}
void setUniform2f(GLuint prog, const char* name, float a, float b) {
    GLint loc = p_glGetUniformLocation(prog, name);
    if (loc >= 0) p_glUniform2f(loc, a, b);
}
void setUniform3f(GLuint prog, const char* name, float a, float b, float c) {
    GLint loc = p_glGetUniformLocation(prog, name);
    if (loc >= 0) p_glUniform3f(loc, a, b, c);
}

void drawSky() {
    if (!g_enabled.load(std::memory_order_relaxed)) return;

    std::lock_guard lock(g_glMu);
    if (!initGl()) return;

    const int modeIdx = g_mode.load(std::memory_order_relaxed);
    if (modeIdx < 0 || modeIdx >= static_cast<int>(SkyMode::Count)) return;
    const SkyMode mode = static_cast<SkyMode>(modeIdx);
    if (!buildProg(mode)) return;

    GLuint prog = g_progs[modeIdx];
    p_glUseProgram(prog);

    GLint vp[4] = {0, 0, 1920, 1080};
    p_glGetIntegerv(GL_VIEWPORT, vp);
    const float w = static_cast<float>(std::max(1, vp[2]));
    const float h = static_cast<float>(std::max(1, vp[3]));
    const float aspect = w / h;
    const float t = gameTimeSeconds();
    const float speed = g_speed.load(std::memory_order_relaxed);
    // Slow auto-pan so sky is always “moving” even without camera matrix
    const float yaw = t * 0.04f * speed;
    const float pitch = 0.15f * std::sin(t * 0.12f * speed);

    setUniform1f(prog, "uYaw", yaw);
    setUniform1f(prog, "uPitch", pitch);
    setUniform1f(prog, "uFov", 70.f);
    setUniform1f(prog, "uAspect", aspect);

    const float intensity = g_intensity.load(std::memory_order_relaxed);
    const float alpha = g_alpha.load(std::memory_order_relaxed);
    const float scale = g_scale.load(std::memory_order_relaxed);
    const float tr = g_themeR.load(), tg = g_themeG.load(), tb = g_themeB.load();

    switch (mode) {
        case SkyMode::Midnight:
            setUniform1f(prog, "GameTime", t);
            setUniform1f(prog, "StarDensity", g_starDensity.load());
            setUniform1f(prog, "CloudSpeed", g_cloudSpeed.load() * speed);
            setUniform1f(prog, "CloudDensity", g_cloudDensity.load());
            setUniform1f(prog, "ShowMoon", g_showMoon.load() ? 1.f : 0.f);
            setUniform2f(prog, "iResolution", w, h);
            break;
        case SkyMode::Plasma:
            setUniform1f(prog, "GameTime", t);
            setUniform1f(prog, "PlasmaSpeed", speed);
            setUniform1f(prog, "PlasmaIntensity", intensity);
            setUniform3f(prog, "ThemeColor", tr, tg, tb);
            setUniform1f(prog, "UseTheme", g_useTheme.load() ? 1.f : 0.f);
            break;
        case SkyMode::Aurora:
            setUniform1f(prog, "GameTime", t);
            setUniform1f(prog, "AuroraSpeed", g_auroraSpeed.load() * speed);
            setUniform1f(prog, "AuroraIntensity", g_auroraIntensity.load() * intensity);
            setUniform1f(prog, "AuroraStars", 1.f);
            setUniform1f(prog, "AuroraReflect", g_auroraReflect.load() ? 1.f : 0.f);
            setUniform2f(prog, "iResolution", w, h);
            break;
        case SkyMode::Water:
        case SkyMode::Caustic:
        case SkyMode::Thunder:
        case SkyMode::Pulsar:
            setUniform1f(prog, "uTime", t);
            setUniform3f(prog, "uColor", tr, tg, tb);
            setUniform1f(prog, "uAlpha", alpha);
            setUniform1f(prog, "uSpeed", speed);
            setUniform1f(prog, "uScale", scale);
            setUniform1f(prog, "uIntensity", intensity * 0.01f);
            if (mode == SkyMode::Thunder) {
                setUniform1f(prog, "uThunderInterval", g_thunderInterval.load());
                setUniform1f(prog, "uThunderChance", g_thunderChance.load());
                setUniform1f(prog, "uThunderGlow", g_thunderGlow.load());
            }
            break;
        default:
            break;
    }

    p_glDisable(GL_DEPTH_TEST);
    p_glDisable(GL_CULL_FACE);
    p_glDisable(GL_SCISSOR_TEST);
    if (p_glDepthMask) p_glDepthMask(0);
    p_glEnable(GL_BLEND);
    if (p_glBlendEquation) p_glBlendEquation(GL_FUNC_ADD);
    if (p_glBlendFunc) p_glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    p_glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    p_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_ibo);
    if (g_aPos >= 0) {
        p_glEnableVertexAttribArray(static_cast<GLuint>(g_aPos));
        p_glVertexAttribPointer(static_cast<GLuint>(g_aPos), 2, GL_FLOAT, 0, 0, nullptr);
    }
    p_glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, nullptr);

    if (p_glDepthMask) p_glDepthMask(1);
    p_glUseProgram(0);
}

EGLBoolean swapDetour(EGLDisplay d, EGLSurface s) {
    drawSky();
    return g_swapOriginal ? g_swapOriginal(d, s) : eglSwapBuffers(d, s);
}

bool installSwapHook() {
    if (g_swapHooked) return true;
    void* egl = dlopen("libEGL.so", RTLD_NOW);
    if (!egl) egl = dlopen("libEGL.so.1", RTLD_NOW);
    if (!egl) return false;
    void* sym = dlsym(egl, "eglSwapBuffers");
    if (!sym) return false;
    void* o = nullptr;
    if (pl::memory::hook(sym, reinterpret_cast<void*>(&swapDetour), &o) != 0) {
        logLine("SkyShaders: eglSwapBuffers hook FAIL");
        return false;
    }
    g_swapOriginal = reinterpret_cast<EglSwapBuffersFn>(o);
    g_swapHooked = true;
    logLine("SkyShaders: eglSwapBuffers hooked");
    return true;
}

void onToggle(std::string_view, bool enabled) {
    g_enabled.store(enabled, std::memory_order_release);
    if (enabled) installSwapHook();
}

void onConfig(std::string_view, std::string_view key, std::string_view value) {
    try {
        if (key == "mode") {
            int m = std::stoi(std::string(value));
            if (m < 0) m = 0;
            if (m >= static_cast<int>(SkyMode::Count)) m = static_cast<int>(SkyMode::Count) - 1;
            g_mode.store(m, std::memory_order_relaxed);
        } else if (key == "speed") {
            g_speed.store(std::stof(std::string(value)), std::memory_order_relaxed);
        } else if (key == "intensity") {
            g_intensity.store(std::stof(std::string(value)), std::memory_order_relaxed);
        } else if (key == "alpha") {
            g_alpha.store(std::stof(std::string(value)), std::memory_order_relaxed);
        } else if (key == "scale") {
            g_scale.store(std::stof(std::string(value)), std::memory_order_relaxed);
        } else if (key == "starDensity") {
            g_starDensity.store(std::stof(std::string(value)), std::memory_order_relaxed);
        } else if (key == "cloudSpeed") {
            g_cloudSpeed.store(std::stof(std::string(value)), std::memory_order_relaxed);
        } else if (key == "cloudDensity") {
            g_cloudDensity.store(std::stof(std::string(value)), std::memory_order_relaxed);
        } else if (key == "showMoon") {
            g_showMoon.store(value == "true" || value == "1", std::memory_order_relaxed);
        } else if (key == "auroraSpeed") {
            g_auroraSpeed.store(std::stof(std::string(value)), std::memory_order_relaxed);
        } else if (key == "auroraIntensity") {
            g_auroraIntensity.store(std::stof(std::string(value)), std::memory_order_relaxed);
        } else if (key == "auroraReflect") {
            g_auroraReflect.store(value == "true" || value == "1", std::memory_order_relaxed);
        } else if (key == "thunderInterval") {
            g_thunderInterval.store(std::stof(std::string(value)), std::memory_order_relaxed);
        } else if (key == "thunderChance") {
            g_thunderChance.store(std::stof(std::string(value)), std::memory_order_relaxed);
        } else if (key == "thunderGlow") {
            g_thunderGlow.store(std::stof(std::string(value)), std::memory_order_relaxed);
        } else if (key == "useTheme") {
            g_useTheme.store(value == "true" || value == "1", std::memory_order_relaxed);
        } else if (key == "themeR") {
            g_themeR.store(std::stof(std::string(value)), std::memory_order_relaxed);
        } else if (key == "themeG") {
            g_themeG.store(std::stof(std::string(value)), std::memory_order_relaxed);
        } else if (key == "themeB") {
            g_themeB.store(std::stof(std::string(value)), std::memory_order_relaxed);
        }
    } catch (...) {
    }
}

} // namespace

void registerModule() {
    pl::modmenu::ModuleBuilder b(kModuleId, "Sky Shaders (Lexora)");
    b.description("Animated Lexora skies: Midnight, Plasma, Aurora, Water, Caustic, Thunder, Pulsar.")
        .defaultEnabled(false)
        .onToggle(onToggle)
        .onConfigChanged(onConfig);
    // 0=Midnight 1=Plasma 2=Aurora 3=Water 4=Caustic 5=Thunder 6=Pulsar
    b.config("mode", "Sky Type (0 Mid 1 Plasma 2 Aurora 3 Water 4 Caustic 5 Thunder 6 Pulsar)",
             pl::modmenu::ConfigType::SliderFloat, "1", "0", "6", "");
    b.config("speed", "Animation Speed", pl::modmenu::ConfigType::SliderFloat, "1", "0.05", "3", "");
    b.config("intensity", "Intensity", pl::modmenu::ConfigType::SliderFloat, "1", "0.1", "3", "");
    b.config("alpha", "Alpha (Water/Caustic/Thunder/Pulsar)", pl::modmenu::ConfigType::SliderFloat, "0.85", "0.1", "1", "");
    b.config("scale", "Scale", pl::modmenu::ConfigType::SliderFloat, "5", "1", "12", "");
    b.config("starDensity", "Midnight Star Density", pl::modmenu::ConfigType::SliderFloat, "0.985", "0.9", "0.999", "");
    b.config("cloudSpeed", "Midnight Cloud Speed", pl::modmenu::ConfigType::SliderFloat, "0.5", "0", "2", "");
    b.config("cloudDensity", "Midnight Cloud Density", pl::modmenu::ConfigType::SliderFloat, "0.5", "0", "1.5", "");
    b.config("showMoon", "Midnight Moon", pl::modmenu::ConfigType::Toggle, "true", "", "", "");
    b.config("auroraSpeed", "Aurora Speed", pl::modmenu::ConfigType::SliderFloat, "0.06", "0.01", "0.3", "");
    b.config("auroraIntensity", "Aurora Intensity", pl::modmenu::ConfigType::SliderFloat, "1.8", "0.2", "4", "");
    b.config("auroraReflect", "Aurora Reflect Below", pl::modmenu::ConfigType::Toggle, "true", "", "", "");
    b.config("thunderInterval", "Thunder Interval (s)", pl::modmenu::ConfigType::SliderFloat, "4", "1", "12", "");
    b.config("thunderChance", "Thunder Chance", pl::modmenu::ConfigType::SliderFloat, "0.65", "0.1", "1", "");
    b.config("thunderGlow", "Thunder Glow", pl::modmenu::ConfigType::SliderFloat, "1", "0.2", "3", "");
    b.config("useTheme", "Plasma Use Theme Color", pl::modmenu::ConfigType::Toggle, "false", "", "", "");
    b.config("themeR", "Theme R", pl::modmenu::ConfigType::SliderFloat, "0.6", "0", "1", "");
    b.config("themeG", "Theme G", pl::modmenu::ConfigType::SliderFloat, "0.1", "0", "1", "");
    b.config("themeB", "Theme B", pl::modmenu::ConfigType::SliderFloat, "0.4", "0", "1", "");
    b.registerModule();
}

void onSignaturesReady() {
    // Optional future: capture real camera yaw/pitch from game signatures
}

void shutdown() {
    g_enabled.store(false, std::memory_order_release);
}

} // namespace bactro::skyshaders
