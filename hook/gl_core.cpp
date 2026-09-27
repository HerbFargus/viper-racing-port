// gl_core.cpp -- the renderer (gl_core.h): OpenGL 3.3 through gl_table.h, and its state in shadow checks.
#define _CRT_SECURE_NO_WARNINGS
#include "gl_core.h"
#include <string.h>
#include <math.h>
#include <unordered_map>
#include "viperport.h"
#include "port.h"

SDL_Window* platform_window();

namespace gfx {

// ---- start-up: once per process, never part of a check -------------------------------------------------------------
namespace {

const char* VS = R"(
in vec3 aPos; in float aRhw; in vec4 aCol; in vec4 aSpec; in vec2 aUV;
uniform mat4 uM;          // WORLD x VIEW x PROJECTION, D3D row-vector order
uniform vec4 uClip;       // the D3DVIEWPORT2 clip window: ClipX, ClipY, ClipWidth, ClipHeight
uniform int uTL;          // 1: D3DTLVERTEX, in the game's screen pixels
uniform vec2 uRT;         // render-target size
uniform vec2 uMap;        // game pixels -> target pixels: scale, x offset
SHADE out vec4 vCol; SHADE out vec4 vSpec; out vec2 vUV;
void main() {
    if (uTL == 1) {
        vec2 t = vec2(uMap.y + aPos.x * uMap.x, aPos.y * uMap.x);
        gl_Position = vec4(t.x * 2.0 / uRT.x - 1.0, t.y * 2.0 / uRT.y - 1.0, aPos.z * 2.0 - 1.0, 1.0);
    } else {
        vec4 p = uM * vec4(aPos, 1.0);
        // D3D: sx = X + W (x/w - ClipX) / ClipW, sy = Y + H (ClipY - y/w) / ClipH, sz = z/w -- and the
        // render target's rows run top-down (row 0 = GL window y 0), so y needs no flip beyond D3D's own
        gl_Position = vec4(2.0 * (p.x - uClip.x * p.w) / uClip.z - p.w,
                           2.0 * (uClip.y * p.w - p.y) / uClip.w - p.w,
                           2.0 * p.z - p.w, p.w);
    }
    vCol = aCol; vSpec = aSpec; vUV = aUV;
}
)";

const char* FS = R"(
SHADE in vec4 vCol; SHADE in vec4 vSpec; in vec2 vUV;
uniform sampler2D uTex;
uniform int uTexOn, uBlend, uTexAlpha, uKeyed, uSpec, uFog, uAlphaFunc;
uniform float uAlphaRef;
uniform vec3 uFogCol;
out vec4 frag;
bool passes(float a) {                    // D3DCMPFUNC on 0..255 alpha
    float x = floor(a * 255.0 + 0.5), r = uAlphaRef;
    if (uAlphaFunc == 1) return false;    if (uAlphaFunc == 2) return x < r;
    if (uAlphaFunc == 3) return x == r;   if (uAlphaFunc == 4) return x <= r;
    if (uAlphaFunc == 5) return x > r;    if (uAlphaFunc == 6) return x != r;
    if (uAlphaFunc == 7) return x >= r;   return true;
}
void main() {
    vec4 c = vCol;
    if (uTexOn == 1) {
        vec4 t = texture(uTex, vUV);
        if (uKeyed == 1 && t.a < 0.5) discard;                     // COLORKEYENABLE on a keyed texture
        if (uBlend == 1) c = t;                                     // DECAL
        else if (uBlend == 4) c = t * vCol;                         // MODULATEALPHA
        else c = vec4(t.rgb * vCol.rgb, uTexAlpha == 1 ? t.a : vCol.a);   // MODULATE
    }
    if (uSpec == 1) c.rgb = min(c.rgb + vSpec.rgb, vec3(1.0));
    if (uFog == 1) c.rgb = mix(uFogCol, c.rgb, vSpec.a);           // vertex fog: specular alpha
    if (uAlphaFunc != 8 && !passes(c.a)) discard;                  // ALPHATESTENABLE
    frag = c;
}
)";

const char* COMP_VS = R"(
in vec2 aPos; out vec2 vUV;
void main() { vUV = vec2(aPos.x * 0.5 + 0.5, aPos.y * 0.5 + 0.5); gl_Position = vec4(aPos, 0.0, 1.0); }
)";
const char* COMP_FS = R"(
in vec2 vUV; uniform sampler2D uTex; out vec4 frag;
void main() { frag = texture(uTex, vUV); }    // premultiplied: only what the 2D drew is opaque
)";

struct Program {
    GLuint id = 0;
    GLint uM, uClip, uTL, uRT, uMap, uTex, uTexOn, uBlend, uTexAlpha, uKeyed, uSpec, uFog, uAlphaFunc, uAlphaRef, uFogCol;
};

struct Infra {                                   // made once by start()
    bool ready = false;
    DWORD thread = 0;
    SDL_GLContext ctx = 0;
    Program smooth, flat;
    GLuint comp = 0, comp_tex_uniform = 0;
    GLuint vao = 0, vbo = 0, ibo = 0, quad_vao = 0, quad_vbo = 0, page_tex = 0;
} in;

GLuint compile(GLenum type, const char* prelude, const char* src) {
    GLuint s = gl_api.CreateShader(type);
    const char* parts[2] = {prelude, src};
    gl_api.ShaderSource(s, 2, parts, 0);
    gl_api.CompileShader(s);
    GLint ok = 0;
    gl_api.GetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        gl_api.GetShaderInfoLog(s, sizeof log, 0, log);
        logf("renderer: shader compile failed: %s", log);
    }
    return s;
}

GLuint link(const char* prelude, const char* vs, const char* fs, bool full) {
    GLuint p = gl_api.CreateProgram();
    gl_api.AttachShader(p, compile(GL_VERTEX_SHADER, prelude, vs));
    gl_api.AttachShader(p, compile(GL_FRAGMENT_SHADER, prelude, fs));
    if (full) {
        gl_api.BindAttribLocation(p, 0, "aPos"); gl_api.BindAttribLocation(p, 1, "aRhw");
        gl_api.BindAttribLocation(p, 2, "aCol"); gl_api.BindAttribLocation(p, 3, "aSpec"); gl_api.BindAttribLocation(p, 4, "aUV");
    } else {
        gl_api.BindAttribLocation(p, 0, "aPos");
    }
    gl_api.LinkProgram(p);
    GLint ok = 0;
    gl_api.GetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        gl_api.GetProgramInfoLog(p, sizeof log, 0, log);
        logf("renderer: shader link failed: %s", log);
    }
    return p;
}

void make_program(Program& pr, bool flat) {
    const char* prelude = flat ? "#version 330 core\n#define SHADE flat\n" : "#version 330 core\n#define SHADE\n";
    pr.id = link(prelude, VS, FS, true);
#define U(n) pr.n = gl_api.GetUniformLocation(pr.id, #n);
    U(uM) U(uClip) U(uTL) U(uRT) U(uMap) U(uTex) U(uTexOn) U(uBlend) U(uTexAlpha) U(uKeyed) U(uSpec) U(uFog) U(uAlphaFunc)
    U(uAlphaRef) U(uFogCol)
#undef U
}

bool on_gl_thread() {
    if (!in.ready) return false;
    if (GetCurrentThreadId() == in.thread) return true;
    static bool said;
    if (!said) logf("renderer: a drawing call from another thread (%lu) was skipped", GetCurrentThreadId()), said = true;
    return false;
}

}  // namespace

State st;
Page pg;
std::set<Surface*> handles;

// ---- shadow checks ----------------------------------------------------------------------------------------------------
namespace {

template <class T> void put(std::vector<uint8_t>& o, const T& v) { o.insert(o.end(), (const uint8_t*)&v, (const uint8_t*)&v + sizeof v); }
template <class T> void get(const uint8_t*& i, T& v) { memcpy(&v, i, sizeof v); i += sizeof v; }
template <class T> void put_vec(std::vector<uint8_t>& o, const std::vector<T>& v) {
    uint32_t n = (uint32_t)v.size();
    put(o, n);
    if (n) o.insert(o.end(), (const uint8_t*)v.data(), (const uint8_t*)(v.data() + n));
}
template <class T> void get_vec(const uint8_t*& i, std::vector<T>& v) {
    uint32_t n;
    get(i, n);
    if (v.size() != n) v.resize(n);                // the same size keeps the same buffer: a Lock's pointer holds
    if (n) memcpy(v.data(), i, n * sizeof(T));
    i += n * sizeof(T);
}

// the three pieces of the renderer's own state, as objects a check can save
struct StateObj : Obj {
    StateObj() : Obj(K_STATE) {}
    void save_fields(std::vector<uint8_t>& o) const override { put(o, st); }
    void load_fields(const uint8_t*& i) override { get(i, st); }
} g_state_obj;
struct PageObj : Obj {                           // (the overlay is scratch: draw_page rewrites all of it)
    PageObj() : Obj(K_PAGE) {}
    void save_fields(std::vector<uint8_t>& o) const override { put_vec(o, pg.page); put_vec(o, pg.under); }
    void load_fields(const uint8_t*& i) override { get_vec(i, pg.page); get_vec(i, pg.under); }
} g_page_obj;
struct HandlesObj : Obj {
    HandlesObj() : Obj(K_HANDLES) {}
    void save_fields(std::vector<uint8_t>& o) const override {
        put(o, (uint32_t)handles.size());
        for (Surface* s : handles) put(o, s);
    }
    void load_fields(const uint8_t*& i) override {
        uint32_t n;
        get(i, n);
        handles.clear();
        for (uint32_t k = 0; k < n; k++) { Surface* s; get(i, s); handles.insert(s); }
    }
} g_handles_obj;

const char* kind_name(Kind k) {
    static const char* const n[] = {"device state", "2D page", "texture handle list", "IDirectDraw", "IDirectDraw2",
                                    "IDirect3D2", "device", "viewport", "material", "surface"};
    return k <= K_SURFACE ? n[k] : "?";
}

struct Entry {
    Obj* o;
    std::vector<uint8_t> s, e1;                  // at its first touch (or birth); after the original's pass
    bool t1 = false, t2 = false;                 // touched in the original's pass, the rewrite's
    bool born1 = false, born2 = false, taken = false;
};
struct Check {
    bool active = false;
    std::vector<Entry> e;
    std::unordered_map<Obj*, size_t> at;
};
__declspec(thread) Check* t_chk;
__declspec(thread) std::set<Obj*>* t_deleted;    // while a check's end deletes: what's already gone

int check_phase() {
    if (!t_chk || !t_chk->active) return 0;
    return shadow_com_phase();
}

Entry* entry(Obj* o) {
    auto f = t_chk->at.find(o);
    return f == t_chk->at.end() ? 0 : &t_chk->e[f->second];
}
Entry& add(Obj* o) {
    t_chk->at[o] = t_chk->e.size();
    t_chk->e.push_back(Entry());
    Entry& e = t_chk->e.back();
    e.o = o;
    o->save(e.s);
    return e;
}

uint32_t refs_in(const std::vector<uint8_t>& v) {
    uint32_t r = 0;
    if (v.size() >= 4) memcpy(&r, v.data(), 4);
    return r;
}

void chk_begin() {
    if (!t_chk) t_chk = new Check;
    t_chk->active = true;
    if (!t_chk->e.empty()) t_chk->e.clear(), t_chk->at.clear();   // (every check comes here, physics ones too)
}

void chk_after_original() {
    if (!t_chk || !t_chk->active) return;
    for (Entry& e : t_chk->e) {
        e.o->save(e.e1);
        const uint8_t* p = e.s.data();
        e.o->load(p);
    }
}

bool chk_differs(char* where, size_t n) {
    if (!t_chk || !t_chk->active) return false;
    std::vector<uint8_t> e2;
    for (Entry& e : t_chk->e) {
        e2.clear();
        e.o->save(e2);
        const std::vector<uint8_t>& e1 = e.t1 ? e.e1 : e.s;
        const char* k = kind_name(e.o->kind);
        if (e.born2) {                           // made in the rewrite's pass alone: fine if it went again
            if (refs_in(e2) == 0) continue;
            _snprintf(where, n, "the renderer: the rewrite made a %s the original didn't", k);
        } else if (e.born1 && refs_in(e1) == 0) {   // made and released in the original's pass
            if (!e.taken || refs_in(e2) == 0) continue;
            _snprintf(where, n, "the renderer: a %s the original made and released is still held by the rewrite", k);
        } else if (e.born1 && !e.taken) {
            _snprintf(where, n, "the renderer: the original made a %s the rewrite didn't", k);
        } else {
            if (e1 == e2) continue;
            size_t b = 0;
            while (b < e1.size() && b < e2.size() && e1[b] == e2[b]) b++;
            if (e.o->kind == K_SURFACE) {
                Surface* s = static_cast<Surface*>(e.o);
                _snprintf(where, n, "the renderer's %s (%dx%d%s), state byte %u of %u", k, s->w, s->h,
                          s->is_back ? ", the back buffer" : "", (unsigned)b, (unsigned)e1.size());
            } else {
                _snprintf(where, n, "the renderer's %s, state byte %u of %u%s", k, (unsigned)b, (unsigned)e1.size(),
                          b < 4 ? " (its reference count)" : "");
            }
        }
        where[n - 1] = 0;
        return true;
    }
    return false;
}

void chk_end(bool original_only) {
    if (!t_chk || !t_chk->active) return;
    t_chk->active = false;
    // the original's results stand (after the original's pass alone, they already do)
    if (!original_only)
        for (Entry& e : t_chk->e) {
            const std::vector<uint8_t>& e1 = e.t1 ? e.e1 : e.s;
            const uint8_t* p = e1.data();
            e.o->load(p);
        }
    // what went for good during the check goes now; so does what only the rewrite's pass made
    std::set<Obj*> deleted;
    t_deleted = &deleted;
    for (Entry& e : t_chk->e) {
        if (deleted.count(e.o) || e.o->kind <= K_HANDLES) continue;
        bool stale = !original_only && e.born2;
        if (stale || e.o->refs <= 0) delete e.o;
    }
    t_deleted = 0;
    t_chk->e.clear();
    t_chk->at.clear();
}

const ShadowState k_hooks = {chk_begin, chk_after_original, chk_differs, chk_end, gl_record_name};

}  // namespace

void check_hooks_install() { shadow_set_state(&k_hooks); }

void touch(Obj* o) {
    int ph = check_phase();
    if (!ph) return;
    Entry* e = entry(o);
    if (!e) e = &add(o);
    (ph == 1 ? e->t1 : e->t2) = true;
}
void touch_state() { touch(&g_state_obj); }
void touch_page() { touch(&g_page_obj); }
void touch_handles() { touch(&g_handles_obj); }

Obj* made(Kind k) {
    if (check_phase() != 2) return 0;
    for (Entry& e : t_chk->e)
        if (e.born1 && !e.taken && e.o->kind == k) {
            e.taken = true;
            e.t2 = true;
            return e.o;                          // as it was born: the check put it back
        }
    return 0;
}

void born(Obj* o) {
    int ph = check_phase();
    if (!ph) return;
    Entry& e = add(o);
    if (ph == 1) e.born1 = e.t1 = true;
    else e.born2 = e.t2 = true;
}

void gone(Obj* o) {
    if (check_phase()) return;                   // at the check's end (chk_end)
    delete o;
}

Obj::~Obj() {
    if (t_deleted) t_deleted->insert(this);
}

void Obj::save(std::vector<uint8_t>& out) const {
    out.clear();
    put(out, refs);
    save_fields(out);
}
void Obj::load(const uint8_t*& in) {
    get(in, refs);
    load_fields(in);
}
ULONG Obj::hold() {
    touch(this);
    return (ULONG)++refs;
}
ULONG Obj::drop() {
    touch(this);
    LONG n = --refs;
    if (n == 0) gone(this);
    return (ULONG)n;
}

// levels live as long as their root
ULONG Surface::hold() { return root && root != this ? 1 : Obj::hold(); }
ULONG Surface::drop() { return root && root != this ? 1 : Obj::drop(); }

Surface::~Surface() {
    if (tex && in.ready) {
        glr::Direct d;
        gl_api.DeleteTextures(1, &tex);
    }
    handles.erase(this);
    delete next_mip;                             // a chain's levels belong to its root
    if (back) back->drop();
}

void Surface::save_fields(std::vector<uint8_t>& o) const {
    put(o, caps); put(o, flags); put(o, w); put(o, h); put(o, backbuffers); put(o, mip_count); put(o, fmt);
    put(o, root); put(o, next_mip); put(o, back); put(o, is_back); put(o, keyed); put(o, key); put(o, gen); put(o, tex);
    put_vec(o, uploaded);
    put_vec(o, pixels);
}
void Surface::load_fields(const uint8_t*& i) {
    get(i, caps); get(i, flags); get(i, w); get(i, h); get(i, backbuffers); get(i, mip_count); get(i, fmt);
    get(i, root); get(i, next_mip); get(i, back); get(i, is_back); get(i, keyed); get(i, key); get(i, gen); get(i, tex);
    get_vec(i, uploaded);
    get_vec(i, pixels);
}
void Material::save_fields(std::vector<uint8_t>& o) const { put(o, m); }
void Material::load_fields(const uint8_t*& i) { get(i, m); }
void Viewport::save_fields(std::vector<uint8_t>& o) const { put(o, vp); put(o, background); }
void Viewport::load_fields(const uint8_t*& i) { get(i, vp); get(i, background); }
void Device::save_fields(std::vector<uint8_t>& o) const { put(o, current); }
void Device::load_fields(const uint8_t*& i) { get(i, current); }

// ---- the target -----------------------------------------------------------------------------------------------------
namespace {

// a rectangle in the game's screen pixels -> the target's; one as wide as the game's screen spans the
// target's whole width (the widescreen sides)
struct Rect { int x, y, w, h; bool full; };
Rect map_rect(int x, int y, int w, int h) {
    Rect r;
    r.full = x <= 0 && x + w >= st.w;
    r.x = r.full ? 0 : (int)floorf(st.ox + x * st.scale + 0.5f);
    r.w = r.full ? st.rt_w : (int)floorf(st.ox + (x + w) * st.scale + 0.5f) - r.x;
    r.y = (int)floorf(y * st.scale + 0.5f);
    r.h = (int)floorf((y + h) * st.scale + 0.5f) - r.y;
    return r;
}

// the clip window for the target: widened by the extra width when the viewport spans the screen
void clip_window(const D3DVIEWPORT2& vp, float out[4]) {
    Rect r = map_rect(vp.dwX, vp.dwY, vp.dwWidth, vp.dwHeight);
    float k = r.full ? (float)r.w / (vp.dwWidth * st.scale) : 1.0f;
    out[0] = vp.dvClipX * k, out[1] = vp.dvClipY, out[2] = vp.dvClipWidth * k, out[3] = vp.dvClipHeight;
}

void make_target() {                             // the render target at the window's size
    int ww = 0, wh = 0;
    glr::GetDrawableSize(platform_window(), &ww, &wh);
    if (ww <= 0 || wh <= 0) ww = st.w, wh = st.h;
    touch_state();
    st.scale = (float)wh / st.h;
    if (st.w * st.scale > ww) st.scale = (float)ww / st.w;   // a window narrower than 4:3: fit the width
    st.ox = (ww - st.w * st.scale) * 0.5f;
    if (ww == st.rt_w && wh == st.rt_h && st.fbo) return;
    st.rt_w = ww, st.rt_h = wh;
    if (st.fbo) {
        glr::DeleteFramebuffers(1, &st.fbo);
        glr::DeleteTextures(1, &st.fbo_color);
        glr::DeleteRenderbuffers(1, &st.fbo_depth);
    }
    glr::GenTextures(1, &st.fbo_color);
    glr::BindTexture(GL_TEXTURE_2D, st.fbo_color);
    glr::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, ww, wh, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
    glr::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glr::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glr::GenRenderbuffers(1, &st.fbo_depth);
    glr::BindRenderbuffer(GL_RENDERBUFFER, st.fbo_depth);
    glr::RenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, ww, wh);
    glr::GenFramebuffers(1, &st.fbo);
    glr::BindFramebuffer(GL_FRAMEBUFFER, st.fbo);
    glr::FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, st.fbo_color, 0);
    glr::FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, st.fbo_depth);
    if (glr::CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) logf("renderer: render target incomplete");
    glr::Disable(GL_SCISSOR_TEST);
    glr::ClearColor(0, 0, 0, 1);
    glr::ClearDepth(1.0);
    glr::DepthMask(GL_TRUE);
    glr::Clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

}  // namespace

void set_defaults() {
    touch_state();
    memset(st.rs, 0, sizeof st.rs);
    st.rs[D3DRENDERSTATE_TEXTUREADDRESS] = D3DTADDRESS_WRAP;
    st.rs[D3DRENDERSTATE_TEXTUREADDRESSU] = D3DTADDRESS_WRAP;
    st.rs[D3DRENDERSTATE_TEXTUREADDRESSV] = D3DTADDRESS_WRAP;
    st.rs[D3DRENDERSTATE_TEXTUREPERSPECTIVE] = 1;
    st.rs[D3DRENDERSTATE_ZENABLE] = 1;
    st.rs[D3DRENDERSTATE_FILLMODE] = D3DFILL_SOLID;
    st.rs[D3DRENDERSTATE_SHADEMODE] = D3DSHADE_GOURAUD;
    st.rs[D3DRENDERSTATE_ZWRITEENABLE] = 1;
    st.rs[D3DRENDERSTATE_TEXTUREMAG] = D3DFILTER_NEAREST;
    st.rs[D3DRENDERSTATE_TEXTUREMIN] = D3DFILTER_NEAREST;
    st.rs[D3DRENDERSTATE_SRCBLEND] = D3DBLEND_ONE;
    st.rs[D3DRENDERSTATE_DESTBLEND] = D3DBLEND_ZERO;
    st.rs[D3DRENDERSTATE_TEXTUREMAPBLEND] = D3DTBLEND_MODULATE;
    st.rs[D3DRENDERSTATE_CULLMODE] = D3DCULL_CCW;
    st.rs[D3DRENDERSTATE_ZFUNC] = D3DCMP_LESSEQUAL;
    st.rs[D3DRENDERSTATE_ALPHAFUNC] = D3DCMP_ALWAYS;
    D3DMATRIX id = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    st.world = st.view = st.proj = id;
    memset(&st.vp, 0, sizeof st.vp);
    st.vp.dwWidth = st.w, st.vp.dwHeight = st.h;
    st.vp.dvClipX = -1, st.vp.dvClipWidth = 2, st.vp.dvClipY = 1, st.vp.dvClipHeight = 2, st.vp.dvMaxZ = 1;
}

void set_mode(int w, int h) {
    touch_state();
    touch_page();
    st.w = w, st.h = h;
    pg.page.assign((size_t)w * h, 0);
    pg.under.assign((size_t)w * h, 0);
    pg.overlay.assign((size_t)w * h, 0);
    if (!in.ready) return;
    if (st.small_fbo) glr::DeleteFramebuffers(1, &st.small_fbo), glr::DeleteTextures(1, &st.small_color);
    glr::GenTextures(1, &st.small_color);
    glr::BindTexture(GL_TEXTURE_2D, st.small_color);
    glr::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
    glr::GenFramebuffers(1, &st.small_fbo);
    glr::BindFramebuffer(GL_FRAMEBUFFER, st.small_fbo);
    glr::FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, st.small_color, 0);
    glr::BindTexture(GL_TEXTURE_2D, in.page_tex);
    glr::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
    make_target();
    if (shadow_com_phase() != 2) logf("renderer: display mode %dx%d, drawn at %dx%d", w, h, st.rt_w, st.rt_h);
}

bool started() { return in.ready; }

bool start() {
    if (in.ready) return true;
    // once per process: a check can't run it twice, so the original's pass keeps its result (port.h)
    if (shadow_com_phase()) shadow_keep_original("started the renderer");
    SDL_Window* win = platform_window();
    if (!win) {
        logf("renderer: no SDL window (viperport.ini [platform] sdl=1 is needed)");
        return false;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    in.ctx = SDL_GL_CreateContext(win);
    if (!in.ctx) {
        logf("renderer: can't create an OpenGL 3.3 context: %s", SDL_GetError());
        return false;
    }
    if (!gl_api_load()) {
        logf("renderer: OpenGL 3.3 functions missing");
        return false;
    }
    glr::Direct direct;
    SDL_GL_SetSwapInterval(1);
    in.thread = GetCurrentThreadId();
    logf("renderer: OpenGL %s on %s", (const char*)gl_api.GetString(GL_VERSION), (const char*)gl_api.GetString(GL_RENDERER));
    make_program(in.smooth, false);
    make_program(in.flat, true);
    in.comp = link("#version 330 core\n", COMP_VS, COMP_FS, false);
    in.comp_tex_uniform = gl_api.GetUniformLocation(in.comp, "uTex");
    gl_api.GenVertexArrays(1, &in.vao);
    gl_api.BindVertexArray(in.vao);
    gl_api.GenBuffers(1, &in.vbo);
    gl_api.GenBuffers(1, &in.ibo);
    gl_api.BindBuffer(GL_ARRAY_BUFFER, in.vbo);
    gl_api.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, in.ibo);
    gl_api.VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 32, (void*)0);
    gl_api.VertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, 32, (void*)12);
    gl_api.VertexAttribPointer(2, GL_BGRA, GL_UNSIGNED_BYTE, GL_TRUE, 32, (void*)16);
    gl_api.VertexAttribPointer(3, GL_BGRA, GL_UNSIGNED_BYTE, GL_TRUE, 32, (void*)20);
    gl_api.VertexAttribPointer(4, 2, GL_FLOAT, GL_FALSE, 32, (void*)24);
    for (int i = 0; i < 5; i++) gl_api.EnableVertexAttribArray(i);
    static const float quad[] = {-1, -1, 1, -1, -1, 1, 1, 1};
    gl_api.GenVertexArrays(1, &in.quad_vao);
    gl_api.BindVertexArray(in.quad_vao);
    gl_api.GenBuffers(1, &in.quad_vbo);
    gl_api.BindBuffer(GL_ARRAY_BUFFER, in.quad_vbo);
    gl_api.BufferData(GL_ARRAY_BUFFER, sizeof quad, quad, 0x88E4 /*GL_STATIC_DRAW*/);
    gl_api.VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 8, (void*)0);
    gl_api.EnableVertexAttribArray(0);
    gl_api.GenTextures(1, &in.page_tex);
    gl_api.BindTexture(GL_TEXTURE_2D, in.page_tex);
    gl_api.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl_api.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl_api.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl_api.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl_api.ProvokingVertex(GL_FIRST_VERTEX_CONVENTION);        // D3D flat shading takes the first vertex
    in.ready = true;
    set_defaults();
    set_mode(st.w, st.h);
    return true;
}

// ---- the page the game's 2D draws on ----------------------------------------------------------------------------------
namespace {

void read_page() {                               // Lock of the back buffer: the 3D as it stands
    if (!on_gl_thread()) return;
    touch_page();
    // the 4:3 middle, shrunk to the game's resolution (the 2D reads it: alpha pastes, XOR)
    int x0 = (int)floorf(st.ox + 0.5f), x1 = (int)floorf(st.ox + st.w * st.scale + 0.5f);
    glr::BindFramebuffer(GL_READ_FRAMEBUFFER, st.fbo);
    glr::BindFramebuffer(GL_DRAW_FRAMEBUFFER, st.small_fbo);
    glr::Disable(GL_SCISSOR_TEST);
    glr::BlitFramebuffer(x0, 0, x1, st.rt_h, 0, 0, st.w, st.h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glr::BindFramebuffer(GL_READ_FRAMEBUFFER, st.small_fbo);
    glr::PixelStorei(GL_PACK_ALIGNMENT, 2);
    glr::ReadPixels(0, 0, st.w, st.h, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, pg.page.data());
    pg.under = pg.page;
}

void draw_page() {                               // Unlock: what the 2D drew, over the full-resolution 3D
    if (!on_gl_thread()) return;
    size_t n = pg.page.size(), changed = 0;
    for (size_t i = 0; i < n; i++) {
        uint16_t c = pg.page[i];
        if (c == pg.under[i]) { pg.overlay[i] = 0; continue; }
        uint32_t r = ((c >> 11) & 31) * 255 / 31, gg = ((c >> 5) & 63) * 255 / 63, b = (c & 31) * 255 / 31;
        pg.overlay[i] = 0xFF000000u | (b << 16) | (gg << 8) | r;
        changed++;
    }
    if (!changed) return;
    glr::BindFramebuffer(GL_FRAMEBUFFER, st.fbo);
    int x0 = (int)floorf(st.ox + 0.5f), x1 = (int)floorf(st.ox + st.w * st.scale + 0.5f);
    glr::Viewport(x0, 0, x1 - x0, (int)floorf(st.h * st.scale + 0.5f));
    glr::Disable(GL_SCISSOR_TEST); glr::Disable(GL_DEPTH_TEST); glr::Disable(GL_CULL_FACE);
    glr::Enable(GL_BLEND);
    glr::BlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glr::PixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glr::ActiveTexture(GL_TEXTURE0);
    glr::BindTexture(GL_TEXTURE_2D, in.page_tex);
    glr::TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, st.w, st.h, GL_RGBA, GL_UNSIGNED_BYTE, pg.overlay.data());
    glr::UseProgram(in.comp);
    glr::Uniform1i(in.comp_tex_uniform, 0);
    glr::BindVertexArray(in.quad_vao);
    glr::DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

}  // namespace

void present() {
    if (!on_gl_thread()) return;
    glr::BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glr::BindFramebuffer(GL_READ_FRAMEBUFFER, st.fbo);
    glr::Disable(GL_SCISSOR_TEST);
    // the target's row 0 is the top of the picture; the window's is the bottom
    glr::BlitFramebuffer(0, 0, st.rt_w, st.rt_h, 0, st.rt_h, st.rt_w, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glr::SwapWindow(platform_window());
    int x0 = (int)floorf(st.ox + 0.5f), x1 = (int)floorf(st.ox + st.w * st.scale + 0.5f);
    glr::SetView(x0, 0, x1 - x0, (int)floorf(st.h * st.scale + 0.5f), st.w, st.h);   // for the mouse
    // the sides only get drawn by a full-width 3D view; anything else there would be stale
    glr::BindFramebuffer(GL_FRAMEBUFFER, st.fbo);
    glr::Enable(GL_SCISSOR_TEST);
    glr::ClearColor(0, 0, 0, 1);
    glr::Scissor(0, 0, x0, st.rt_h);
    glr::Clear(GL_COLOR_BUFFER_BIT);
    glr::Scissor(x1, 0, st.rt_w - x1, st.rt_h);
    glr::Clear(GL_COLOR_BUFFER_BIT);
    make_target();                               // follows the window if it changed size
    touch_state();
    st.frames++;
}

// Switched away: the picture shown once more. A full-screen window's frames bypass the desktop's compositor, so the
// taskbar's preview keeps whatever it last composited (a menu); swapped again once the window is composited, it
// shows the race. Outside the game's frame: nothing recorded, nothing changed but the bindings present leaves.
void repaint() {
    if (!in.ready || !st.fbo || !on_gl_thread() || shadow_com_check()) return;
    glr::Direct direct;
    glr::BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glr::BindFramebuffer(GL_READ_FRAMEBUFFER, st.fbo);
    glr::Disable(GL_SCISSOR_TEST);               // (every draw and clear sets its own scissor)
    glr::BlitFramebuffer(0, 0, st.rt_w, st.rt_h, 0, st.rt_h, st.rt_w, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glr::SwapWindow(platform_window());
    glr::BindFramebuffer(GL_FRAMEBUFFER, st.fbo);
}

// ---- surfaces -----------------------------------------------------------------------------------------------------------
void pixel_format(Format f, DDPIXELFORMAT& pf) {
    memset(&pf, 0, sizeof pf);
    pf.dwSize = sizeof pf;
    pf.dwFlags = DDPF_RGB;
    pf.dwRGBBitCount = 16;
    switch (f) {
    case F565: pf.dwRBitMask = 0xF800, pf.dwGBitMask = 0x07E0, pf.dwBBitMask = 0x001F; break;
    case F555: pf.dwRBitMask = 0x7C00, pf.dwGBitMask = 0x03E0, pf.dwBBitMask = 0x001F; break;
    case F1555: pf.dwFlags |= DDPF_ALPHAPIXELS; pf.dwRBitMask = 0x7C00, pf.dwGBitMask = 0x03E0, pf.dwBBitMask = 0x001F;
                pf.dwRGBAlphaBitMask = 0x8000; break;
    case F4444: pf.dwFlags |= DDPF_ALPHAPIXELS; pf.dwRBitMask = 0x0F00, pf.dwGBitMask = 0x00F0, pf.dwBBitMask = 0x000F;
                pf.dwRGBAlphaBitMask = 0xF000; break;
    }
}

Format format_of(const DDPIXELFORMAT& pf) {
    if (pf.dwFlags & DDPF_ALPHAPIXELS) return pf.dwRGBAlphaBitMask == 0xF000 ? F4444 : F1555;
    return pf.dwGBitMask == 0x03E0 ? F555 : F565;
}

void describe(Surface* s, LPDDSURFACEDESC d) {
    DWORD size = d->dwSize ? d->dwSize : sizeof(DDSURFACEDESC);
    memset(d, 0, size);
    d->dwSize = size;
    d->dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT;
    d->dwWidth = s->w, d->dwHeight = s->h;
    d->lPitch = s->w * 2;
    d->ddsCaps = s->caps;
    if (s->backbuffers) d->dwFlags |= DDSD_BACKBUFFERCOUNT, d->dwBackBufferCount = s->backbuffers;
    if (s->mip_count > 1) d->dwFlags |= DDSD_MIPMAPCOUNT, d->dwMipMapCount = s->mip_count;
    if (s->caps.dwCaps & DDSCAPS_ZBUFFER) {
        d->dwFlags = (d->dwFlags & ~DDSD_PIXELFORMAT) | DDSD_ZBUFFERBITDEPTH;
        d->dwZBufferBitDepth = 16;
    } else {
        pixel_format(s->fmt, d->ddpfPixelFormat);
    }
    if (s->keyed)
        d->dwFlags |= DDSD_CKSRCBLT, d->ddckCKSrcBlt.dwColorSpaceLowValue = d->ddckCKSrcBlt.dwColorSpaceHighValue = s->key;
}

HRESULT create_surface(LPDDSURFACEDESC d, Surface** out) {
    DWORD caps = d->ddsCaps.dwCaps;
    Surface* s = make<Surface>();
    s->caps = d->ddsCaps;
    s->flags = d->dwFlags;
    s->root = s;
    if (caps & DDSCAPS_PRIMARYSURFACE) {
        s->w = st.w, s->h = st.h;
        s->backbuffers = (d->dwFlags & DDSD_BACKBUFFERCOUNT) ? d->dwBackBufferCount : 0;
        if (caps & DDSCAPS_FLIP) {
            Surface* b = make<Surface>();
            b->caps.dwCaps = DDSCAPS_BACKBUFFER | DDSCAPS_3DDEVICE | DDSCAPS_VIDEOMEMORY | DDSCAPS_FLIP;
            b->w = st.w, b->h = st.h, b->root = b, b->is_back = true;
            s->back = b;
        }
    } else if (caps & DDSCAPS_ZBUFFER) {
        s->w = d->dwWidth, s->h = d->dwHeight;
        s->caps.dwCaps |= DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM;
    } else {
        s->w = d->dwWidth, s->h = d->dwHeight;
        s->fmt = (d->dwFlags & DDSD_PIXELFORMAT) ? format_of(d->ddpfPixelFormat) : F565;
        s->pixels.assign((size_t)s->w * s->h, 0);
        if (d->dwFlags & DDSD_CKSRCBLT) s->keyed = true, s->key = (uint16_t)d->ddckCKSrcBlt.dwColorSpaceLowValue;
        int levels = (d->dwFlags & DDSD_MIPMAPCOUNT) ? (int)d->dwMipMapCount : 1;
        s->mip_count = levels;
        Surface* prev = s;
        for (int i = 1; i < levels && prev->w > 1 && prev->h > 1; i++) {
            Surface* l = make<Surface>();
            l->caps = s->caps;
            l->caps.dwCaps |= DDSCAPS_MIPMAP;
            l->w = prev->w / 2, l->h = prev->h / 2;
            l->fmt = s->fmt, l->keyed = s->keyed, l->key = s->key, l->root = s;
            l->pixels.assign((size_t)l->w * l->h, 0);
            prev->next_mip = l;
            prev = l;
        }
    }
    *out = s;
    return DD_OK;
}

HRESULT lock(Surface* s, LPDDSURFACEDESC d) {
    describe(s, d);
    if (s->is_back) {
        read_page();
        touch_state();
        d->lpSurface = pg.page.data();
        d->lPitch = st.w * 2;
        d->dwWidth = st.w, d->dwHeight = st.h;
        st.page_locked = true;
    } else {
        touch(s);                                // the caller writes its pixels
        d->lpSurface = s->pixels.data();
    }
    d->dwFlags |= DDSD_LPSURFACE;
    return DD_OK;
}

HRESULT unlock(Surface* s) {
    if (s->is_back && st.page_locked) {
        touch_state();
        st.page_locked = false;
        draw_page();
    } else {
        touch(s);
        s->gen++;
    }
    return DD_OK;
}

namespace {
void copy_from(Surface* d, Surface* s, int sx, int sy, int dx, int dy, int cw, int ch) {
    touch(d);
    for (int y = 0; y < ch; y++) {
        int ty = dy + y, fy = sy + y;
        if (ty < 0 || ty >= d->h || fy < 0 || fy >= s->h) continue;
        for (int x = 0; x < cw; x++) {
            int tx = dx + x, fx = sx + x;
            if (tx >= 0 && tx < d->w && fx >= 0 && fx < s->w) d->pixels[ty * d->w + tx] = s->pixels[fy * s->w + fx];
        }
    }
    d->gen++;
}
}  // namespace

HRESULT blt(Surface* dst, LPRECT dr, Surface* s, LPRECT sr) {
    if (!s) { ::com_unsupported("IDirectDrawSurface3::Blt (colour fill)"); return DD_OK; }
    RECT d = dr ? *dr : RECT{0, 0, dst->w, dst->h}, r = sr ? *sr : RECT{0, 0, s->w, s->h};
    int dw = d.right - d.left, dh = d.bottom - d.top, sw = r.right - r.left, sh = r.bottom - r.top;
    if (dw == sw && dh == sh) {
        copy_from(dst, s, r.left, r.top, d.left, d.top, dw, dh);
    } else {                                                      // stretched: nearest, as the cards did
        touch(dst);
        for (int y = 0; y < dh; y++)
            for (int x = 0; x < dw; x++) {
                int fx = r.left + x * sw / dw, fy = r.top + y * sh / dh, tx = d.left + x, ty = d.top + y;
                if (tx >= 0 && tx < dst->w && ty >= 0 && ty < dst->h && fx >= 0 && fx < s->w && fy >= 0 && fy < s->h)
                    dst->pixels[ty * dst->w + tx] = s->pixels[fy * s->w + fx];
            }
        dst->gen++;
    }
    return DD_OK;
}

HRESULT blt_fast(Surface* dst, DWORD x, DWORD y, Surface* s, LPRECT sr) {
    RECT r = sr ? *sr : RECT{0, 0, s->w, s->h};
    copy_from(dst, s, r.left, r.top, (int)x, (int)y, r.right - r.left, r.bottom - r.top);
    return DD_OK;
}

void set_key(Surface* s, const DDCOLORKEY* k) {
    touch(s);
    s->keyed = k != 0;
    s->key = k ? (uint16_t)k->dwColorSpaceLowValue : 0;
    s->gen++;
}

HRESULT attached(Surface* s, const DDSCAPS* c, Surface** out) {
    if ((c->dwCaps & DDSCAPS_BACKBUFFER) && s->back) { *out = s->back; s->back->hold(); return DD_OK; }
    if ((c->dwCaps & DDSCAPS_MIPMAP) && s->next_mip) { *out = s->next_mip; return DD_OK; }
    *out = 0;
    return DDERR_NOTFOUND;
}

HRESULT tex_load(Surface* dst, Surface* from) {  // copy every level, as D3D does
    for (Surface* d = dst; d && from; d = d->next_mip, from = from->next_mip)
        if (d->w == from->w && d->h == from->h) touch(d), d->pixels = from->pixels, d->gen++;
        else copy_from(d, from, 0, 0, 0, 0, d->w, d->h);
    return DD_OK;
}

D3DTEXTUREHANDLE tex_handle(Surface* s) {
    if (!handles.count(s)) touch_handles(), handles.insert(s);
    return (D3DTEXTUREHANDLE)(uintptr_t)s;
}

// ---- DirectDraw -----------------------------------------------------------------------------------------------------
void enum_drivers(LPDDENUMCALLBACKA cb, LPVOID ctx) {
    cb(0, (LPSTR)"OpenGL (viperport)", (LPSTR)"display", ctx);    // the primary display only
}

void dd_caps(LPDDCAPS hal, LPDDCAPS hel) {
    for (LPDDCAPS c : {hal, hel}) {
        if (!c) continue;
        DWORD size = c->dwSize;
        memset(c, 0, size);
        c->dwSize = size;
        c->dwCaps = DDCAPS_3D | DDCAPS_BLT | DDCAPS_BLTSTRETCH | DDCAPS_COLORKEY | DDCAPS_ZBLTS | DDCAPS_BLTCOLORFILL;
        c->dwCKeyCaps = DDCKEYCAPS_SRCBLT;
        c->dwVidMemTotal = c->dwVidMemFree = 64u << 20;
        c->ddsCaps.dwCaps = DDSCAPS_3DDEVICE | DDSCAPS_TEXTURE | DDSCAPS_ZBUFFER | DDSCAPS_FLIP | DDSCAPS_MIPMAP |
                            DDSCAPS_VIDEOMEMORY | DDSCAPS_PRIMARYSURFACE | DDSCAPS_BACKBUFFER;
        c->dwZBufferBitDepths = DDBD_16 | DDBD_24;
    }
}

void vidmem(LPDDSCAPS caps, LPDWORD total, LPDWORD free) {
    // 64 MB of card memory and no AGP: the top tier, and far from the 4 GB wrap that crashed v1.1
    DWORD mem = caps && (caps->dwCaps & DDSCAPS_NONLOCALVIDMEM) ? 0 : 64u << 20;
    if (total) *total = mem;
    if (free) *free = mem;
}

void display_mode(LPDDSURFACEDESC d) {
    DWORD size = d->dwSize ? d->dwSize : sizeof *d;
    memset(d, 0, size);
    d->dwSize = size;
    d->dwFlags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT;
    d->dwWidth = st.w, d->dwHeight = st.h, d->lPitch = st.w * 2;
    pixel_format(F565, d->ddpfPixelFormat);
}

void enum_modes(LPVOID ctx, LPDDENUMMODESCALLBACK cb) {
    static const int modes[][2] = {{512, 384}, {640, 480}, {800, 600}, {1024, 768}};
    for (auto& m : modes) {
        DDSURFACEDESC d = {};
        d.dwSize = sizeof d;
        d.dwFlags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_CAPS;
        d.dwWidth = m[0], d.dwHeight = m[1], d.lPitch = m[0] * 2;
        d.ddsCaps.dwCaps = DDSCAPS_3DDEVICE;
        pixel_format(F565, d.ddpfPixelFormat);
        if (cb(&d, ctx) == DDENUMRET_CANCEL) break;
    }
}

// ---- Direct3D -----------------------------------------------------------------------------------------------------------
HRESULT create_device(Device** out) {
    if (!start()) return DDERR_GENERIC;
    set_defaults();
    *out = make<Device>();
    return DD_OK;
}

void device_caps(LPD3DDEVICEDESC hal, LPD3DDEVICEDESC hel) {
    for (LPD3DDEVICEDESC d : {hal, hel}) {
        if (!d) continue;
        DWORD size = d->dwSize;
        memset(d, 0, size);
        d->dwSize = size;
        d->dwFlags = 0xFFF;                                   // every field valid
        d->dcmColorModel = D3DCOLOR_RGB;
        d->dwDevCaps = D3DDEVCAPS_FLOATTLVERTEX | D3DDEVCAPS_TEXTUREVIDEOMEMORY | D3DDEVCAPS_DRAWPRIMTLVERTEX |
                       D3DDEVCAPS_CANRENDERAFTERFLIP | D3DDEVCAPS_TEXTURENONLOCALVIDMEM;
        d->dtcTransformCaps.dwSize = sizeof(D3DTRANSFORMCAPS);
        d->dtcTransformCaps.dwCaps = D3DTRANSFORMCAPS_CLIP;
        d->bClipping = TRUE;
        d->dlcLightingCaps.dwSize = sizeof(D3DLIGHTINGCAPS);
        for (D3DPRIMCAPS* p : {&d->dpcLineCaps, &d->dpcTriCaps}) {
            p->dwSize = sizeof(D3DPRIMCAPS);
            p->dwMiscCaps = 0xFFFFFFFF;
            p->dwRasterCaps = 0xFFFFFFFF;
            p->dwZCmpCaps = p->dwAlphaCmpCaps = 0xFF;
            p->dwSrcBlendCaps = p->dwDestBlendCaps = 0x1FFF;
            p->dwShadeCaps = 0xFFFFFFFF;
            p->dwTextureCaps = D3DPTEXTURECAPS_PERSPECTIVE | D3DPTEXTURECAPS_POW2 | D3DPTEXTURECAPS_ALPHA |
                               D3DPTEXTURECAPS_TRANSPARENCY;
            p->dwTextureFilterCaps = 0x3F;
            p->dwTextureBlendCaps = 0xFF;
            p->dwTextureAddressCaps = 0x7;
        }
        d->dwDeviceRenderBitDepth = DDBD_16 | DDBD_32;
        d->dwDeviceZBufferBitDepth = DDBD_16 | DDBD_24;
        d->dwMaxBufferSize = 0;
        d->dwMaxVertexCount = 65535;
        if (size >= sizeof(D3DDEVICEDESC)) {
            d->dwMinTextureWidth = d->dwMinTextureHeight = 1;
            d->dwMaxTextureWidth = d->dwMaxTextureHeight = 2048;
        }
    }
}

void stats(LPD3DSTATS s) {
    DWORD n = s->dwSize;
    memset(s, 0, n);
    s->dwSize = n;
}

void enum_formats(LPD3DENUMTEXTUREFORMATSCALLBACK cb, LPVOID ctx) {
    for (Format f : {F565, F1555, F4444}) {
        DDSURFACEDESC d = {};
        d.dwSize = sizeof d;
        d.dwFlags = DDSD_CAPS | DDSD_PIXELFORMAT;
        d.ddsCaps.dwCaps = DDSCAPS_TEXTURE;
        pixel_format(f, d.ddpfPixelFormat);
        if (cb(&d, ctx) == D3DENUMRET_CANCEL) break;
    }
}

void set_state(DWORD s, DWORD v) {
    if (s >= 64) return;
    touch_state();
    st.rs[s] = v;
    if (s == D3DRENDERSTATE_TEXTUREADDRESS) st.rs[D3DRENDERSTATE_TEXTUREADDRESSU] = st.rs[D3DRENDERSTATE_TEXTUREADDRESSV] = v;
}
DWORD get_state(DWORD s) { return s < 64 ? st.rs[s] : 0; }

void set_transform(DWORD t, const D3DMATRIX* m) {
    touch_state();
    if (t == D3DTRANSFORMSTATE_WORLD) st.world = *m;
    else if (t == D3DTRANSFORMSTATE_VIEW) st.view = *m;
    else if (t == D3DTRANSFORMSTATE_PROJECTION) st.proj = *m;
}
void get_transform(DWORD t, D3DMATRIX* m) {
    *m = t == D3DTRANSFORMSTATE_WORLD ? st.world : t == D3DTRANSFORMSTATE_VIEW ? st.view : st.proj;
}

void remove_viewport(Device* d, Viewport* v) {
    if (v == d->current) touch(d), d->current = 0;
}
void use_viewport(Device* d, Viewport* v) {
    touch(d);
    d->current = v;
    if (v) touch_state(), st.vp = v->vp;
}
void set_viewport(Viewport* v, const D3DVIEWPORT2* vp) {
    touch(v);
    touch_state();
    v->vp = *vp;
    st.vp = *vp;
}
void set_background(Viewport* v, D3DMATERIALHANDLE h) {
    touch(v);
    v->background = (Material*)(uintptr_t)h;
}
void set_material(Material* m, const D3DMATERIAL* p) {
    touch(m);
    m->m = *p;
}
D3DMATERIALHANDLE material_handle(Material* m) { return (D3DMATERIALHANDLE)(uintptr_t)m; }

void clear(Viewport* v, DWORD n, const D3DRECT* rects, DWORD flags) {
    if (!on_gl_thread()) return;
    glr::BindFramebuffer(GL_FRAMEBUFFER, st.fbo);
    glr::Enable(GL_SCISSOR_TEST);
    glr::DepthMask(GL_TRUE);
    GLbitfield bits = 0;
    if (flags & D3DCLEAR_TARGET) {
        bits |= GL_COLOR_BUFFER_BIT;
        D3DCOLORVALUE c = v->background ? v->background->m.diffuse : D3DCOLORVALUE{0, 0, 0, 1};
        glr::ClearColor(c.r, c.g, c.b, 1.0f);
    }
    if (flags & D3DCLEAR_ZBUFFER) bits |= GL_DEPTH_BUFFER_BIT, glr::ClearDepth(1.0);
    for (DWORD i = 0; i < n; i++) {
        Rect r = map_rect(rects[i].x1, rects[i].y1, rects[i].x2 - rects[i].x1, rects[i].y2 - rects[i].y1);
        glr::Scissor(r.x, r.y, r.w, r.h);
        glr::Clear(bits);
    }
}

namespace {
void mat_mul(const D3DMATRIX& a, const D3DMATRIX& b, float* out) {     // row-major a x b
    const float* x = &a._11;
    const float* y = &b._11;
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            out[r * 4 + c] = x[r * 4] * y[c] + x[r * 4 + 1] * y[4 + c] + x[r * 4 + 2] * y[8 + c] + x[r * 4 + 3] * y[12 + c];
}
}  // namespace

void transform(Viewport* v, DWORD n, LPD3DTRANSFORMDATA d, DWORD flags, LPDWORD offscreen) {
    // on the CPU, as D3D did: W x V x P, then the viewport mapping; clip codes for anything outside
    const D3DVIEWPORT2& vp = v->vp;
    float m[16], wv[16];
    D3DMATRIX t;
    mat_mul(st.world, st.view, wv);
    memcpy(&t, wv, sizeof t);
    mat_mul(t, st.proj, m);
    float cw[4];
    clip_window(vp, cw);                           // as the target draws it (widened when full width)
    Rect vr = map_rect(vp.dwX, vp.dwY, vp.dwWidth, vp.dwHeight);
    DWORD all = ~0u, any = 0;
    for (DWORD i = 0; i < n; i++) {
        const float* src = (const float*)((const char*)d->lpIn + i * d->dwInSize);
        float p[4];
        for (int c = 0; c < 4; c++) p[c] = src[0] * m[c] + src[1] * m[4 + c] + src[2] * m[8 + c] + m[12 + c];
        DWORD code = 0;
        if (p[0] < cw[0] * p[3]) code |= D3DCLIP_LEFT;
        if (p[0] > (cw[0] + cw[2]) * p[3]) code |= D3DCLIP_RIGHT;
        if (p[1] > vp.dvClipY * p[3]) code |= D3DCLIP_TOP;
        if (p[1] < (vp.dvClipY - vp.dvClipHeight) * p[3]) code |= D3DCLIP_BOTTOM;
        if (p[2] < 0) code |= D3DCLIP_FRONT;
        if (p[2] > p[3]) code |= D3DCLIP_BACK;
        all &= code, any |= code;
        float rw = p[3] != 0 ? 1.0f / p[3] : 0.0f;
        D3DTLVERTEX* out = (D3DTLVERTEX*)((char*)d->lpOut + i * d->dwOutSize);
        float tx = vr.x + vr.w * (p[0] * rw - cw[0]) / cw[2];          // target pixels
        out->sx = (tx - st.ox) / st.scale;                               // back to the game's
        out->sy = vp.dwY + vp.dwHeight * (vp.dvClipY - p[1] * rw) / vp.dvClipHeight;
        out->sz = p[2] * rw;
        out->rhw = rw;
        if (d->lpHOut) {
            D3DHVERTEX* h = (D3DHVERTEX*)d->lpHOut + i;
            h->dwFlags = code, h->hx = p[0], h->hy = p[1], h->hz = p[2];
        }
    }
    d->dwClipIntersection = all, d->dwClipUnion = any;
    if (offscreen) *offscreen = (flags & D3DTRANSFORM_CLIPPED) ? all : 0;
}

// ---- drawing --------------------------------------------------------------------------------------------------------------
namespace {

GLenum blend_factor(DWORD b) {
    switch (b) {
    case D3DBLEND_ZERO: return GL_ZERO;
    case D3DBLEND_ONE: return GL_ONE;
    case D3DBLEND_SRCCOLOR: return GL_SRC_COLOR;
    case D3DBLEND_INVSRCCOLOR: return GL_ONE_MINUS_SRC_COLOR;
    case D3DBLEND_SRCALPHA: return GL_SRC_ALPHA;
    case D3DBLEND_INVSRCALPHA: return GL_ONE_MINUS_SRC_ALPHA;
    case D3DBLEND_DESTALPHA: return GL_DST_ALPHA;
    case D3DBLEND_INVDESTALPHA: return GL_ONE_MINUS_DST_ALPHA;
    case D3DBLEND_DESTCOLOR: return GL_DST_COLOR;
    case D3DBLEND_INVDESTCOLOR: return GL_ONE_MINUS_DST_COLOR;
    case D3DBLEND_SRCALPHASAT: return GL_SRC_ALPHA_SATURATE;
    }
    return GL_ONE;
}

GLenum depth_func(DWORD f) {
    static const GLenum m[] = {GL_ALWAYS, GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS};
    return f < 9 ? m[f] : GL_LEQUAL;
}

GLint min_filter(DWORD f, bool mips) {
    switch (f) {
    case D3DFILTER_LINEAR: return GL_LINEAR;
    case D3DFILTER_MIPNEAREST: return mips ? GL_NEAREST_MIPMAP_NEAREST : GL_NEAREST;
    case D3DFILTER_MIPLINEAR: return mips ? GL_LINEAR_MIPMAP_NEAREST : GL_LINEAR;
    case D3DFILTER_LINEARMIPNEAREST: return mips ? GL_NEAREST_MIPMAP_LINEAR : GL_NEAREST;
    case D3DFILTER_LINEARMIPLINEAR: return mips ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR;
    }
    return GL_NEAREST;
}

// a texture's levels as a GL texture, re-uploaded when any level changed since
GLuint texture_for(Surface* root) {
    int n = 0;
    for (Surface* l = root; l; l = l->next_mip) n++;
    bool stale = !root->tex || (int)root->uploaded.size() != n;
    int i = 0;
    for (Surface* l = root; l && !stale; l = l->next_mip, i++) stale = root->uploaded[i] != l->gen;
    if (stale) touch(root);
    bool fresh = !root->tex;
    if (fresh) glr::GenTextures(1, &root->tex);
    glr::BindTexture(GL_TEXTURE_2D, root->tex);
    if (fresh) glr::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, n - 1);
    root->uploaded.resize(n, 0);
    i = 0;
    std::vector<uint32_t> rgba;
    glr::PixelStorei(GL_UNPACK_ALIGNMENT, 2);
    for (Surface* l = root; l; l = l->next_mip, i++) {
        if (root->uploaded[i] == l->gen) continue;
        root->uploaded[i] = l->gen;
        const uint16_t* p = l->pixels.data();
        if (root->keyed && (l->fmt == F565 || l->fmt == F555)) {
            // the colour key becomes alpha 0 (the shader discards it); the texel's own colour otherwise
            rgba.resize((size_t)l->w * l->h);
            for (size_t k = 0; k < rgba.size(); k++) {
                uint16_t c = p[k];
                if (c == root->key) { rgba[k] = 0; continue; }
                uint32_t r, gg, b;
                if (l->fmt == F565) r = (c >> 11) & 31, gg = (c >> 5) & 63, b = c & 31, gg = gg * 255 / 63;
                else r = (c >> 10) & 31, gg = (c >> 5) & 31, b = c & 31, gg = gg * 255 / 31;
                rgba[k] = 0xFF000000u | ((b * 255 / 31) << 16) | (gg << 8) | (r * 255 / 31);
            }
            glr::TexImage2D(GL_TEXTURE_2D, i, GL_RGBA8, l->w, l->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        } else if (l->fmt == F4444) {
            glr::TexImage2D(GL_TEXTURE_2D, i, GL_RGBA8, l->w, l->h, 0, GL_BGRA, GL_UNSIGNED_SHORT_4_4_4_4_REV, p);
        } else if (l->fmt == F1555 || l->fmt == F555) {
            glr::TexImage2D(GL_TEXTURE_2D, i, GL_RGBA8, l->w, l->h, 0, GL_BGRA, GL_UNSIGNED_SHORT_1_5_5_5_REV, p);
        } else {
            glr::TexImage2D(GL_TEXTURE_2D, i, GL_RGB8, l->w, l->h, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, p);
        }
    }
    return root->tex;
}

// the vertices as a check records them: without what Direct3D never reads -- an LVERTEX's reserved dword, and u/v with
// no texture bound -- which the game leaves as stack garbage (its triangles are built in locals), different in the two
// passes. Only during a check; the GPU gets the same image either way.
const void* for_check(D3DVERTEXTYPE vt, const void* v, DWORD n, bool textured, std::vector<uint8_t>& buf) {
    if (!shadow_com_phase() || !v) return v;
    bool lv = vt == D3DVT_LVERTEX;
    if (!lv && textured) return v;
    buf.assign((const uint8_t*)v, (const uint8_t*)v + (size_t)n * 32);
    for (size_t i = 0; i < n; i++) {
        uint8_t* p = &buf[i * 32];
        if (lv) memset(p + 12, 0, 4);
        if (!textured) memset(p + 24, 0, 8);
    }
    return buf.data();
}

}  // namespace

HRESULT draw(D3DPRIMITIVETYPE pt, D3DVERTEXTYPE vtype, const void* verts, DWORD nverts, const WORD* idx, DWORD nidx) {
    if (pt != D3DPT_TRIANGLELIST) {
        ::com_unsupported(idx ? "DrawIndexedPrimitive (not a triangle list)" : "DrawPrimitive (not a triangle list)");
        return DD_OK;
    }
    if (!on_gl_thread()) return DD_OK;
    if (vtype != D3DVT_LVERTEX && vtype != D3DVT_TLVERTEX) { ::com_unsupported("DrawPrimitive with D3DVT_VERTEX"); return DD_OK; }
    const DWORD* rs = st.rs;
    Program& pr = rs[D3DRENDERSTATE_SHADEMODE] == D3DSHADE_FLAT ? in.flat : in.smooth;
    glr::BindFramebuffer(GL_FRAMEBUFFER, st.fbo);
    glr::UseProgram(pr.id);
    glr::BindVertexArray(in.vao);
    bool tl = vtype == D3DVT_TLVERTEX;
    // viewport: D3D maps into the viewport rect and clips to it, TL vertices included
    Rect r = map_rect(st.vp.dwX, st.vp.dwY, st.vp.dwWidth, st.vp.dwHeight);
    if (tl) glr::Viewport(0, 0, st.rt_w, st.rt_h);
    else glr::Viewport(r.x, r.y, r.w, r.h);
    glr::Enable(GL_SCISSOR_TEST);
    glr::Scissor(r.x, r.y, r.w, r.h);
    float clip[4];
    clip_window(st.vp, clip);
    glr::Uniform1i(pr.uTL, tl);
    glr::Uniform2f(pr.uRT, (float)st.rt_w, (float)st.rt_h);
    glr::Uniform2f(pr.uMap, st.scale, st.ox);
    glr::Uniform4f(pr.uClip, clip[0], clip[1], clip[2], clip[3]);
    float wvp[16], wv[16];
    D3DMATRIX t;
    mat_mul(st.world, st.view, wv);
    memcpy(&t, wv, sizeof t);
    mat_mul(t, st.proj, wvp);
    glr::UniformMatrix4fv(pr.uM, 1, GL_FALSE, wvp);
    // render states
    if (rs[D3DRENDERSTATE_ALPHABLENDENABLE]) {
        glr::Enable(GL_BLEND);
        glr::BlendFunc(blend_factor(rs[D3DRENDERSTATE_SRCBLEND]), blend_factor(rs[D3DRENDERSTATE_DESTBLEND]));
    } else {
        glr::Disable(GL_BLEND);
    }
    if (rs[D3DRENDERSTATE_ZENABLE]) glr::Enable(GL_DEPTH_TEST), glr::DepthFunc(depth_func(rs[D3DRENDERSTATE_ZFUNC]));
    else glr::Disable(GL_DEPTH_TEST);
    glr::DepthMask(rs[D3DRENDERSTATE_ZWRITEENABLE] ? GL_TRUE : GL_FALSE);
    // D3D fronts are clockwise on screen; the target's rows run top-down, so on GL's y-up window they're CCW
    glr::FrontFace(GL_CCW);
    if (rs[D3DRENDERSTATE_CULLMODE] == D3DCULL_NONE) glr::Disable(GL_CULL_FACE);
    else glr::Enable(GL_CULL_FACE), glr::CullFace(rs[D3DRENDERSTATE_CULLMODE] == D3DCULL_CW ? GL_FRONT : GL_BACK);
    DWORD blend = rs[D3DRENDERSTATE_TEXTUREMAPBLEND];
    glr::Uniform1i(pr.uBlend, blend == D3DTBLEND_DECAL || blend == D3DTBLEND_COPY ? 1 : blend == D3DTBLEND_MODULATEALPHA ? 4 : 2);
    glr::Uniform1i(pr.uSpec, rs[D3DRENDERSTATE_SPECULARENABLE] ? 1 : 0);
    glr::Uniform1i(pr.uFog, rs[D3DRENDERSTATE_FOGENABLE] ? 1 : 0);
    DWORD fc = rs[D3DRENDERSTATE_FOGCOLOR];
    glr::Uniform3f(pr.uFogCol, ((fc >> 16) & 255) / 255.0f, ((fc >> 8) & 255) / 255.0f, (fc & 255) / 255.0f);
    glr::Uniform1i(pr.uAlphaFunc, rs[D3DRENDERSTATE_ALPHATESTENABLE] ? (int)rs[D3DRENDERSTATE_ALPHAFUNC] : 8);
    glr::Uniform1f(pr.uAlphaRef, (float)(rs[D3DRENDERSTATE_ALPHAREF] & 0xff));
    // texture
    Surface* tex = (Surface*)(uintptr_t)rs[D3DRENDERSTATE_TEXTUREHANDLE];
    bool textured = tex && handles.count(tex);
    if (textured) {
        glr::ActiveTexture(GL_TEXTURE0);
        texture_for(tex);
        bool mips = tex->next_mip != 0;
        glr::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, min_filter(rs[D3DRENDERSTATE_TEXTUREMIN], mips));
        glr::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, rs[D3DRENDERSTATE_TEXTUREMAG] == D3DFILTER_NEAREST ? GL_NEAREST : GL_LINEAR);
        glr::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, rs[D3DRENDERSTATE_TEXTUREADDRESSU] == D3DTADDRESS_CLAMP ? GL_CLAMP_TO_EDGE
                           : rs[D3DRENDERSTATE_TEXTUREADDRESSU] == D3DTADDRESS_MIRROR ? GL_MIRRORED_REPEAT : GL_REPEAT);
        glr::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, rs[D3DRENDERSTATE_TEXTUREADDRESSV] == D3DTADDRESS_CLAMP ? GL_CLAMP_TO_EDGE
                           : rs[D3DRENDERSTATE_TEXTUREADDRESSV] == D3DTADDRESS_MIRROR ? GL_MIRRORED_REPEAT : GL_REPEAT);
        float bias;
        memcpy(&bias, &rs[D3DRENDERSTATE_MIPMAPLODBIAS], 4);
        glr::TexParameterf(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, bias);
        glr::Uniform1i(pr.uTex, 0);
        glr::Uniform1i(pr.uTexOn, 1);
        glr::Uniform1i(pr.uTexAlpha, tex->fmt == F4444 || tex->fmt == F1555);
        glr::Uniform1i(pr.uKeyed, rs[D3DRENDERSTATE_COLORKEYENABLE] && tex->keyed ? 1 : 0);
    } else {
        glr::Uniform1i(pr.uTexOn, 0);
        glr::Uniform1i(pr.uKeyed, 0);
    }
    std::vector<uint8_t> buf;
    glr::BindBuffer(GL_ARRAY_BUFFER, in.vbo);
    glr::BufferData(GL_ARRAY_BUFFER, (ptrdiff_t)nverts * 32, for_check(vtype, verts, nverts, textured, buf), GL_STREAM_DRAW);
    touch_state();
    if (idx) {
        glr::BindBuffer(GL_ELEMENT_ARRAY_BUFFER, in.ibo);
        glr::BufferData(GL_ELEMENT_ARRAY_BUFFER, (ptrdiff_t)nidx * 2, idx, GL_STREAM_DRAW);
        glr::DrawElements(GL_TRIANGLES, nidx, GL_UNSIGNED_SHORT, 0);
        st.triangles += nidx / 3;
    } else {
        glr::DrawArrays(GL_TRIANGLES, 0, nverts);
        st.triangles += nverts / 3;
    }
    st.draws++;
    return DD_OK;
}

void report() {
    if (st.frames) logf("exit: renderer: %llu frames, %llu draw calls, %llu triangles", st.frames, st.draws, st.triangles);
}

// ---- for the harnesses ---------------------------------------------------------------------------------------------------
// test/world_gx_dd.cpp: the renderer started without a window or an OpenGL context, on gl_api as the harness filled it
// (fakes), on the calling thread: what start() leaves ready (render states, the display mode's page and target), minus
// the shader programs, vertex arrays and page texture, whose names stay 0. Never called by the DLL.
bool start_headless() {
    if (in.ready) return true;
    in.thread = GetCurrentThreadId();
    in.ready = true;
    set_defaults();
    set_mode(st.w, st.h);
    return true;
}

}  // namespace gfx

void gfx_repaint() { gfx::repaint(); }
